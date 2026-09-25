// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — configuration loaded from /etc/ranarch/ranarch.conf.
#pragma once

#include "ranarch/types.h"

#include <atomic>
#include <mutex>
#include <string>

namespace ranarch {

enum class SignaturePolicy : int { Strict = 0, Warn, Ignore };
enum class SandboxBackend : int { Bwrap = 0, Nspawn, None };
enum class DepDefaultAction : int { Ask = 0, Skip, Abort };

struct Config {
    // [paths]
    std::string staging_dir   = "/var/lib/ranarch/staging";
    std::string db_path        = "/var/lib/ranarch/ranarch.db";
    std::string dep_map_path   = "/etc/ranarch/dep_map.csv";
    std::string keyring_dir    = "/usr/share/ranarch/keyring";
    std::string runtime_keyring_dir = "/var/lib/ranarch/keyring";
    std::string log_file       = "/var/log/ranarch/ranarch.log";
    std::string socket_path    = "/run/ranarch/ranarch.sock";

    // [log]
    std::string log_level      = "info";

    // [signature]
    SignaturePolicy signature_policy = SignaturePolicy::Warn;

    // [sandbox]
    bool          sandbox_trial_install = true;
    SandboxBackend sandbox_backend = SandboxBackend::Bwrap;

    // [checks] — validation toggles used as the default for every install
    // request. A client may override them per call via the IPC `options.checks`
    // object. All default to enabled (see ValidationOptions).
    ValidationOptions checks;

    // [deps]
    DepDefaultAction dep_default_action = DepDefaultAction::Ask;

    // [pacman]
    std::string pacman_bin   = "/usr/bin/pacman";

    // Where the config was loaded from (for diagnostics / reload).
    std::string source_path;
};

// Thread-safe accessor: load() reads the current snapshot under a mutex.
//                       reload() re-parses the on-disk file.
//                       request_reload() sets a flag consumed by the daemon loop.
class ConfigHolder {
public:
    static ConfigHolder& instance();

    // Parse `path` into a fresh Config and install it as the active snapshot.
    // Throws ranarch::Exception on parse failure.
    void load(const std::string& path);

    // Re-read the previously-loaded file. Returns false if no path was set.
    bool reload();

    Config snapshot() const;

    // Cooperative reload signal: SIGHUP handler calls request_reload();
    // the daemon main loop calls consume_reload_request() to perform reload().
    void request_reload();
    bool consume_reload_request();

private:
    ConfigHolder();
    mutable std::mutex   m_mutex;
    Config               m_cfg;
    std::atomic<bool>   m_reload_requested{false};
};

// Parse INI text into a Config (mostly for unit tests). Throws on malformed input.
Config parse_ini(const std::string& text);

} // namespace ranarch
