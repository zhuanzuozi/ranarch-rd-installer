// SPDX-License-Identifier: GPL-3.0-or-later
#include "logger.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <string>
#include <unordered_map>

namespace ranarch::log {

namespace {

Logger g_logger;
std::atomic<bool> g_init_done{false};

Level parse_level(const std::string& s) {
    static const std::unordered_map<std::string, Level> m = {
        {"trace", Level::Trace}, {"debug", Level::Debug},
        {"info",  Level::Info},  {"warn",  Level::Warn},
        {"warning", Level::Warn}, {"error", Level::Err},
        {"err",  Level::Err},    {"critical", Level::Critical},
    };
    auto it = m.find(s);
    return it == m.end() ? Level::Info : it->second;
}

} // namespace

void Logger::log_raw(Level lvl, const std::string& msg) {
    if (static_cast<int>(lvl) < static_cast<int>(level_.load())) {
        return;
    }
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char ts[24];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    std::string line = std::string(ts) + " [" + level_name(lvl) +
                       "] [ranarch] " + msg + "\n";

    std::lock_guard<std::mutex> lk(m_mu);
    if (m_to_stderr) {
        std::fwrite(line.data(), 1, line.size(), stderr);
    }
    if (m_file) {
        std::fwrite(line.data(), 1, line.size(), m_file);
        std::fflush(m_file);
    }
}

void init(const std::string& file_path,
          const std::string& level_str,
          bool to_stderr) {
    if (g_init_done.exchange(true)) {
        set_level(level_str);
        return;
    }
    g_logger.m_to_stderr = to_stderr;
    g_logger.level_.store(parse_level(level_str));
    if (!file_path.empty()) {
        std::FILE* f = std::fopen(file_path.c_str(), "a");
        if (f) {
            g_logger.m_file = f;
        } else if (!to_stderr) {
            g_logger.m_to_stderr = true; // fall back
        }
    }
}

void set_level(const std::string& level_str) {
    g_logger.level_.store(parse_level(level_str));
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_logger.m_mu);
    if (g_logger.m_file) {
        std::fclose(g_logger.m_file);
        g_logger.m_file = nullptr;
    }
    g_init_done.store(false);
}

Logger& get() {
    return g_logger;
}

} // namespace ranarch::log
