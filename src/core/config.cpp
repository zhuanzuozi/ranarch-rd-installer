// SPDX-License-Identifier: MIT
#include "config.h"

#include "ranarch/error.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace ranarch {

namespace {

std::string trim(std::string s) {
    auto not_ws = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_ws));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_ws).base(), s.end());
    return s;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

SignaturePolicy parse_sig_policy(const std::string& v) {
    std::string l = to_lower(v);
    if (l == "strict")  return SignaturePolicy::Strict;
    if (l == "ignore")  return SignaturePolicy::Ignore;
    return SignaturePolicy::Warn; // default / "warn"
}

SandboxBackend parse_sandbox_backend(const std::string& v) {
    std::string l = to_lower(v);
    if (l == "nspawn") return SandboxBackend::Nspawn;
    if (l == "none")  return SandboxBackend::None;
    return SandboxBackend::Bwrap;
}

DepDefaultAction parse_dep_action(const std::string& v) {
    std::string l = to_lower(v);
    if (l == "skip")  return DepDefaultAction::Skip;
    if (l == "abort") return DepDefaultAction::Abort;
    return DepDefaultAction::Ask;
}

} // namespace

Config parse_ini(const std::string& text) {
    Config cfg;
    std::string section;
    std::istringstream is(text);
    std::string raw;
    while (std::getline(is, raw)) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = to_lower(line.substr(1, line.size() - 2));
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            throw Exception(RanArchError::IpcProtocol,
                            "config line has no '=': " + raw);
        }
        std::string key = to_lower(trim(line.substr(0, eq)));
        std::string val = trim(line.substr(eq + 1));

        if (section == "paths") {
            if      (key == "staging_dir")   cfg.staging_dir   = val;
            else if (key == "db_path")       cfg.db_path       = val;
            else if (key == "dep_map_path")  cfg.dep_map_path  = val;
            else if (key == "keyring_dir")   cfg.keyring_dir   = val;
            else if (key == "runtime_keyring_dir") cfg.runtime_keyring_dir = val;
            else if (key == "log_file")      cfg.log_file      = val;
            else if (key == "socket_path")   cfg.socket_path   = val;
        } else if (section == "log") {
            if (key == "level") cfg.log_level = val;
        } else if (section == "signature") {
            if (key == "policy") cfg.signature_policy = parse_sig_policy(val);
        } else if (section == "sandbox") {
            if (key == "trial_install") cfg.sandbox_trial_install = (to_lower(val) == "true" || val == "1");
            else if (key == "backend")  cfg.sandbox_backend = parse_sandbox_backend(val);
        } else if (section == "deps") {
            if (key == "default_action") cfg.dep_default_action = parse_dep_action(val);
        } else if (section == "checks") {
            // Defaults for every install; a request may override them per call.
            bool on = (to_lower(val) == "true" || val == "1" || to_lower(val) == "yes");
            if      (key == "signature")      cfg.checks.signature      = on;
            else if (key == "dependencies")   cfg.checks.dependencies   = on;
            else if (key == "conflicts")      cfg.checks.conflicts      = on;
            else if (key == "path_traversal") cfg.checks.path_traversal = on;
            else if (key == "sandbox_trial")  cfg.checks.sandbox_trial  = on;
        } else if (section == "pacman") {
            if (key == "bin") cfg.pacman_bin = val;
        }
        // Unknown keys/sections are silently ignored for forward-compat.
    }
    return cfg;
}

ConfigHolder& ConfigHolder::instance() {
    static ConfigHolder h;
    return h;
}

ConfigHolder::ConfigHolder() = default;

void ConfigHolder::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        throw Exception(RanArchError::Io, "cannot open config: " + path);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    Config parsed = parse_ini(ss.str());
    parsed.source_path = path;

    std::lock_guard<std::mutex> lk(m_mutex);
    m_cfg = std::move(parsed);
}

bool ConfigHolder::reload() {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        path = m_cfg.source_path;
    }
    if (path.empty()) return false;
    load(path); // throws on failure; caller decides
    return true;
}

Config ConfigHolder::snapshot() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_cfg;
}

void ConfigHolder::request_reload()   { m_reload_requested.store(true); }
bool ConfigHolder::consume_reload_request() {
    return m_reload_requested.exchange(false);
}

} // namespace ranarch
