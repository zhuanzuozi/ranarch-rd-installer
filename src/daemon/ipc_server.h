// SPDX-License-Identifier: MIT
// RanArch RD Installer — IPC server (Unix socket + epoll).
//
// Accepts client connections on a Unix socket, reads length-prefixed JSON
// requests, dispatches them to the Installer, and streams events back.
#pragma once

#include "core/config.h"
#include "core/db.h"
#include "core/dep_map.h"
#include "core/alpm_bridge.h"
#include "core/keyring.h"
#include "core/installer.h"
#include "session.h"

#include <string>
#include <sys/types.h>

namespace ranarch {

class IpcServer {
public:
    IpcServer(Database& db, DepMap& dep_map, AlpmBridge& alpm,
              Keyring& keyring, Installer& installer,
              SessionManager& sessions);
    ~IpcServer();

    // Bind to `socket_path` and start listening. Returns false on failure.
    bool start(const std::string& socket_path);

    // Run the epoll event loop. Returns when `stop()` is called.
    void run();

    // Signal the event loop to stop (thread-safe).
    void stop();

    /**
     * 会话级后端（快捷方式用 pkexec 以 root 拉起的那种）：
     * 把 socket 的属主改成发起会话的用户，并置 0600 —— 只有他能连，别人连不上。
     */
    void set_socket_owner(uid_t uid) { m_socket_owner = uid; }

    /**
     * 盯着一个进程：它一消失（前端退出）就自行结束事件循环，
     * 从而实现「前端退出时后端一起退出」。
     */
    void set_exit_with_pid(pid_t pid) { m_exit_with_pid = pid; }

private:
    void handle_client(int fd);
    void process_request(int fd, const std::string& json_str);

    std::string m_socket_path;
    int         m_listen_fd = -1;
    int         m_epoll_fd  = -1;
    bool        m_running   = false;

    /** 见 set_socket_owner()：static_cast<uid_t>(-1) 表示不改属主 */
    uid_t m_socket_owner  = static_cast<uid_t>(-1);
    /** 见 set_exit_with_pid()：<= 0 表示不盯任何进程 */
    pid_t m_exit_with_pid = -1;
    /** 被盯进程的 pidfd（拿不到时退回 kill() 轮询） */
    int   m_watched_pidfd = -1;

    Database&       m_db;
    DepMap&         m_dep_map;
    AlpmBridge&     m_alpm;
    Keyring&        m_keyring;
    Installer&      m_installer;
    SessionManager& m_sessions;
};

} // namespace ranarch
