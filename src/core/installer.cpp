// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — install orchestration implementation.
#include "installer.h"

#include "ranarch/error.h"
#include "logger.h"

#include <chrono>
#include <sstream>
#include <sys/wait.h>

namespace ranarch {

namespace {

// Tiny JSON string escaper — avoids pulling in a JSON library for the
// event emission. The full IPC protocol (Step 12) uses a vendored json.h.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Base64 for PTY bytes: terminal output is arbitrary binary (ANSI escapes,
// partial UTF-8 sequences), so it cannot travel inside a JSON string directly.
std::string base64_encode(const uint8_t* data, std::size_t size) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < size) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) |
                      uint32_t(data[i + 2]);
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];  out += tbl[n & 63];
        i += 3;
    }
    if (i + 1 == size) {
        uint32_t n = uint32_t(data[i]) << 16;
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == size) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];  out += '=';
    }
    return out;
}

// Emit one chunk of child-process terminal output.
void emit_pty_data(EventCallback& cb, const uint8_t* data, std::size_t size) {
    if (!cb || size == 0) return;
    cb("{\"type\":\"pty_data\",\"data\":\"" + base64_encode(data, size) + "\"}");
}

} // namespace

Installer::Installer(Database& db, DepMap& dep_map, AlpmBridge& alpm, Keyring& keyring)
    : m_db(db),
      m_dep_map(dep_map),
      m_alpm(alpm),
      m_keyring(keyring),
      m_conflict_checker(db, &alpm),
      m_dep_resolver(dep_map, &alpm, &db) {}

void Installer::emit_step(EventCallback& cb, Step step, const std::string& detail) {
    if (!cb) return;
    std::ostringstream ss;
    ss << "{\"type\":\"step\",\"step\":\"" << step_name(step) << "\"";
    if (!detail.empty())
        ss << ",\"detail\":\"" << json_escape(detail) << "\"";
    ss << "}";
    cb(ss.str());
}

bool Installer::install_via_pacman(const std::string& arch_name,
                                    EventCallback& emit,
                                    std::string& error_out) {
    auto cfg = ConfigHolder::instance().snapshot();
    // --noconfirm: the user already confirmed through our own prompt, so we do
    // not want pacman asking again (there is no TTY reader attached here).
    std::vector<std::string> argv = {cfg.pacman_bin, "--noconfirm",
                                      "-S", arch_name};

    RA_LOG_INFO("installing dependency via pacman: %s", arch_name.c_str());
    if (emit) {
        emit("{\"type\":\"pty_start\",\"cmd\":\"pacman -S " +
             json_escape(arch_name) + "\"}");
    }

    int pid = m_pty.spawn(argv);
    if (pid < 0) {
        error_out = "failed to spawn " + cfg.pacman_bin;
        return false;
    }

    int exit_status = -1;
    PtyCallbacks cb;
    cb.on_output = [&emit](const uint8_t* data, std::size_t size) {
        emit_pty_data(emit, data, size);
    };
    cb.on_exit = [&exit_status](int st) { exit_status = st; };
    m_pty.run(cb);

    int code = -1;
    if (exit_status != -1 && WIFEXITED(exit_status))
        code = WEXITSTATUS(exit_status);
    if (emit)
        emit("{\"type\":\"pty_end\",\"exit_code\":" + std::to_string(code) + "}");

    if (code != 0) {
        error_out = "pacman exited with status " + std::to_string(code);
        return false;
    }
    return true;
}

