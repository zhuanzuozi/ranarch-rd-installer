// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — IPC server implementation.
#include "ipc_server.h"
#include "protocol.h"
#include "authorize.h"

#include "core/json.h"
#include "core/logger.h"
#include "ranarch/error.h"

#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace ranarch {

namespace {

// Read exactly `n` bytes from `fd`. Returns false on EOF/error.
bool read_exact(int fd, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, p + got, n - got);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

// Write exactly `n` bytes to `fd`. Returns false on error.
bool write_exact(int fd, const void* buf, size_t n) {
    auto* p = static_cast<const uint8_t*>(buf);
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = ::write(fd, p + sent, n - sent);
        if (w <= 0) return false;
        sent += static_cast<size_t>(w);
    }
    return true;
}

// 取 Unix socket 对端的凭据（pid / uid）—— polkit 授权要针对「发起请求的进程」。
bool peer_credentials(int fd, pid_t& pid, uid_t& uid) {
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
    pid = cred.pid;
    uid = cred.uid;
    return true;
}

/**
 * 特权操作前的授权闸门：向 polkit 申请授权，被拒绝或无法请求时回错误事件并返回 false。
 * 调用方据此直接 return，绝不继续往下做任何会改动系统的事情。
 */
bool require_authorization(int fd, const char* action_id) {
    pid_t pid = 0;
    uid_t uid = 0;
    if (!peer_credentials(fd, pid, uid)) {
        RA_LOG_ERROR("cannot read peer credentials for authorization");
        proto::send_message(fd, proto::build_error_response(
            "AUTH_DENIED", "无法确认调用方进程身份，拒绝该操作").dump());
        return false;
    }
    std::string err;
    if (request_authorization(pid, uid, action_id, err)) return true;
    proto::send_message(fd, proto::build_error_response("AUTH_DENIED", err).dump());
    return false;
}

// pidfd_open(2) 的薄封装：内核不支持时返回 -1，调用方退回 kill() 轮询。
int open_pidfd(pid_t pid) {
#ifdef SYS_pidfd_open
    return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#else
    errno = ENOSYS;
    return -1;
#endif
}

// 被盯的进程是否已经消失：优先用 pidfd（不会被 pid 复用骗到），否则 kill(pid,0)。
bool watched_process_gone(int pidfd, pid_t pid) {
    if (pidfd >= 0) {
        struct pollfd pfd{ pidfd, POLLIN, 0 };
        return ::poll(&pfd, 1, 0) > 0;
    }
    return ::kill(pid, 0) != 0 && errno == ESRCH;
}

// 会话级后端：把 socket 交给发起会话的用户，并收紧到 0600（只有他能连）。
void hand_over_socket(const std::string& path, uid_t uid) {
    struct passwd* pw = ::getpwuid(uid);
    gid_t gid = pw ? pw->pw_gid : static_cast<gid_t>(uid);
    if (::chown(path.c_str(), uid, gid) != 0) {
        RA_LOG_WARN("chown(%s -> %u): %s", path.c_str(),
                    static_cast<unsigned>(uid), std::strerror(errno));
    }
    if (::chmod(path.c_str(), 0600) != 0) {
        RA_LOG_WARN("chmod(%s, 0600): %s", path.c_str(), std::strerror(errno));
    }
    RA_LOG_INFO("socket handed over to uid=%u (session-scoped backend)",
                static_cast<unsigned>(uid));
}

} // namespace

bool proto::send_message(int fd, const std::string& json_str) {
    uint32_t len = htonl(static_cast<uint32_t>(json_str.size()));
    if (!write_exact(fd, &len, sizeof(len))) return false;
    if (!json_str.empty() && !write_exact(fd, json_str.data(), json_str.size()))
        return false;
    return true;
}

bool proto::recv_message(int fd, std::string& out) {
    uint32_t len = 0;
    if (!read_exact(fd, &len, sizeof(len))) return false;
    len = ntohl(len);
    if (len > 10 * 1024 * 1024) return false;  // sanity: max 10 MB
    out.resize(len);
    if (len > 0 && !read_exact(fd, &out[0], len)) return false;
    return true;
}

IpcServer::IpcServer(Database& db, DepMap& dep_map, AlpmBridge& alpm,
                     Keyring& keyring, Installer& installer,
                     SessionManager& sessions)
    : m_db(db), m_dep_map(dep_map), m_alpm(alpm), m_keyring(keyring),
      m_installer(installer), m_sessions(sessions) {}

IpcServer::~IpcServer() {
    stop();
    if (m_watched_pidfd >= 0) ::close(m_watched_pidfd);
    if (m_listen_fd >= 0) ::close(m_listen_fd);
    if (m_epoll_fd >= 0)  ::close(m_epoll_fd);
    if (!m_socket_path.empty()) ::unlink(m_socket_path.c_str());
}

