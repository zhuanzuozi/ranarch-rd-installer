// SPDX-License-Identifier: MIT
// RanArch RD Installer — SQLite-backed tracking database.
//
// Owns the package / file / dep-override / trusted-key tables. All access is
// serialised through an internal mutex so a single Database instance is safe
// to share across the daemon's threads. The schema matches the plan:
//   packages(id PK, name, version, source_format, original_file,
//           description, signature_status, install_date)
//   files(package_id, path, mode, size, sha256; PK(package_id,path); FK cascade)
//   dep_overrides(source_format, source_name, arch_name; PK(fmt,name))
//   trusted_keys(fingerprint PK, added_date, description)
//   idx_files_path ON files(path)
#pragma once

#include "ranarch/types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ranarch {

// One row from the `packages` table.
struct PackageRecord {
    int64_t     id = 0;
    std::string name;
    std::string version;
    std::string source_format;     // "deb" | "rpm"
    std::string original_file;
    std::string description;
    std::string signature_status;  // signature_status_string(SignatureStatus)
    int64_t     install_date = 0;  // unix epoch seconds
};

// One row from the `files` table.
struct FileRecord {
    int64_t     package_id = 0;
    std::string path;
    uint32_t    mode = 0;
    uint64_t    size = 0;
    std::string sha256;
};

// One row from the `trusted_keys` table.
struct KeyRecord {
    std::string fingerprint;
    int64_t     added_date = 0;
    std::string description;
};

// String form stored in the `signature_status` column. Stable — do not change.
const char* signature_status_string(SignatureStatus s);

class Database {
public:
    Database();
    ~Database();

    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    // Open (and, on first run, initialise the schema of) `path`. Pass
    // ":memory:" for an in-memory database (used by tests). Throws
    // RanArchError::Io on open failure.
    void open(const std::string& path);

    bool is_open() const noexcept;
    void close();

    // --- packages -------------------------------------------------------
    // Insert a freshly installed package + its file list. Returns the new id.
    // `original_file` is the path the .deb/.rpm was installed from.
    int64_t add_package(const PackageMeta& meta,
                        const std::vector<FileEntry>& files,
                        const std::string& signature_status,
                        const std::string& original_file);

    std::vector<PackageRecord>        list_packages();
    std::optional<PackageRecord>      find_by_name(const std::string& name);
    std::optional<PackageRecord>      find_owner_of(const std::string& path);
    std::vector<FileRecord>           list_files(int64_t package_id);
    void                              remove_package(int64_t id);

    // --- dep overrides --------------------------------------------------
    std::optional<std::string> get_override(const std::string& source_format,
                                             const std::string& source_name);
    void set_override(const std::string& source_format,
                      const std::string& source_name,
                      const std::string& arch_name);

    // --- trusted keys ---------------------------------------------------
    void                       add_key(const std::string& fingerprint,
                                      const std::string& description);
    std::vector<KeyRecord>     list_keys();
    bool                       has_key(const std::string& fingerprint);

private:
    void ensure_schema();
    void exec(const char* sql);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ranarch