bool Installer::resolve_dependencies(const PackageMeta& meta,
                                      const InstallOptions& opts,
                                      EventCallback& emit,
                                      DepPromptCallback& prompt,
                                      InstallResult& result) {
    if (!opts.checks.dependencies) {
        RA_LOG_WARN("dependency check disabled by request");
        return true;
    }

    const char* fmt = meta.format == PackageFormat::Deb ? "deb" : "rpm";
    auto deps = m_dep_resolver.resolve(meta.depends, meta.format);
    auto cfg  = ConfigHolder::instance().snapshot();

    for (const auto& d : deps) {
        if (d.state == DepState::Satisfied) continue;

        // Anything else needs a decision: Unmapped has no Arch name at all,
        // Mapped/NotInstalled has one but it is not on the system yet.
        DepDecision dec;
        if (prompt) {
            dec = prompt(d);
        } else {
            // No interactive channel: fall back to the configured policy.
            if (!d.arch_candidate.empty()) dec.arch_name = d.arch_candidate;
            switch (cfg.dep_default_action) {
                case DepDefaultAction::Abort:
                    dec.action = DepDecision::Action::Abort; break;
                case DepDefaultAction::Skip:
                case DepDefaultAction::Ask:
                default:
                    dec.action = DepDecision::Action::Skip;  break;
            }
        }

        std::string arch = dec.arch_name.empty() ? d.arch_candidate : dec.arch_name;

        switch (dec.action) {
            case DepDecision::Action::Abort:
                result.warnings.push_back("aborted on dependency " + d.raw_name);
                return false;

            case DepDecision::Action::Skip:
                result.warnings.push_back(
                    d.arch_candidate.empty()
                        ? "skipped unmapped dependency " + d.raw_name
                        : "skipped dependency " + d.arch_candidate);
                break;

            case DepDecision::Action::Map: {
                if (arch.empty()) {
                    result.warnings.push_back(
                        "cannot map " + d.raw_name + ": no Arch name given");
                    return false;
                }
                // Persist so the mapping is reused next time, and apply it in
                // process so the rest of this run sees it as well.
                m_db.set_override(fmt, d.raw_name, arch);
                m_dep_map.set_override(fmt, d.raw_name, arch);
                RA_LOG_INFO("mapped dependency %s -> %s",
                            d.raw_name.c_str(), arch.c_str());
                result.warnings.push_back("mapped " + d.raw_name + " → " + arch);
                break;
            }

            case DepDecision::Action::Install: {
                if (arch.empty()) {
                    result.warnings.push_back(
                        "cannot install " + d.raw_name + ": no Arch name known");
                    return false;
                }
                emit_step(emit, Step::DepInstall, arch);
                std::string err;
                if (!install_via_pacman(arch, emit, err)) {
                    RA_LOG_ERROR("dependency install failed: %s", err.c_str());
                    result.warnings.push_back(
                        "failed to install dependency " + arch + ": " + err);
                    return false;
                }
                break;
            }
        }
    }
    return true;
}

