// SPDX-License-Identifier: MIT
// RanArch RD Installer — install session state machine.
//
// Each install request gets a unique session_id. The session tracks the
// install progress and accumulates events that the IPC server forwards to
// the connected frontend.
#pragma once

#include "ranarch/types.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ranarch {

struct InstallSession {
    std::string session_id;
    std::string file_path;
    InstallOptions options;
    InstallResult result;
    bool        active  = false;
    bool        done    = false;
    // Event log (JSON strings) accumulated during the install.
    std::vector<std::string> events;
};

class SessionManager {
public:
    // Create a new session. Returns the session_id.
    std::string create(const std::string& file_path, const InstallOptions& opts);

    // Get a pointer to a session by id (nullptr if not found).
    // The pointer is valid until the session is finished/removed.
    InstallSession* get(const std::string& session_id);

    // Append an event to a session's event log.
    void append_event(const std::string& session_id, const std::string& event_json);

    // Mark a session as done with a result.
    void finish(const std::string& session_id, const InstallResult& result);

    // Remove a finished session.
    void remove(const std::string& session_id);

    // List all active session IDs.
    std::vector<std::string> list_active() const;

private:
    mutable std::mutex m_mu;
    std::map<std::string, InstallSession> m_sessions;
    static std::atomic<uint64_t> s_counter;
};

} // namespace ranarch
