// SPDX-License-Identifier: MIT
// ranarch-daemon — main entry point.
//
// Initializes config, logger, DB, alpm, keyring, dep_map, installer, and the
// IPC server. Runs the epoll event loop until SIGTERM/SIGINT. SIGHUP triggers
// a config reload.
#include "core/alpm_bridge.h"
#include "core/config.h"
#include "core/db.h"
#include "core/dep_map.h"
#include "core/keyring.h"
#include "core/logger.h"
#include "core/installer.h"
#include "ipc_server.h"
#include "session.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/types.h>

namespace {

std::atomic<bool> g_should_stop{false};
std::atomic<bool> g_reload_config{false};
ranarch::IpcServer* g_server = nullptr;

void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        g_should_stop.store(true);
        if (g_server) g_server->stop();
    } else if (sig == SIGHUP) {
        g_reload_config.store(true);
    }
}

} // namespace

int main(int argc, char** argv) {
    using namespace ranarch;

    // ---- Parse args ----
    std::string config_path = "/etc/ranarch/ranarch.conf";
    bool foreground = false;
    // 会话级后端（桌面快捷方式拉起）：盯着前端的 pid，它退出就跟着退出；
    // 并把 socket 交给发起会话的用户，只有他能连。
    pid_t exit_with_pid = -1;
    uid_t socket_owner = static_cast<uid_t>(-1);
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--foreground" || arg == "-f") {
            foreground = true;
        } else if (arg.rfind("--exit-with-pid=", 0) == 0) {
            exit_with_pid = static_cast<pid_t>(std::stoi(arg.substr(16)));
        } else if (arg.rfind("--socket-owner=", 0) == 0) {
            socket_owner = static_cast<uid_t>(std::stoul(arg.substr(15)));
        }
    }
    // pkexec / sudo 起的时候拿不到显式参数也能推断出「会话用户」
    if (socket_owner == static_cast<uid_t>(-1)) {
        const char* env_uid = ::getenv("PKEXEC_UID");
        if (!env_uid) env_uid = ::getenv("SUDO_UID");
        if (env_uid) socket_owner = static_cast<uid_t>(std::stoul(env_uid));
    }

    // ---- Load config ----
    try {
        ConfigHolder::instance().load(config_path);
    } catch (const std::exception& e) {
        std::cerr << "config load failed: " << e.what()
                  << " — using defaults\n";
    }
    Config cfg = ConfigHolder::instance().snapshot();

    // ---- Init logger ----
    log::init(cfg.log_file, cfg.log_level, foreground);
    RA_LOG_INFO("ranarch-daemon starting (pid=%d)", getpid());

    // ---- Install signal handlers ----
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGHUP,  signal_handler);

    // ---- Open DB ----
    Database db;
    try {
        db.open(cfg.db_path);
        RA_LOG_INFO("database opened: %s", cfg.db_path.c_str());
    } catch (const std::exception& e) {
        RA_LOG_ERROR("database open failed: %s", e.what());
        return 1;
    }

    // ---- Init alpm ----
    AlpmBridge alpm;
    if (alpm.init()) {
        RA_LOG_INFO("alpm bridge initialized");
    } else {
        RA_LOG_WARN("alpm init failed — pacman queries unavailable");
    }

    // ---- Load keyring ----
    Keyring keyring;
    keyring.init(cfg.keyring_dir, cfg.runtime_keyring_dir);
    RA_LOG_INFO("keyring initialized");

    // ---- Load dep_map ----
    DepMap dep_map;
    if (!dep_map.load_file(cfg.dep_map_path)) {
        RA_LOG_WARN("dep_map file not found at %s — using builtin",
                    cfg.dep_map_path.c_str());
        dep_map.load_builtin();
    }

    // ---- Create installer + session manager + IPC server ----
    Installer      installer(db, dep_map, alpm, keyring);
    SessionManager sessions;
    IpcServer      server(db, dep_map, alpm, keyring, installer, sessions);

    // 会话级后端的两个开关（见 main 开头的参数解析）
    if (exit_with_pid > 0) server.set_exit_with_pid(exit_with_pid);
    if (socket_owner != static_cast<uid_t>(-1)) server.set_socket_owner(socket_owner);

    if (!server.start(cfg.socket_path)) {
        RA_LOG_ERROR("failed to start IPC server on %s", cfg.socket_path.c_str());
        return 1;
    }

    g_server = &server;
    server.run();
    g_server = nullptr;

    // ---- Handle SIGHUP-triggered reload (after run() returns) ----
    if (g_reload_config.exchange(false)) {
        RA_LOG_INFO("reloading config...");
        if (ConfigHolder::instance().reload()) {
            cfg = ConfigHolder::instance().snapshot();
            log::set_level(cfg.log_level);
            RA_LOG_INFO("config reloaded");
        }
    }

    RA_LOG_INFO("ranarch-daemon shutting down");
    log::shutdown();
    return 0;
}
