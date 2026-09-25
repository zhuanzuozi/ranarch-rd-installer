// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — PTY runner for child processes.
//
// Spawns a child process (pacman, gpg, bwrap) with a pseudo-terminal attached,
// so the daemon can capture real-time terminal output and forward it to the
// frontend. Uses forkpty() + epoll for efficient I/O multiplexing.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ranarch {

struct PtyCallbacks {
    // Called when the child produces output bytes (stdout+stderr merged via PTY).
    std::function<void(const uint8_t* data, std::size_t size)> on_output;
    // Called when the child exits. `exit_code` is the raw waitpid status.
    std::function<void(int exit_code)> on_exit;
};

class PtyRunner {
public:
    PtyRunner();
    ~PtyRunner();

    PtyRunner(const PtyRunner&)            = delete;
    PtyRunner& operator=(const PtyRunner&) = delete;

    // Spawn a child process with a PTY. `argv[0]` is the program path.
    // Returns the child PID on success, -1 on failure.
    int spawn(const std::vector<std::string>& argv);

    // Run the event loop until the child exits. Calls the callbacks for
    // output data and exit status. Must be called after spawn().
    void run(PtyCallbacks& cb);

    // Send data to the child's stdin (via the PTY master fd).
    bool send_input(const uint8_t* data, std::size_t size);

    // Terminate the child process (SIGTERM).
    void kill();

    int child_pid() const { return m_pid; }
    bool is_running() const { return m_pid > 0; }

private:
    int m_master_fd = -1;
    int m_pid       = -1;
};

} // namespace ranarch
