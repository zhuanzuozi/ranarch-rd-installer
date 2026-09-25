// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — libalpm bridge implementation.
#include "alpm_bridge.h"

#include <alpm.h>

namespace ranarch {

AlpmBridge::AlpmBridge() = default;

AlpmBridge::~AlpmBridge() {
    if (m_handle) {
        alpm_release(reinterpret_cast<alpm_handle_t*>(m_handle));
        m_handle  = nullptr;
        m_localdb = nullptr;
    }
}

bool AlpmBridge::init() {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_handle) return true;  // already initialised

    alpm_errno_t err = static_cast<alpm_errno_t>(0);
    alpm_handle_t* h = alpm_initialize("/", "/var/lib/pacman/", &err);
    if (!h) {
        // Not an Arch system, or pacman db missing — not a fatal error.
        return false;
    }
    m_handle  = h;
    m_localdb = alpm_get_localdb(h);
    return m_localdb != nullptr;
}

std::optional<std::string> AlpmBridge::is_installed(const std::string& name) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_localdb) return std::nullopt;
    alpm_pkg_t* pkg = alpm_db_get_pkg(
        reinterpret_cast<alpm_db_t*>(m_localdb), name.c_str());
    if (!pkg) return std::nullopt;
    return std::string(alpm_pkg_get_version(pkg));
}

std::vector<std::string> AlpmBridge::get_files(const std::string& name) {
    std::lock_guard<std::mutex> lk(m_mu);
    std::vector<std::string> out;
    if (!m_localdb) return out;
    alpm_pkg_t* pkg = alpm_db_get_pkg(
        reinterpret_cast<alpm_db_t*>(m_localdb), name.c_str());
    if (!pkg) return out;
    const alpm_filelist_t* fl = alpm_pkg_get_files(pkg);
    if (!fl) return out;
    for (size_t i = 0; i < fl->count; ++i) {
        if (fl->files[i].name)
            out.emplace_back(fl->files[i].name);
    }
    return out;
}

std::optional<std::string> AlpmBridge::find_owner(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_localdb) return std::nullopt;

    // Build the reverse index on first call (lazy).
    if (!m_index_built) {
        alpm_db_t* db = reinterpret_cast<alpm_db_t*>(m_localdb);
        alpm_list_t* pkgs = alpm_db_get_pkgcache(db);
        for (alpm_list_t* it = pkgs; it; it = alpm_list_next(it)) {
            alpm_pkg_t* pkg = static_cast<alpm_pkg_t*>(it->data);
            if (!pkg) continue;
            const char* pkgname = alpm_pkg_get_name(pkg);
            if (!pkgname) continue;
            const alpm_filelist_t* fl = alpm_pkg_get_files(pkg);
            if (!fl) continue;
            for (size_t i = 0; i < fl->count; ++i) {
                if (fl->files[i].name) {
                    // alpm file names are relative to root; prepend "/".
                    m_file_index[std::string("/") + fl->files[i].name] = pkgname;
                }
            }
        }
        m_index_built = true;
    }

    auto it = m_file_index.find(path);
    if (it == m_file_index.end()) return std::nullopt;
    return it->second;
}

} // namespace ranarch
