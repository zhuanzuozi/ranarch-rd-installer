// SPDX-License-Identifier: MIT
// ranarchctl — CLI client for the RanArch RD Installer daemon.
//
// Connects to the daemon's Unix socket and sends requests. Renders install
// events (step, log, PTY data) to stdout. Supports: install, remove, list.
#include "core/config.h"
#include "core/json.h"
#include "core/logger.h"
#include "daemon/protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <command> [args]\n"
              << "Commands:\n"
              << "  install <file.deb|file.rpm> [options]\n"
              << "  remove  <package_id>\n"
              << "  list\n"
              << "  info    <session_id>\n"
              << "\n"
              << "install options:\n"
              << "  --dry-run              sandbox trial only, write nothing for real\n"
              << "  --sandbox              run a trial install before the real one\n"
              << "  --no-signature         skip GPG signature verification\n"
              << "  --no-dependencies      skip dependency resolution (like --nodeps)\n"
              << "  --no-conflicts         overwrite files owned by other packages\n"
              << "  --no-path-traversal    DANGEROUS: allow paths escaping the root\n"
              << "  --force                shorthand for every --no-* above\n"
              << "\n"
              << "Global options:\n"
              << "  --socket <path>        daemon socket (default /run/ranarch/ranarch.sock)\n";
}

