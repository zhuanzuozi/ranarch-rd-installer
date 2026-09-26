// SPDX-License-Identifier: MIT
// RanArch RD Installer — trusted GPG keyring implementation.
#include "keyring.h"

#include "ranarch/error.h"

#include <gpgme.h>

#include <dirent.h>
#include <ftw.h>
#include <string>
#include <sys/stat.h>

namespace ranarch {

namespace {

// Recursively remove a directory tree (used for temp GnuPG home cleanup).
int rm_cb(const char* path, const struct stat*, int, struct FTW*) {
    return ::remove(path);
}

void remove_tree(const std::string& dir) {
    if (!dir.empty())
        nftw(dir.c_str(), rm_cb, 64, FTW_DEPTH | FTW_PHYS);
}

std::string make_temp_home() {
    char tmpl[] = "/tmp/ranarch-gpg-XXXXXX";
    char* d = mkdtemp(tmpl);
    if (!d) throw Exception(RanArchError::Io, "mkdtemp failed for GnuPG home");
    return d;
}

// Create a gpgme context using the given home directory.
gpgme_ctx_t create_ctx(const std::string& home) {
    static bool gpgme_init = false;
    if (!gpgme_init) {
        gpgme_check_version(nullptr);
        gpgme_set_locale(nullptr, LC_CTYPE, setlocale(LC_CTYPE, nullptr));
        gpgme_init = true;
    }
    gpgme_ctx_t ctx = nullptr;
    gpgme_error_t err = gpgme_new(&ctx);
    if (gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
        throw Exception(RanArchError::Internal,
                        std::string("gpgme_new: ") + gpgme_strerror(err));
    }
    gpgme_set_protocol(ctx, GPGME_PROTOCOL_OpenPGP);
    gpgme_ctx_set_engine_info(ctx, GPGME_PROTOCOL_OpenPGP, nullptr, home.c_str());
    return ctx;
}

// Import all key files (.gpg, .asc, .key) from a directory.
void import_dir(gpgme_ctx_t ctx, const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = ::readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        // Check extension
        bool is_key = false;
        for (const char* ext : {".gpg", ".asc", ".key"}) {
            std::string e(ext);
            if (name.size() >= e.size() &&
                name.compare(name.size() - e.size(), e.size(), e) == 0) {
                is_key = true;
                break;
            }
        }
        if (!is_key) continue;
        std::string path = dir + "/" + name;
        gpgme_data_t dh;
        if (gpgme_data_new_from_file(&dh, path.c_str(), 1) != GPG_ERR_NO_ERROR)
            continue;
        gpgme_op_import(ctx, dh);
        gpgme_data_release(dh);
    }
    ::closedir(d);
}

} // namespace

Keyring::Keyring() = default;

Keyring::~Keyring() {
    remove_tree(m_gnupg_home);
}

void Keyring::init(const std::string& system_dir, const std::string& runtime_dir) {
    m_gnupg_home = make_temp_home();
    gpgme_ctx_t ctx = create_ctx(m_gnupg_home);
    if (!system_dir.empty())  import_dir(ctx, system_dir);
    if (!runtime_dir.empty()) import_dir(ctx, runtime_dir);
    gpgme_release(ctx);
}

std::string Keyring::import_key(const std::vector<uint8_t>& key_data) {
    if (key_data.empty()) return "";
    gpgme_ctx_t ctx = create_ctx(m_gnupg_home);
    gpgme_data_t dh;
    gpgme_error_t err = gpgme_data_new_from_mem(&dh,
        reinterpret_cast<const char*>(key_data.data()), key_data.size(), 0);
    if (gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
        gpgme_release(ctx);
        return "";
    }
    err = gpgme_op_import(ctx, dh);
    gpgme_data_release(dh);
    if (gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
        gpgme_release(ctx);
        return "";
    }
    gpgme_import_result_t res = gpgme_op_import_result(ctx);
    std::string fp;
    if (res && res->imports) {
        fp = res->imports->fpr;
    }
    gpgme_release(ctx);
    return fp;
}

std::string Keyring::import_key_file(const std::string& path) {
    gpgme_ctx_t ctx = create_ctx(m_gnupg_home);
    gpgme_data_t dh;
    if (gpgme_data_new_from_file(&dh, path.c_str(), 1) != GPG_ERR_NO_ERROR) {
        gpgme_release(ctx);
        return "";
    }
    gpgme_op_import(ctx, dh);
    gpgme_data_release(dh);
    gpgme_import_result_t res = gpgme_op_import_result(ctx);
    std::string fp;
    if (res && res->imports) {
        fp = res->imports->fpr;
    }
    gpgme_release(ctx);
    return fp;
}

bool Keyring::has_key(const std::string& fingerprint) const {
    if (fingerprint.empty() || m_gnupg_home.empty()) return false;
    gpgme_ctx_t ctx = create_ctx(m_gnupg_home);
    gpgme_key_t key = nullptr;
    gpgme_error_t err = gpgme_get_key(ctx, fingerprint.c_str(), &key, 0);
    bool found = (gpgme_err_code(err) == GPG_ERR_NO_ERROR && key != nullptr);
    if (key) gpgme_key_release(key);
    gpgme_release(ctx);
    return found;
}

} // namespace ranarch
