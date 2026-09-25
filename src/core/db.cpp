// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — SQLite tracking database implementation.
#include "db.h"

#include "ranarch/error.h"

#include <sqlite3.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ranarch {

namespace {

[[noreturn]] void throw_db(RanArchError code, const std::string& what) {
    throw Exception(code, what);
}

// Convert a sqlite return code into a thrown exception unless it is one of
// the "success" codes (OK / DONE / ROW).
void check_rc(int rc, sqlite3* db, RanArchError code, const char* context) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        std::string msg = context;
        msg += ": ";
        msg += sqlite3_errmsg(db);
        throw_db(code, msg);
    }
}

// RAII prepared-statement wrapper. Throws on prepare failure.
struct Stmt {
    sqlite3_stmt* p  = nullptr;
    sqlite3*      db = nullptr;

    Stmt(sqlite3* d, const char* sql) : db(d) {
        if (sqlite3_prepare_v2(db, sql, -1, &p, nullptr) != SQLITE_OK) {
            throw Exception(RanArchError::DbCorrupt,
                            std::string("prepare: ") + sqlite3_errmsg(db));
        }
    }
    ~Stmt() { if (p) sqlite3_finalize(p); }
    Stmt(const Stmt&)            = delete;
    Stmt& operator=(const Stmt&) = delete;
    operator sqlite3_stmt*() const { return p; }
};

int64_t now_unix() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

PackageRecord read_package(sqlite3_stmt* s) {
    PackageRecord r;
    r.id            = sqlite3_column_int64(s, 0);
    if (auto v = sqlite3_column_text(s, 1)) r.name        = reinterpret_cast<const char*>(v);
    if (auto v = sqlite3_column_text(s, 2)) r.version     = reinterpret_cast<const char*>(v);
    if (auto v = sqlite3_column_text(s, 3)) r.source_format = reinterpret_cast<const char*>(v);
    if (auto v = sqlite3_column_text(s, 4)) r.original_file = reinterpret_cast<const char*>(v);
    if (auto v = sqlite3_column_text(s, 5)) r.description = reinterpret_cast<const char*>(v);
    if (auto v = sqlite3_column_text(s, 6)) r.signature_status = reinterpret_cast<const char*>(v);
    r.install_date  = sqlite3_column_int64(s, 7);
    return r;
}

} // namespace

const char* signature_status_string(SignatureStatus s) {
    switch (s) {
        case SignatureStatus::SignedOk: return "signed_ok";
        case SignatureStatus::Unsigned:  return "unsigned";
        case SignatureStatus::Bad:       return "bad";
        case SignatureStatus::Skipped:   return "skipped";
    }
    return "skipped";
}

struct Database::Impl {
    sqlite3*    db = nullptr;
    std::mutex  mu;
};

Database::Database()  : m_impl(std::make_unique<Impl>()) {}
Database::~Database() { close(); }

void Database::open(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (m_impl->db) {
        sqlite3_close_v2(m_impl->db);
        m_impl->db = nullptr;
    }
    int rc = sqlite3_open_v2(path.c_str(), &m_impl->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = m_impl->db ? sqlite3_errmsg(m_impl->db) : "open failed";
        sqlite3_close_v2(m_impl->db);
        m_impl->db = nullptr;
        throw Exception(RanArchError::Io, std::string("db open: ") + err);
    }
    // Wait up to 5s when the database is locked by another writer.
    sqlite3_busy_timeout(m_impl->db, 5000);
    ensure_schema();
}

bool Database::is_open() const noexcept { return m_impl->db != nullptr; }

void Database::close() {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (m_impl->db) {
        sqlite3_close_v2(m_impl->db);
        m_impl->db = nullptr;
    }
}

void Database::exec(const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(m_impl->db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "exec failed";
        sqlite3_free(err);
        throw Exception(RanArchError::DbCorrupt, msg);
    }
}

