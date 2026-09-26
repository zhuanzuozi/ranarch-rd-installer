// SPDX-License-Identifier: MIT
// RanArch RD Installer — core data types shared across the library and clients.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ranarch {

// Source package format of the file being installed.
enum class PackageFormat : uint8_t {
    Deb,
    Rpm,
    Unknown,
};

// A single file extracted from a .deb/.rpm payload.
struct FileEntry {
    std::string path;          // absolute target path inside install root, e.g. /usr/bin/foo
    uint32_t    mode = 0;      // st_mode bits
    uint64_t    size = 0;
    bool        is_symlink = false;
    std::string symlink_target; // valid iff is_symlink
    std::vector<uint8_t> content; // empty for symlinks/dirs
};

// A parsed dependency line, e.g. "libfoo (>= 1.2)".
struct Dependency {
    std::string name;
    std::string op;     // "", ">=", "=", "<<", ">", "<"
    std::string version;
};

// Outcome of resolving a single dependency.
enum class DepState : uint8_t {
    Satisfied,        // already installed locally (pacman db)
    Mapped,           // mapping known but not yet installed
    Unmapped,         // no deb/rpm -> arch mapping in table
    NotInstalled,     // arch candidate known, not installed
    InstallRequested, // user chose pacman -S
    Skipped,          // user chose to skip
};

struct DepStatus {
    std::string raw_name;        // original deb/rpm dependency name
    std::string arch_candidate; // mapped arch package name (empty if unmapped)
    bool        installed = false;
    DepState    state = DepState::Unmapped;
    std::string op;
    std::string version;
};

// Result of signature verification.
enum class SignatureStatus : uint8_t {
    SignedOk,
    Unsigned,
    Bad,
    Skipped,
};

// Full metadata parsed out of a .deb/.rpm.
struct PackageMeta {
    PackageFormat format = PackageFormat::Unknown;
    std::string name;
    std::string version;
    std::string arch;
    std::string summary;
    std::string description;
    std::vector<Dependency> depends;
    std::vector<FileEntry>  file_entries;
    std::optional<std::vector<uint8_t>> embedded_signature; // raw signature blob
    std::vector<uint8_t>   signed_payload; // bytes the signature covers (detached verify)

    // --- Payload streaming -------------------------------------------
    // Parsers deliberately do NOT read the payload into memory: large packages
    // (IDEs, games, toolchains) are routinely hundreds of MB, and buffering
    // both the archive and every file's contents costs several times the
    // package size. Instead the parser records where the payload lives and the
    // extractor streams it straight to disk.
    //
    // source_path     — the package file on disk ("" when parsed from memory,
    //                   in which case file_entries carry inline content).
    // payload_offset  — absolute byte offset of the payload (rpm); unused for
    //                   deb, whose payload is located by walking the ar members.
    std::string source_path;
    uint64_t    payload_offset = 0;

    // True when file_entries[].content was populated by an in-memory parse.
    bool content_embedded = false;
};

// A single file conflict with an existing package.
struct Conflict {
    std::string path;
    std::string owner_pkg;
    std::string owner_source; // "ranarch" | "pacman"
};

// Named step in an install session, surfaced as structured events.
enum class Step : uint8_t {
    Parsing,
    VerifyingSig,
    ResolvingDeps,
    ConflictCheck,
    SandboxTrial,
    DepInstall,
    Staging,
    Installing,
    Recording,
    Done,
};

inline const char* step_name(Step s) {
    switch (s) {
        case Step::Parsing:        return "parsing";
        case Step::VerifyingSig:   return "verifying_sig";
        case Step::ResolvingDeps:  return "resolving_deps";
        case Step::ConflictCheck:  return "conflict_check";
        case Step::SandboxTrial:   return "sandbox_trial";
        case Step::DepInstall:     return "dep_install";
        case Step::Staging:        return "staging";
        case Step::Installing:     return "installing";
        case Step::Recording:      return "recording";
        case Step::Done:           return "done";
    }
    return "unknown";
}

// Toggles for the individual checks an install performs.
//
// Every check defaults to enabled. Turning one off is an explicit,
// "I know what I'm doing" action: the installer records the disabled checks in
// the log and emits them as a `checks_disabled` event so the decision is
// auditable rather than silent.
//
// Wiring:
//   C++      — InstallOptions::checks
//   IPC      — the `options.checks` object (see docs/frontend-integration.md)
//   CLI      — ranarchctl install --no-<name>
//   config   — the [checks] section of /etc/ranarch/ranarch.conf
struct ValidationOptions {
    // Verify the package's GPG signature. Off ⇒ signature is never read and
    // the package is recorded as `skipped`.
    bool signature = true;

    // Resolve dependencies and refuse to continue on unresolved ones. Off is
    // the equivalent of pacman's --nodeps.
    bool dependencies = true;

    // Refuse to overwrite a file that another package (ranarch or pacman)
    // already owns. Off is the equivalent of --overwrite.
    bool conflicts = true;

    // DANGEROUS. Reject archive paths that escape the install root
    // ("../../etc/passwd"). Off removes the only defence against a malicious
    // package writing anywhere on the filesystem — only ever disable this when
    // extracting a package you fully trust into a scratch directory.
    bool path_traversal = true;

    // Run a sandboxed dry-run trial before touching the real filesystem.
    bool sandbox_trial = false;
};

// Names of the checks that `v` has turned off, in a stable order. Used for
// logging and for the `checks_disabled` IPC event.
inline std::vector<std::string> disabled_checks(const ValidationOptions& v) {
    std::vector<std::string> out;
    if (!v.signature)      out.emplace_back("signature");
    if (!v.dependencies)   out.emplace_back("dependencies");
    if (!v.conflicts)      out.emplace_back("conflicts");
    if (!v.path_traversal) out.emplace_back("path_traversal");
    return out;
}

// Options controlling an install run.
struct InstallOptions {
    bool interactive = true;  // surface prompts to the user
    bool dry_run     = false; // sandbox trial only, do not write

    ValidationOptions checks;
};

// Final outcome of an install attempt.
struct InstallResult {
    bool        ok = false;
    std::string summary;
    uint64_t    installed_files_count = 0;
    std::vector<std::string> warnings;
    std::string error_code;     // empty on success
    std::string error_message;
};

} // namespace ranarch
