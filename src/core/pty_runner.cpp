// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — PTY runner implementation.
#include "pty_runner.h"

#include "ranarch/error.h"

#include <pty.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <signal.h>

namespace ranarch {

PtyRunner::PtyRunner() = default;

PtyRunner::~PtyRunner() {
    if (m_pid > 0) ::kill(m_pid, SIGTERM);
    if (m_master_fd >= 0) ::close(m_master_fd);
}

int PtyRunner::spawn(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;

    // Convert to C-style argv array.
    std::vector<const char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(a.c_str());
    cargv.push_back(nullptr);

    int master_fd = -1;
    pid_t pid = forkpty(&master_fd, nullptr, nullptr, nullptr);
    if (pid < 0) {
        return -1;  // fork failed
    }
    if (pid == 0) {
        // Child process — exec the command.
        execvp(cargv[0], const_cast<char* const*>(cargv.data()));
        // If exec returns, it failed.
        std::fprintf(stderr, "exec failed: %s: %s\n", cargv[0], std::strerror(errno));
        _exit(127);
    }

    // Parent process.
    m_master_fd = master_fd;
    m_pid       = pid;
    return pid;
}

void PtyRunner::run(PtyCallbacks& cb) {
    if (m_master_fd < 0 || m_pid <= 0) return;

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        throw Exception(RanArchError::Internal, "epoll_create1 failed");
    }

    struct epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = m_master_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, m_master_fd, &ev);

    bool child_done = false;
    uint8_t buf[4096];

    while (!child_done) {
        struct epoll_event events[4];
        int n = epoll_wait(epoll_fd, events, 4, -1);
        if (n < 0) {
            if (errno == EINTR) continue;  // signal interrupted
            break;
        }

        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd != m_master_fd) continue;
            ssize_t rd = read(m_master_fd, buf, sizeof(buf));
            if (rd > 0) {
                if (cb.on_output) cb.on_output(buf, static_cast<std::size_t>(rd));
            } else if (rd == 0 || (rd < 0 && errno == EIO)) {
                // Child closed the PTY (exited).
                child_done = true;
            } else if (rd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // No data available, try again.
                continue;
            } else if (rd < 0) {
                // Some other error.
                child_done = true;
            }
        }
    }

    ::close(epoll_fd);

    // Reap the child and report exit status.
    int status = 0;
    if (waitpid(m_pid, &status, 0) > 0) {
        if (cb.on_exit) cb.on_exit(status);
    }
    m_pid = -1;
    if (m_master_fd >= 0) {
        ::close(m_master_fd);
        m_master_fd = -1;
    }
}

bool PtyRunner::send_input(const uint8_t* data, std::size_t size) {
    if (m_master_fd < 0) return false;
    ssize_t n = write(m_master_fd, data, size);
    return n == static_cast<ssize_t>(size);
}

void PtyRunner::kill() {
    if (m_pid > 0) {
        ::kill(m_pid, SIGTERM);
    }
}

} // namespace ranarch