void Database::ensure_schema() {
    exec("PRAGMA foreign_keys = ON;");
    exec(
        "CREATE TABLE IF NOT EXISTS packages ("
        "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name            TEXT NOT NULL,"
        "  version         TEXT NOT NULL,"
        "  source_format   TEXT NOT NULL,"
        "  original_file   TEXT,"
        "  description     TEXT,"
        "  signature_status TEXT,"
        "  install_date    INTEGER NOT NULL"
        ");"
    );
    exec(
        "CREATE TABLE IF NOT EXISTS files ("
        "  package_id INTEGER NOT NULL,"
        "  path       TEXT NOT NULL,"
        "  mode       INTEGER,"
        "  size       INTEGER,"
        "  sha256     TEXT,"
        "  PRIMARY KEY (package_id, path),"
        "  FOREIGN KEY (package_id) REFERENCES packages(id) ON DELETE CASCADE"
        ");"
    );
    exec(
        "CREATE TABLE IF NOT EXISTS dep_overrides ("
        "  source_format TEXT NOT NULL,"
        "  source_name   TEXT NOT NULL,"
        "  arch_name     TEXT NOT NULL,"
        "  PRIMARY KEY (source_format, source_name)"
        ");"
    );
    exec(
        "CREATE TABLE IF NOT EXISTS trusted_keys ("
        "  fingerprint TEXT PRIMARY KEY,"
        "  added_date  INTEGER NOT NULL,"
        "  description TEXT"
        ");"
    );
    exec("CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);");
}

int64_t Database::add_package(const PackageMeta& meta,
                              const std::vector<FileEntry>& files,
                              const std::string& signature_status,
                              const std::string& original_file) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (!m_impl->db) throw_db(RanArchError::DbCorrupt, "db not open");

    const char* fmt = meta.format == PackageFormat::Deb ? "deb"
                    : meta.format == PackageFormat::Rpm ? "rpm"
                                                        : "unknown";

    exec("BEGIN TRANSACTION;");
    try {
        Stmt s(m_impl->db,
                "INSERT INTO packages(name, version, source_format, original_file, "
                "description, signature_status, install_date) VALUES(?,?,?,?,?,?,?);");
        sqlite3_bind_text(s, 1, meta.name.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, meta.version.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, fmt,                       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 4, original_file.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 5, meta.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 6, signature_status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 7, now_unix());
        check_rc(sqlite3_step(s), m_impl->db, RanArchError::DbCorrupt, "insert package");

        int64_t id = sqlite3_last_insert_rowid(m_impl->db);

        Stmt fs(m_impl->db,
                "INSERT INTO files(package_id, path, mode, size, sha256) VALUES(?,?,?,?,?);");
        for (const auto& f : files) {
            sqlite3_bind_int64(fs, 1, id);
            sqlite3_bind_text (fs, 2, f.path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int  (fs, 3, static_cast<int>(f.mode));
            sqlite3_bind_int64(fs, 4, static_cast<int64_t>(f.size));
            sqlite3_bind_text (fs, 5, "", -1, SQLITE_TRANSIENT); // sha256 not yet computed
            check_rc(sqlite3_step(fs), m_impl->db, RanArchError::DbCorrupt, "insert file");
            sqlite3_reset(fs);
            sqlite3_clear_bindings(fs);
        }
        exec("COMMIT;");
        return id;
    } catch (...) {
        // Roll back whatever partial work landed and re-throw.
        exec("ROLLBACK;");
        throw;
    }
}

std::vector<PackageRecord> Database::list_packages() {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (!m_impl->db) throw_db(RanArchError::DbCorrupt, "db not open");
    Stmt s(m_impl->db,
           "SELECT id, name, version, source_format, original_file, "
           "description, signature_status, install_date "
           "FROM packages ORDER BY install_date DESC;");
    std::vector<PackageRecord> out;
    while (sqlite3_step(s) == SQLITE_ROW) out.push_back(read_package(s));
    return out;
}

std::optional<PackageRecord> Database::find_by_name(const std::string& name) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (!m_impl->db) throw_db(RanArchError::DbCorrupt, "db not open");
    Stmt s(m_impl->db,
           "SELECT id, name, version, source_format, original_file, "
           "description, signature_status, install_date "
           "FROM packages WHERE name=? ORDER BY install_date DESC LIMIT 1;");
    sqlite3_bind_text(s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) == SQLITE_ROW) return read_package(s);
    return std::nullopt;
}

std::optional<PackageRecord> Database::find_owner_of(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (!m_impl->db) throw_db(RanArchError::DbCorrupt, "db not open");
    Stmt s(m_impl->db,
           "SELECT p.id, p.name, p.version, p.source_format, p.original_file, "
           "p.description, p.signature_status, p.install_date "
           "FROM files f JOIN packages p ON p.id = f.package_id "
           "WHERE f.path=? ORDER BY p.install_date DESC, p.id DESC LIMIT 1;");
    sqlite3_bind_text(s, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) == SQLITE_ROW) return read_package(s);
    return std::nullopt;
}