bool IpcServer::start(const std::string& socket_path) {
    m_socket_path = socket_path;
    // Remove any stale socket file.
    ::unlink(socket_path.c_str());

    m_listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_listen_fd < 0) {
        RA_LOG_ERROR("socket(): %s", std::strerror(errno));
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(m_listen_fd, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        RA_LOG_ERROR("bind(%s): %s", socket_path.c_str(), std::strerror(errno));
        ::close(m_listen_fd);
        m_listen_fd = -1;
        return false;
    }

    if (::listen(m_listen_fd, 8) < 0) {
        RA_LOG_ERROR("listen(): %s", std::strerror(errno));
        ::close(m_listen_fd);
        m_listen_fd = -1;
        return false;
    }

    // 会话级后端：socket 建好后移交给发起会话的用户（只有 root 才做得到）
    if (m_socket_owner != static_cast<uid_t>(-1) && ::geteuid() == 0) {
        hand_over_socket(socket_path, m_socket_owner);
    }

    m_epoll_fd = epoll_create1(0);
    if (m_epoll_fd < 0) {
        RA_LOG_ERROR("epoll_create1(): %s", std::strerror(errno));
        return false;
    }

    struct epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = m_listen_fd;
    epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_listen_fd, &ev);

    RA_LOG_INFO("listening on %s", socket_path.c_str());
    return true;
}

void IpcServer::run() {
    m_running = true;

    // 「前端退出时后端一起退出」：盯住前端进程，它消失就结束事件循环。
    if (m_exit_with_pid > 0) {
        m_watched_pidfd = open_pidfd(m_exit_with_pid);
        if (m_watched_pidfd < 0) {
            RA_LOG_WARN("pidfd_open(%d) failed: %s — falling back to kill() polling",
                        static_cast<int>(m_exit_with_pid), std::strerror(errno));
        }
        RA_LOG_INFO("session-scoped: will exit when pid %d goes away",
                    static_cast<int>(m_exit_with_pid));
    }

    struct epoll_event events[16];
    while (m_running) {
        int n = epoll_wait(m_epoll_fd, events, 16, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        // 每秒醒一次，顺便看一眼前端还在不在
        if (m_exit_with_pid > 0 && watched_process_gone(m_watched_pidfd, m_exit_with_pid)) {
            RA_LOG_INFO("frontend pid %d exited — shutting down",
                        static_cast<int>(m_exit_with_pid));
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == m_listen_fd) {
                // Accept new connection.
                int client_fd = ::accept(m_listen_fd, nullptr, nullptr);
                if (client_fd >= 0) {
                    struct epoll_event cev{};
                    cev.events  = EPOLLIN;
                    cev.data.fd = client_fd;
                    epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, client_fd, &cev);
                }
            } else {
                handle_client(events[i].data.fd);
            }
        }
    }
}

void IpcServer::stop() {
    m_running = false;
}

void IpcServer::handle_client(int fd) {
    std::string msg;
    if (!proto::recv_message(fd, msg)) {
        // Client disconnected or error — remove from epoll and close.
        epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        return;
    }
    process_request(fd, msg);
}