InstallResult Installer::install(const std::string& path,
                                   const InstallOptions& opts,
                                   EventCallback emit,
                                   DepPromptCallback prompt) {
    InstallResult result;
    result.summary = "installing " + path;

    // Surface any disabled checks before doing anything: the frontend has to be
    // able to show the user that a guard was deliberately bypassed, and the log
    // has to record it for audit. This is the only place a bypass is announced.
    {
        auto disabled = disabled_checks(opts.checks);
        if (!disabled.empty()) {
            std::string list;
            for (std::size_t i = 0; i < disabled.size(); ++i) {
                if (i) list += ", ";
                list += disabled[i];
            }
            RA_LOG_WARN("%s: proceeding with checks disabled: %s",
                        path.c_str(), list.c_str());
            if (emit) {
                std::string ev = "{\"type\":\"checks_disabled\",\"checks\":[";
                for (std::size_t i = 0; i < disabled.size(); ++i) {
                    if (i) ev += ',';
                    ev += "\"" + disabled[i] + "\"";
                }
                ev += "]}";
                emit(ev);
            }
            for (const auto& name : disabled)
                result.warnings.push_back("check disabled: " + name);
        }
    }

    try {
        // 1. Parse the package file.
        emit_step(emit, Step::Parsing, path);
        PackageMeta meta;
        // Detect format by extension, fall back to magic.
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".deb") {
            meta = parse_deb(path);
        } else if (path.size() >= 4 && path.substr(path.size() - 4) == ".rpm") {
            meta = parse_rpm(path);
        } else {
            // Try both parsers — deb first (ar magic), then rpm (lead magic).
            try {
                meta = parse_deb(path);
            } catch (...) {
                meta = parse_rpm(path);
            }
        }
        RA_LOG_INFO("%s: parsed %s-%s (%zu entries)", path.c_str(),
                    meta.name.c_str(), meta.version.c_str(),
                    meta.file_entries.size());

        // 2. Verify signature (unless disabled).
        emit_step(emit, Step::VerifyingSig);
        SignatureStatus sig_status = SignatureStatus::Skipped;
        if (opts.checks.signature) {
            auto cfg = ConfigHolder::instance().snapshot();
            if (meta.format == PackageFormat::Deb)
                sig_status = m_sig_verifier.verify_deb(meta, m_keyring);
            else
                sig_status = m_sig_verifier.verify_rpm(meta, m_keyring);
            sig_status = SigVerifier::enforce_policy(sig_status,
                                                      cfg.signature_policy);
            // Strict policy rejects both a failed signature and a missing one.
            if (cfg.signature_policy == SignaturePolicy::Strict &&
                (sig_status == SignatureStatus::Bad ||
                 sig_status == SignatureStatus::Unsigned)) {
                bool bad = (sig_status == SignatureStatus::Bad);
                result.ok = false;
                result.error_code = to_string(bad ? RanArchError::SignatureBad
                                                  : RanArchError::SignatureMissing);
                result.error_message = bad
                    ? "signature verification failed"
                    : "package is not signed (policy=strict)";
                emit_step(emit, Step::Done, "signature rejected");
                return result;
            }
        } else {
            RA_LOG_WARN("%s: signature check disabled by request", path.c_str());
        }

        // 3. Resolve dependencies (unless disabled), prompting as needed.
        emit_step(emit, Step::ResolvingDeps);
        if (!resolve_dependencies(meta, opts, emit, prompt, result)) {
            result.ok = false;
            result.error_code = to_string(RanArchError::DepUnresolved);
            result.error_message = "dependency resolution failed or was aborted";
            emit_step(emit, Step::Done, "deps unresolved");
            return result;
        }

        // 4. Check conflicts (unless disabled).
        emit_step(emit, Step::ConflictCheck);
        if (opts.checks.conflicts) {
            auto conflicts = m_conflict_checker.check(meta.file_entries);
            if (!conflicts.empty()) {
                result.ok = false;
                result.error_code = to_string(RanArchError::BlockedByConflict);
                result.error_message =
                    std::to_string(conflicts.size()) + " file conflict(s)";
                for (const auto& c : conflicts)
                    result.warnings.push_back("conflict: " + c.path + " (" +
                                              c.owner_source + ":" + c.owner_pkg + ")");
                emit_step(emit, Step::Done, "conflicts found");
                return result;
            }
        } else {
            RA_LOG_WARN("%s: conflict check disabled by request", path.c_str());
        }

        // 5. Sandbox trial — either requested explicitly or implied by dry_run.
        if (opts.dry_run || opts.checks.sandbox_trial) {
            emit_step(emit, Step::SandboxTrial);
            std::string err;
            if (!m_sandbox.trial_install(meta, err, opts.checks.path_traversal)) {
                result.ok = false;
                result.error_code = to_string(RanArchError::SandboxTrialFailed);
                result.error_message = "sandbox trial failed: " + err;
                emit_step(emit, Step::Done, "sandbox failed");
                return result;
            }
            if (opts.dry_run) {
                result.ok = true;
                result.summary = "dry-run successful for " + meta.name;
                emit_step(emit, Step::Done, "dry-run ok");
                return result;
            }
        }

        // 6. Extract files to the target root. Streams from the package file,
        //    so a multi-hundred-MB package never has to fit in memory.
        emit_step(emit, Step::Staging);
        auto installed = m_extractor.extract_package(meta, "/",
                                                     opts.checks.path_traversal);

        emit_step(emit, Step::Installing,
                   std::to_string(installed.size()) + " files");
        result.installed_files_count = installed.size();

        // 7. Record in DB.
        emit_step(emit, Step::Recording);
        std::string sig_str = signature_status_string(sig_status);
        int64_t pkg_id = m_db.add_package(meta, meta.file_entries,
                                            sig_str, path);
        RA_LOG_INFO("recorded %s-%s as package id %lld (%zu files)",
                    meta.name.c_str(), meta.version.c_str(),
                    static_cast<long long>(pkg_id), installed.size());

        // 8. Done.
        result.ok = true;
        result.summary = "installed " + meta.name + "-" + meta.version +
                         " (" + std::to_string(installed.size()) + " files)";
        emit_step(emit, Step::Done, "ok");

    } catch (const Exception& e) {
        // Preserve the specific RanArchError code — clients localise on it.
        result.ok = false;
        result.error_code = to_string(static_cast<RanArchError>(e.code().value()));
        result.error_message = e.what();
        emit_step(emit, Step::Done, "error: " + std::string(e.what()));
    } catch (const std::exception& e) {
        result.ok = false;
        result.error_code = to_string(RanArchError::Internal);
        result.error_message = e.what();
        emit_step(emit, Step::Done, "error");
    }

    return result;
}

bool Installer::remove(int64_t package_id, EventCallback emit) {
    emit_step(emit, Step::Installing, "removing package " + std::to_string(package_id));

    // Get the file list from the DB before removing the record.
    auto files = m_db.list_files(package_id);
    if (files.empty()) {
        // Package not found or has no files — just remove the DB record.
        m_db.remove_package(package_id);
        emit_step(emit, Step::Done, "removed (no files)");
        return true;
    }

    // Remove each file from the filesystem.
    std::vector<std::string> paths;
    for (const auto& f : files) {
        paths.push_back(f.path);
    }

    // Use rollback to remove the files.
    Extractor::rollback(paths, "/");

    // Remove the DB record (cascades file rows).
    m_db.remove_package(package_id);

    emit_step(emit, Step::Done, "removed " + std::to_string(paths.size()) + " files");
    return true;
}

} // namespace ranarch
