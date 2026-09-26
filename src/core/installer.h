// SPDX-License-Identifier: MIT
// RanArch RD Installer — install orchestration.
//
// Coordinates the full install flow: parse → verify signature → resolve deps
// → check conflicts → (optional) sandbox trial → extract files → record in DB.
// Emits structured step events via a callback for the frontend to consume.
#pragma once

#include "alpm_bridge.h"
#include "conflict_checker.h"
#include "db.h"
#include "deb_parser.h"
#include "dep_map.h"
#include "dep_resolver.h"
#include "extractor.h"
#include "keyring.h"
#include "pty_runner.h"
#include "config.h"            // SignaturePolicy, ConfigHolder
#include "ranarch/types.h"
#include "rpm_parser.h"
#include "sandbox.h"
#include "sig_verifier.h"

#include <functional>
#include <string>

namespace ranarch {

// Callback for emitting install events. The `event_json` is a JSON string
// describing the event (step, log, pty_data, prompt, error, done, ...).
using EventCallback = std::function<void(const std::string& event_json)>;

// What the user decided to do about an unresolved dependency.
struct DepDecision {
    enum class Action {
        Install,  // run `pacman -S <arch_name>` and stream its output
        Skip,     // install anyway without this dependency
        Map,      // record <raw_name> -> <arch_name> and treat it as satisfied
        Abort,    // stop the install
    };
    Action      action = Action::Skip;
    std::string arch_name;  // required for Install and Map
};

// Asked once per unresolved dependency. Returning a decision lets the caller
// drive the interactive flow; a default-constructed callback means
// "non-interactive", in which case the configured default action applies.
using DepPromptCallback = std::function<DepDecision(const DepStatus&)>;

class Installer {
public:
    Installer(Database& db, DepMap& dep_map, AlpmBridge& alpm, Keyring& keyring);

    // Install a .deb/.rpm file. Returns the final result.
    //
    // `prompt` is consulted for every dependency that is not already satisfied;
    // pacman output is forwarded as `pty_data` events through `emit`.
    InstallResult install(const std::string& path,
                           const InstallOptions& opts,
                           EventCallback emit,
                           DepPromptCallback prompt = {});

    // Remove a previously installed package by DB id.
    bool remove(int64_t package_id, EventCallback emit);

private:
    void emit_step(EventCallback& cb, Step step, const std::string& detail = "");

    // Resolve dependencies, prompting as needed. Returns false if the install
    // should stop (user aborted, or a required dependency could not be
    // installed). Warnings are appended to `result`.
    bool resolve_dependencies(const PackageMeta& meta,
                              const InstallOptions& opts,
                              EventCallback& emit,
                              DepPromptCallback& prompt,
                              InstallResult& result);

    // Run `pacman -S <pkg>` and forward its terminal output as pty_data events.
    // Returns true on success.
    bool install_via_pacman(const std::string& arch_name,
                             EventCallback& emit,
                             std::string& error_out);

    Database&         m_db;
    DepMap&           m_dep_map;
    AlpmBridge&       m_alpm;
    Keyring&          m_keyring;
    SigVerifier       m_sig_verifier;
    ConflictChecker   m_conflict_checker;
    DepResolver       m_dep_resolver;
    PtyRunner         m_pty;
    Extractor         m_extractor;
    Sandbox           m_sandbox;
};

} // namespace ranarch