void IpcServer::process_request(int fd, const std::string& json_str) {
    Json req = Json::parse(json_str);
    std::string type = req.get_string("type");

    if (type == "list") {
        // List all ranarch-managed packages from the DB.
        auto pkgs = m_db.list_packages();
        Json arr = Json::make_array();
        for (const auto& p : pkgs) {
            Json obj = Json::make_object();
            obj.set("id",              Json::make_int(p.id));
            obj.set("name",           Json::make_string(p.name));
            obj.set("version",        Json::make_string(p.version));
            obj.set("source_format",  Json::make_string(p.source_format));
            obj.set("original_file",  Json::make_string(p.original_file));
            obj.set("signature_status", Json::make_string(p.signature_status));
            obj.set("install_date",   Json::make_int(p.install_date));
            arr.append(std::move(obj));
        }
        proto::send_message(fd, proto::build_packages_response(arr).dump());

    } else if (type == "info") {
        // Return a session's accumulated state: clients that connect late, or
        // that want to re-render a finished install, can replay from here.
        std::string sid = req.get_string("session_id");
        InstallSession* s = m_sessions.get(sid);
        if (!s) {
            proto::send_message(fd, proto::build_error_response(
                "IPC_SESSION_NOT_FOUND", "no such session: " + sid).dump());
        } else {
            Json resp = Json::make_object();
            resp.set("type",      Json::make_string("session"));
            resp.set("session_id", Json::make_string(s->session_id));
            resp.set("file_path", Json::make_string(s->file_path));
            resp.set("active",    Json::make_bool(s->active));
            resp.set("done",      Json::make_bool(s->done));
            Json evs = Json::make_array();
            for (const auto& e : s->events)
                evs.append(Json::make_string(e));
            resp.set("events", std::move(evs));
            if (s->done) {
                resp.set("ok",      Json::make_bool(s->result.ok));
                resp.set("summary", Json::make_string(s->result.summary));
                resp.set("installed_files_count", Json::make_int(
                    static_cast<int64_t>(s->result.installed_files_count)));
            }
            proto::send_message(fd, resp.dump());
        }

    } else if (type == "install") {
        // 特权操作：先向 polkit 请求授权（桌面会弹认证框，授权对象是这个客户端进程）。
        // 被拒绝 / 无法请求就到此为止，绝不带着 root 权限继续。
        if (!require_authorization(fd, kActionInstall)) return;

        std::string path = req.get_string("path");
        const Json* opts = req.find("options");

        // Effective check toggles: the daemon's configured defaults, with any
        // per-request overrides from `options.checks` layered on top.
        ValidationOptions base = ConfigHolder::instance().snapshot().checks;
        InstallOptions io;
        io.interactive = true;
        if (opts) {
            io.interactive = opts->get_bool("interactive", true);
            io.dry_run     = opts->get_bool("dry_run", false);
            io.checks      = proto::checks_from_json(opts->find("checks"), base);
        } else {
            io.checks = base;
        }

        // Create a session.
        std::string sid = m_sessions.create(path, io);
        proto::send_message(fd, proto::build_ok_response(sid).dump());

        // Run the install (synchronous for now — the daemon could be
        // extended to run installs in a worker thread pool).
        auto emit = [this, sid, fd](const std::string& event_json) {
            m_sessions.append_event(sid, event_json);
            proto::send_message(fd, event_json);
        };

        // Interactive dependency prompts. The installer calls this and blocks;
        // we push a `prompt` event, then read the matching `answer` straight off
        // this same connection. Blocking is safe here because installs are
        // serialised: nothing else is reading from `fd` while we wait.
        int64_t next_prompt_id = 1;
        DepPromptCallback prompt;
        if (io.interactive) {
            prompt = [this, sid, fd, &next_prompt_id](const DepStatus& dep) {
                int64_t pid = next_prompt_id++;
                proto::send_message(fd,
                    proto::build_dependency_prompt(sid, pid, dep).dump());

                // A silent client must not wedge the whole daemon, so cap the
                // wait. On timeout we abort rather than guess on the user's
                // behalf — silently skipping a dependency is worse than asking
                // again. Installs are serialised, so this also bounds how long
                // any other client can be left waiting.
                struct timeval tv{};
                tv.tv_sec = 300;  // 5 minutes to answer one prompt
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                DepDecision dec;
                dec.action = DepDecision::Action::Abort;
                while (true) {
                    std::string reply;
                    if (!proto::recv_message(fd, reply)) {
                        RA_LOG_WARN("prompt %lld: no answer (client gone or "
                                    "timed out) — aborting install",
                                    static_cast<long long>(pid));
                        break;
                    }
                    Json ans = Json::parse(reply);
                    if (ans.get_string("type") != "answer") continue;
                    if (ans.get_int("prompt_id") != pid) continue;

                    std::string choice = ans.get_string("choice");
                    if (choice == "install")      dec.action = DepDecision::Action::Install;
                    else if (choice == "map")     dec.action = DepDecision::Action::Map;
                    else if (choice == "abort")   dec.action = DepDecision::Action::Abort;
                    else                          dec.action = DepDecision::Action::Skip;
                    dec.arch_name = ans.get_string("arch_name");
                    break;
                }
                tv.tv_sec = 0;  // restore blocking behaviour
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                return dec;
            };
        }

        InstallResult result = m_installer.install(path, io, emit, prompt);
        m_sessions.finish(sid, result);

        // Send final done event with the result.
        Json done = Json::make_object();
        done.set("type",    Json::make_string("done"));
        done.set("ok",      Json::make_bool(result.ok));
        done.set("summary", Json::make_string(result.summary));
        done.set("installed_files_count", Json::make_int(
            static_cast<int64_t>(result.installed_files_count)));
        if (!result.error_code.empty())
            done.set("error_code", Json::make_string(result.error_code));
        if (!result.error_message.empty())
            done.set("error_message", Json::make_string(result.error_message));
        proto::send_message(fd, done.dump());

    } else if (type == "remove") {
        // 卸载同样是特权操作，同样需要授权。
        if (!require_authorization(fd, kActionRemove)) return;

        int64_t pkg_id = req.get_int("package_id");
        auto emit = [this, fd](const std::string& event_json) {
            proto::send_message(fd, event_json);
        };
        m_installer.remove(pkg_id, emit);
        Json resp = Json::make_object();
        resp.set("type",    Json::make_string("done"));
        resp.set("ok",      Json::make_bool(true));
        resp.set("summary", Json::make_string("removed package " +
                        std::to_string(pkg_id)));
        proto::send_message(fd, resp.dump());

    } else {
        proto::send_message(fd,
            proto::build_error_response("IPC_PROTOCOL",
                                        "unknown request type: " + type).dump());
    }
}

} // namespace ranarch
