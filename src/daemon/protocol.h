// SPDX-License-Identifier: MIT
// RanArch RD Installer — IPC protocol helpers (NDJSON over Unix socket).
//
// Framing: each message is a 4-byte big-endian length prefix + UTF-8 JSON.
// This keeps the wire format simple and lets the reader frame messages
// without scanning for delimiters.
#pragma once

#include "core/json.h"
#include "ranarch/types.h"

#include <cstdint>
#include <string>

namespace ranarch::proto {

// ---- Wire framing ----

// Write a framed JSON message to `fd`. Returns true on success.
bool send_message(int fd, const std::string& json_str);

// Read a framed message from `fd`. Returns true on success, false on EOF/error.
// `out` receives the JSON payload (without the length prefix).
bool recv_message(int fd, std::string& out);

// ---- Request builders ----

// Serialise ValidationOptions into the `checks` object clients send.
//
// Only the flags that differ from "all checks on" are emitted, so a default
// request stays small; the server applies its own defaults for anything absent.
// A flag may be sent as `false` to disable a check, or `true` to force it on
// even when /etc/ranarch/ranarch.conf turned it off.
inline Json checks_to_json(const ValidationOptions& v, bool only_deviations) {
    Json c = Json::make_object();
    const bool all_on = v.signature && v.dependencies && v.conflicts &&
                        v.path_traversal && !v.sandbox_trial;
    if (only_deviations && all_on) return c;
    c.set("signature",      Json::make_bool(v.signature));
    c.set("dependencies",   Json::make_bool(v.dependencies));
    c.set("conflicts",      Json::make_bool(v.conflicts));
    c.set("path_traversal", Json::make_bool(v.path_traversal));
    c.set("sandbox_trial",  Json::make_bool(v.sandbox_trial));
    return c;
}

// Read a `checks` object, falling back to `base` for absent keys.
inline ValidationOptions checks_from_json(const Json* checks,
                                           const ValidationOptions& base) {
    ValidationOptions v = base;
    if (!checks || !checks->is_object()) return v;
    v.signature      = checks->get_bool("signature",      v.signature);
    v.dependencies   = checks->get_bool("dependencies",   v.dependencies);
    v.conflicts      = checks->get_bool("conflicts",      v.conflicts);
    v.path_traversal = checks->get_bool("path_traversal", v.path_traversal);
    v.sandbox_trial  = checks->get_bool("sandbox_trial",  v.sandbox_trial);
    return v;
}

inline Json build_install_request(const std::string& path,
                                    const InstallOptions& opts,
                                    bool only_check_deviations = true) {
    Json req = Json::make_object();
    req.set("type", Json::make_string("install"));
    req.set("path", Json::make_string(path));
    Json o = Json::make_object();
    o.set("interactive", Json::make_bool(opts.interactive));
    o.set("dry_run",     Json::make_bool(opts.dry_run));
    o.set("checks", checks_to_json(opts.checks, only_check_deviations));
    req.set("options", std::move(o));
    return req;
}

inline Json build_remove_request(int64_t package_id) {
    Json req = Json::make_object();
    req.set("type", Json::make_string("remove"));
    req.set("package_id", Json::make_int(package_id));
    return req;
}

// ---- Interactive dependency prompts ----
//
// The daemon sends a `prompt` event and then *blocks* that install until the
// client answers with an `answer` request carrying the same prompt_id. The
// choices are: "install" (pacman -S), "skip", "map" (record raw→arch), "abort".
inline Json build_dependency_prompt(const std::string& session_id,
                                     int64_t prompt_id,
                                     const DepStatus& dep) {
    Json p = Json::make_object();
    p.set("type",      Json::make_string("prompt"));
    p.set("session_id", Json::make_string(session_id));
    p.set("prompt_id", Json::make_int(prompt_id));
    p.set("kind",      Json::make_string("dependency"));

    Json d = Json::make_object();
    d.set("raw_name",       Json::make_string(dep.raw_name));
    d.set("arch_candidate", Json::make_string(dep.arch_candidate));
    d.set("op",             Json::make_string(dep.op));
    d.set("version",        Json::make_string(dep.version));
    d.set("mapped",         Json::make_bool(!dep.arch_candidate.empty()));
    p.set("dependency", std::move(d));

    Json opts = Json::make_array();
    // All four are always offered. "map" matters most for an *unmapped*
    // dependency (no candidate), where the user has to supply the Arch name and
    // wants it remembered for next time.
    opts.append(Json::make_string("install"));
    opts.append(Json::make_string("skip"));
    opts.append(Json::make_string("map"));
    opts.append(Json::make_string("abort"));
    p.set("options", std::move(opts));
    return p;
}

// Client → daemon: the answer to a `prompt`.
inline Json build_answer_request(const std::string& session_id,
                                  int64_t prompt_id,
                                  const std::string& choice,
                                  const std::string& arch_name = "") {
    Json r = Json::make_object();
    r.set("type",      Json::make_string("answer"));
    r.set("session_id", Json::make_string(session_id));
    r.set("prompt_id", Json::make_int(prompt_id));
    r.set("choice",    Json::make_string(choice));
    if (!arch_name.empty())
        r.set("arch_name", Json::make_string(arch_name));
    return r;
}

inline Json build_list_request() {
    Json req = Json::make_object();
    req.set("type", Json::make_string("list"));
    return req;
}

inline Json build_info_request(const std::string& session_id) {
    Json req = Json::make_object();
    req.set("type", Json::make_string("info"));
    req.set("session_id", Json::make_string(session_id));
    return req;
}

// ---- Response builders ----

inline Json build_ok_response(const std::string& session_id = "") {
    Json r = Json::make_object();
    r.set("type", Json::make_string("ok"));
    if (!session_id.empty())
        r.set("session_id", Json::make_string(session_id));
    return r;
}

inline Json build_error_response(const std::string& code,
                                  const std::string& message) {
    Json r = Json::make_object();
    r.set("type",    Json::make_string("error"));
    r.set("code",    Json::make_string(code));
    r.set("message", Json::make_string(message));
    return r;
}

inline Json build_packages_response(const Json& packages) {
    Json r = Json::make_object();
    r.set("type",      Json::make_string("packages"));
    r.set("packages", packages);
    return r;
}

} // namespace ranarch::proto