int connect_daemon(const std::string& socket_path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int cmd_list(int fd) {
    ranarch::Json req = ranarch::proto::build_list_request();
    if (!ranarch::proto::send_message(fd, req.dump())) {
        std::cerr << "error: failed to send request\n";
        return 1;
    }

    std::string resp_str;
    if (!ranarch::proto::recv_message(fd, resp_str)) {
        std::cerr << "error: no response from daemon\n";
        return 1;
    }

    ranarch::Json resp = ranarch::Json::parse(resp_str);
    const ranarch::Json* pkgs = resp.find("packages");
    if (!pkgs || !pkgs->is_array()) {
        std::cerr << "error: malformed response\n";
        return 1;
    }

    std::cout << "ID  Name                Version          Format  Signature  Date\n";
    std::cout << "--  ----                -------          ------  ---------  ----\n";
    for (const auto& p : pkgs->as_array()) {
        std::cout << p.get_int("id") << "  "
                  << p.get_string("name") << "  "
                  << p.get_string("version") << "  "
                  << p.get_string("source_format") << "  "
                  << p.get_string("signature_status") << "  "
                  << p.get_int("install_date") << "\n";
    }
    return 0;
}

// Decode the base64 used for PTY byte streams.
std::vector<uint8_t> base64_decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    int buf = 0, bits = 0;
    for (char c : in) {
        int v = val(c);
        if (v < 0) continue;  // skip '=' and whitespace
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

const char* choice_label(const std::string& c) {
    if (c == "install") return "通过 pacman 安装该依赖 (pacman -S)";
    if (c == "skip")    return "跳过此依赖，继续安装";
    if (c == "map")     return "手动指定 Arch 包名并记住映射";
    if (c == "abort")   return "中止本次安装";
    return c.c_str();
}

// Render a dependency prompt and send the user's answer back to the daemon.
bool answer_prompt(int fd, const ranarch::Json& ev) {
    const ranarch::Json* dep = ev.find("dependency");
    std::string raw  = dep ? dep->get_string("raw_name") : "";
    std::string cand = dep ? dep->get_string("arch_candidate") : "";
    std::string op   = dep ? dep->get_string("op") : "";
    std::string ver  = dep ? dep->get_string("version") : "";
    int64_t     pid  = ev.get_int("prompt_id");

    std::cerr << "\n依赖未满足: " << raw;
    if (!op.empty()) std::cerr << " " << op << " " << ver;
    if (!cand.empty()) std::cerr << "   (候选 Arch 包: " << cand << ")";
    std::cerr << "\n";

    std::vector<std::string> choices;
    const ranarch::Json* opts = ev.find("options");
    if (opts && opts->is_array())
        for (const auto& o : opts->as_array()) choices.push_back(o.as_string());
    for (std::size_t i = 0; i < choices.size(); ++i)
        std::cerr << "  " << (i + 1) << ") " << choice_label(choices[i]) << "\n";

    std::string choice = "skip";
    std::string arch;
    if (::isatty(STDIN_FILENO)) {
        std::cerr << "请选择 [1-" << choices.size() << "]（直接回车 = 跳过）: ";
        std::string line;
        if (std::getline(std::cin, line) && !line.empty()) {
            int n = std::atoi(line.c_str());
            if (n >= 1 && n <= static_cast<int>(choices.size()))
                choice = choices[n - 1];
        }
        if (choice == "install" || choice == "map") {
            // Pre-fill with the mapped candidate when we have one.
            std::cerr << "Arch 包名" << (cand.empty() ? "" : " [" + cand + "]") << ": ";
            std::string a;
            std::getline(std::cin, a);
            arch = a.empty() ? cand : a;
        }
    } else {
        std::cerr << "  (stdin 非终端，自动跳过该依赖)\n";
    }

    ranarch::Json req = ranarch::proto::build_answer_request(
        ev.get_string("session_id"), pid, choice, arch);
    return ranarch::proto::send_message(fd, req.dump());
}

int cmd_install(int fd, const std::string& path, const ranarch::InstallOptions& opts) {
    ranarch::Json req = ranarch::proto::build_install_request(path, opts);
    if (!ranarch::proto::send_message(fd, req.dump())) {
        std::cerr << "error: failed to send request\n";
        return 1;
    }

    // Read and render events until we get a "done" message.
    while (true) {
        std::string msg;
        if (!ranarch::proto::recv_message(fd, msg)) {
            std::cerr << "error: connection lost\n";
            return 1;
        }
        ranarch::Json ev = ranarch::Json::parse(msg);
        std::string type = ev.get_string("type");

        if (type == "ok") {
            std::string sid = ev.get_string("session_id");
            std::cout << "[session " << sid << " started]\n";
        } else if (type == "checks_disabled") {
            // Warn loudly: a guard the user asked for was intentionally skipped.
            std::cout << "  !! checks disabled:";
            const ranarch::Json* arr = ev.find("checks");
            if (arr && arr->is_array()) {
                for (const auto& c : arr->as_array())
                    std::cout << " " << c.as_string();
            }
            std::cout << "\n";
        } else if (type == "prompt") {
            // Interactive dependency decision — render the menu and answer.
            if (!answer_prompt(fd, ev)) {
                std::cerr << "error: failed to send answer\n";
                return 1;
            }
        } else if (type == "pty_start") {
            std::cout << "\n--- " << ev.get_string("cmd") << " ---\n" << std::flush;
        } else if (type == "pty_data") {
            // Child terminal output, base64-encoded to survive binary bytes.
            auto bytes = base64_decode(ev.get_string("data"));
            if (!bytes.empty()) {
                std::fwrite(bytes.data(), 1, bytes.size(), stdout);
                std::fflush(stdout);
            }
        } else if (type == "pty_end") {
            std::cout << "\n--- exit " << ev.get_int("exit_code") << " ---\n";
        } else if (type == "step") {
            std::string step = ev.get_string("step");
            std::string detail = ev.get_string("detail", "");
            std::cout << "  [" << step << "] " << detail << "\n";
        } else if (type == "done") {
            bool ok = ev.get_bool("ok");
            std::string summary = ev.get_string("summary");
            if (ok) {
                std::cout << "OK: " << summary << "\n";
            } else {
                std::string code = ev.get_string("error_code", "");
                std::string emsg = ev.get_string("error_message", "");
                std::cerr << "FAILED: " << summary << "\n";
                if (!code.empty())  std::cerr << "  code: " << code << "\n";
                if (!emsg.empty()) std::cerr << "  error: " << emsg << "\n";
            }
            return ok ? 0 : 1;
        }
    }
}

int cmd_info(int fd, const std::string& session_id) {
    ranarch::Json req = ranarch::proto::build_info_request(session_id);
    if (!ranarch::proto::send_message(fd, req.dump())) {
        std::cerr << "error: failed to send request\n";
        return 1;
    }
    std::string resp_str;
    if (!ranarch::proto::recv_message(fd, resp_str)) {
        std::cerr << "error: no response from daemon\n";
        return 1;
    }
    ranarch::Json resp = ranarch::Json::parse(resp_str);
    if (resp.get_string("type") == "error") {
        std::cerr << "error: " << resp.get_string("code") << ": "
                  << resp.get_string("message") << "\n";
        return 1;
    }
    std::cout << "session  : " << resp.get_string("session_id") << "\n"
              << "package  : " << resp.get_string("file_path") << "\n"
              << "active   : " << (resp.get_bool("active") ? "yes" : "no") << "\n"
              << "done     : " << (resp.get_bool("done") ? "yes" : "no") << "\n";
    const ranarch::Json* evs = resp.find("events");
    if (evs && evs->is_array()) {
        std::cout << "events   :\n";
        for (const auto& e : evs->as_array())
            std::cout << "  " << e.as_string() << "\n";
    }
    return 0;
}

int cmd_remove(int fd, int64_t pkg_id) {
    ranarch::Json req = ranarch::proto::build_remove_request(pkg_id);
    if (!ranarch::proto::send_message(fd, req.dump())) {
        std::cerr << "error: failed to send request\n";
        return 1;
    }
    // Read events until done.
    while (true) {
        std::string msg;
        if (!ranarch::proto::recv_message(fd, msg)) break;
        ranarch::Json ev = ranarch::Json::parse(msg);
        std::string type = ev.get_string("type");
        if (type == "step") {
            std::cout << "  [" << ev.get_string("step") << "] "
                      << ev.get_string("detail", "") << "\n";
        } else if (type == "done") {
            bool ok = ev.get_bool("ok");
            std::cout << (ok ? "OK: " : "FAILED: ")
                      << ev.get_string("summary") << "\n";
            return ok ? 0 : 1;
        }
    }
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];
    std::string socket_path = "/run/ranarch/ranarch.sock";

    // Allow --socket override
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--socket" && i + 1 < argc) {
            socket_path = argv[++i];
        }
    }

    ranarch::log::init("", "warn", false);

    int fd = connect_daemon(socket_path);
    if (fd < 0) {
        std::cerr << "error: cannot connect to daemon at " << socket_path
                  << " (" << std::strerror(errno) << ")\n"
                  << "Is ranarch-daemon running?\n";
        return 1;
    }

    int rc = 1;
    if (cmd == "list") {
        rc = cmd_list(fd);
    } else if (cmd == "install" && argc >= 3) {
        ranarch::InstallOptions opts;
        std::string path;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--socket") { ++i; continue; }   // handled above
            if (a == "--dry-run")            opts.dry_run = true;
            else if (a == "--sandbox")        opts.checks.sandbox_trial = true;
            else if (a == "--no-signature")   opts.checks.signature = false;
            else if (a == "--no-dependencies") opts.checks.dependencies = false;
            else if (a == "--no-conflicts")   opts.checks.conflicts = false;
            else if (a == "--no-path-traversal") opts.checks.path_traversal = false;
            else if (a == "--force") {
                // Turn every guard off at once. The server echoes this back as
                // a checks_disabled event, which we print prominently.
                opts.checks.signature      = false;
                opts.checks.dependencies   = false;
                opts.checks.conflicts      = false;
                opts.checks.path_traversal = false;
            }
            else if (a[0] != '-') path = a;
        }
        if (path.empty()) {
            usage(argv[0]);
            return 1;
        }
        rc = cmd_install(fd, path, opts);
    } else if (cmd == "remove" && argc >= 3) {
        int64_t id = std::atoll(argv[2]);
        rc = cmd_remove(fd, id);
    } else if (cmd == "info" && argc >= 3) {
        rc = cmd_info(fd, argv[2]);
    } else {
        usage(argv[0]);
    }

    ::close(fd);
    ranarch::log::shutdown();
    return rc;
}
