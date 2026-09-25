// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — structured logger.
//
// Self-contained (no third-party dependency) yet level-aware and thread-safe.
// The plan names spdlog; where spdlog is available the build can be wired to
// it, but this header guarantees the same surface compiles everywhere.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace ranarch::log {

enum class Level : int { Trace = 0, Debug, Info, Warn, Err, Critical };

// Initialise the global logger. Safe to call once at program start.
//  file_path  — file sink (appended; rotated manually beyond a size cap).
//  level_str  — "trace"|"debug"|"info"|"warn"|"error"
//  to_stderr  — mirror to stderr as well (foreground mode).
void init(const std::string& file_path,
          const std::string& level_str = "info",
          bool to_stderr = false);

// Hot-reload the level from a config string; called from SIGHUP handler.
void set_level(const std::string& level_str);

// Drop & flush (for clean shutdown / reconfiguration).
void shutdown();

// The logger object call sites use.
class Logger {
public:
    void log_raw(Level lvl, const std::string& msg);
    Level level() const { return level_.load(); }
private:
    std::mutex m_mu;
    std::FILE*  m_file = nullptr;
    bool        m_to_stderr = false;
    std::atomic<Level> level_{Level::Info};
    friend void init(const std::string&, const std::string&, bool);
    friend void set_level(const std::string&);
    friend void shutdown();
};

Logger& get();

inline const char* level_name(Level l) {
    switch (l) {
        case Level::Trace:    return "TRACE";
        case Level::Debug:    return "DEBUG";
        case Level::Info:     return "INFO";
        case Level::Warn:     return "WARN";
        case Level::Err:      return "ERROR";
        case Level::Critical: return "CRITICAL";
    }
    return "?";
}

} // namespace ranarch::log

// Terse call-site macros.
#define RA_LOG(level_, ...) do { \
    auto& _lg = ::ranarch::log::get(); \
    if (static_cast<int>(level_) >= static_cast<int>(_lg.level())) { \
        char _buf[1024]; std::snprintf(_buf, sizeof(_buf), __VA_ARGS__); \
        _lg.log_raw((level_), _buf); \
    } } while (0)

#define RA_LOG_TRACE(...) RA_LOG(::ranarch::log::Level::Trace, __VA_ARGS__)
#define RA_LOG_DEBUG(...) RA_LOG(::ranarch::log::Level::Debug, __VA_ARGS__)
#define RA_LOG_INFO(...)  RA_LOG(::ranarch::log::Level::Info,  __VA_ARGS__)
#define RA_LOG_WARN(...)  RA_LOG(::ranarch::log::Level::Warn,  __VA_ARGS__)
#define RA_LOG_ERROR(...) RA_LOG(::ranarch::log::Level::Err,   __VA_ARGS__)
