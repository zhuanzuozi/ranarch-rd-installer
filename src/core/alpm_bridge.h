// SPDX-License-Identifier: MIT
// RanArch RD Installer — libalpm bridge for querying the local pacman DB.
//
// Wraps libalpm to answer: "is package X installed?" and "what files does
// package X own?". libalpm is not thread-safe, so all calls are serialised
// through an internal mutex. A single instance is shared by the daemon.
// alpm.h is only included in the .cpp; the handle is stored as void* here.
#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ranarch {

class AlpmBridge {
public:
    AlpmBridge();
    ~AlpmBridge();

    AlpmBridge(const AlpmBridge&)            = delete;
    AlpmBridge& operator=(const AlpmBridge&) = delete;

    // Initialize libalpm with root="/" and dbpath="/var/lib/pacman/".
    // Returns false if initialization failed (e.g., not an Arch system).
    bool init();

    // Is the named package installed? Returns its version, or nullopt.
    std::optional<std::string> is_installed(const std::string& name);

    // Get the file list owned by an installed package. Empty if not installed.
    std::vector<std::string> get_files(const std::string& name);

    // Find which installed pacman package owns the given file path.
    // Lazily builds a reverse file→package index on first call.
    // Returns the package name, or nullopt if not owned by any package.
    std::optional<std::string> find_owner(const std::string& path);

    bool is_ready() const { return m_handle != nullptr; }

private:
    void*       m_handle  = nullptr;  // alpm_handle_t* (cast in .cpp)
    void*       m_localdb = nullptr;  // alpm_db_t*     (cast in .cpp)
    std::mutex  m_mu;
    // Lazy reverse index: file path → package name.
    std::unordered_map<std::string, std::string> m_file_index;
    bool m_index_built = false;
};

} // namespace ranarch