std::vector<FileRecord> Database::list_files(int64_t package_id) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (!m_impl->db) throw_db(RanArchError::DbCorrupt, "db not open");
    Stmt s(m_impl->db,
           "SELECT package_id, path, mode, size, sha256 "
           "FROM files WHERE package_id=? ORDER BY path;");
    sqlite3_bind_int64(s, 1, package_id);
    std::vector<FileRecord> out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        FileRecord r;
        r.package_id = sqlite3_column_int64(s, 0);
        if (auto v = sqlite3_column_text(s, 1)) r.path  = reinterpret_cast<const char*>(v);
        r.mode      = static_cast<uint32_t>(sqlite3_column_int(s, 2));
        r.size      = static_cast<uint64_t>(sqlite3_column_int64(s, 3));
        if (auto v = sqlite3_column_text(s, 4)) r.sha256 = reinterpret_cast<const char*>(v);
        out.push_back(std::move(r));
    }
    return out;
}

void Database::remove_package(int64_t id) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (!m_impl->db) throw_db(RanArchError::DbCorrupt, "db not open");
    // foreign_keys=ON cascades the file rows.
    Stmt s(m_impl->db, "DELETE FROM packages WHERE id=?;");
    sqlite3_bind_int64(s, 1, id);
    (void)sqlite3_step(s);
}

std::optional<std::string> Database::get_override(const std::string& source_format,
                                                  const std::string& source_name) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (!m_impl->db) throw_db(RanArchError::DbCorrupt, "db not open");
    Stmt s(m_impl->db,
           "SELECT arch_name FROM dep_overrides "
           "WHERE source_format=? AND source_name=?;");
    sqlite3_bind_text(s, 1, source_format.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, source_name.c_str(),   -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) == SQLITE_ROW) {
        if (auto v = sqlite3_column_text(s, 0))
            return std::string(reinterpret_cast<const char*>(v));
    }
    return std::nullopt;
}

void Database::set_override(const std::string& source_format,
                            const std::string& source_name,
                            const std::string& arch_name) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (!m_impl->db) throw_db(RanArchError::DbCorrupt, "db not open");
    Stmt s(m_impl->db,
           "INSERT INTO dep_overrides(source_format, source_name, arch_name) "
           "VALUES(?,?,?) ON CONFLICT(source_format, source_name) "
           "DO UPDATE SET arch_name = excluded.arch_name;");
    sqlite3_bind_text(s, 1, source_format.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, source_name.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, arch_name.c_str(),     -1, SQLITE_TRANSIENT);
    check_rc(sqlite3_step(s), m_impl->db, RanArchError::DbCorrupt, "set_override");
}

void Database::add_key(const std::string& fingerprint,
                       const std::string& description) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (!m_impl->db) throw_db(RanArchError::DbCorrupt, "db not open");
    Stmt s(m_impl->db,
           "INSERT INTO trusted_keys(fingerprint, added_date, description) "
           "VALUES(?,?,?) ON CONFLICT(fingerprint) "
           "DO UPDATE SET description = excluded.description;");
    sqlite3_bind_text (s, 1, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, now_unix());
    sqlite3_bind_text (s, 3, description.c_str(),  -1, SQLITE_TRANSIENT);
    check_rc(sqlite3_step(s), m_impl->db, RanArchError::DbCorrupt, "add_key");
}

std::vector<KeyRecord> Database::list_keys() {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (!m_impl->db) throw_db(RanArchError::DbCorrupt, "db not open");
    Stmt s(m_impl->db,
           "SELECT fingerprint, added_date, description "
           "FROM trusted_keys ORDER BY added_date DESC;");
    std::vector<KeyRecord> out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        KeyRecord r;
        if (auto v = sqlite3_column_text(s, 0)) r.fingerprint = reinterpret_cast<const char*>(v);
        r.added_date = sqlite3_column_int64(s, 1);
        if (auto v = sqlite3_column_text(s, 2)) r.description = reinterpret_cast<const char*>(v);
        out.push_back(std::move(r));
    }
    return out;
}

bool Database::has_key(const std::string& fingerprint) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (!m_impl->db) throw_db(RanArchError::DbCorrupt, "db not open");
    Stmt s(m_impl->db, "SELECT 1 FROM trusted_keys WHERE fingerprint=?;");
    sqlite3_bind_text(s, 1, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(s) == SQLITE_ROW;
}

} // namespace ranarch
