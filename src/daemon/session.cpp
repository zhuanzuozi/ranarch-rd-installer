// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — install session state machine implementation.
#include "session.h"

#include <chrono>
#include <sstream>

namespace ranarch {

std::atomic<uint64_t> SessionManager::s_counter{0};

std::string SessionManager::create(const std::string& file_path,
                                     const InstallOptions& opts) {
    std::lock_guard<std::mutex> lk(m_mu);
    // Generate a unique session ID.
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    uint64_t seq = s_counter.fetch_add(1);
    std::ostringstream id_ss;
    id_ss << std::hex << now << "-" << seq;

    InstallSession s;
    s.session_id = id_ss.str();
    s.file_path  = file_path;
    s.options    = opts;
    s.active     = true;
    s.done       = false;
    std::string id = s.session_id;
    m_sessions[id] = std::move(s);
    return id;
}

InstallSession* SessionManager::get(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(m_mu);
    auto it = m_sessions.find(session_id);
    if (it == m_sessions.end()) return nullptr;
    return &it->second;
}

void SessionManager::append_event(const std::string& session_id,
                                   const std::string& event_json) {
    std::lock_guard<std::mutex> lk(m_mu);
    auto it = m_sessions.find(session_id);
    if (it != m_sessions.end()) {
        it->second.events.push_back(event_json);
    }
}

void SessionManager::finish(const std::string& session_id,
                              const InstallResult& result) {
    std::lock_guard<std::mutex> lk(m_mu);
    auto it = m_sessions.find(session_id);
    if (it != m_sessions.end()) {
        it->second.active = false;
        it->second.done   = true;
        it->second.result = result;
    }
}

void SessionManager::remove(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_sessions.erase(session_id);
}

std::vector<std::string> SessionManager::list_active() const {
    std::lock_guard<std::mutex> lk(m_mu);
    std::vector<std::string> out;
    for (const auto& [id, s] : m_sessions) {
        if (s.active) out.push_back(id);
    }
    return out;
}

} // namespace ranarch
