// SPDX-License-Identifier: MIT
// RanArch RD Installer — error code taxonomy.
#pragma once

#include <string>
#include <system_error>

namespace ranarch {

// Stable error codes surfaced to clients via IPC `error` events.
// Numbers must never change once shipped; clients localize on the string name.
enum class RanArchError {
    Ok = 0,

    // Parse
    ParseBadMagic = 100,
    ParseTruncated,
    ParseUnsupportedCompression,
    ParseBadHeader,

    // Conflict
    ConflictPacman = 200,
    ConflictRanArch,   // legacy alias spelled out for clarity
    ConflictFs,

    // Dep
    DepUnmapped = 300,
    DepUnresolved,
    DepInstallFailed,

    // Extract
    ExtractDiskFull = 400,
    ExtractPermission,
    ExtractPathTraversal,

    // Signature
    SignatureBad = 500,
    SignatureMissing,
    SignatureKeyMissing,

    // Sandbox
    SandboxUnavailable = 600,
    SandboxTrialFailed,

    // DB
    DbCorrupt = 700,
    DbLocked,

    // IPC
    IpcAuth = 800,
    IpcProtocol,
    IpcSessionNotFound,

    // Generic
    Internal = 900,
    Io,
    Cancelled,

    BlockedByConflict = 1000,
    NeedsUserInput,
    /** polkit 授权被拒绝 / 无法请求授权（特权操作前的强制检查） */
    AuthDenied,
};

// Convert an error code to its stable string name (used as the `code` field
// in IPC events; clients localise by this name).
const char* to_string(RanArchError e);

// Human-readable default message template (English; clients may override).
const char* default_message(RanArchError e);

// std::error_code interop so errors compose with std::system_error.
std::error_code make_error_code(RanArchError e);

class Exception : public std::system_error {
public:
    explicit Exception(RanArchError e, std::string detail = {})
        : std::system_error(make_error_code(e),
                            detail.empty() ? default_message(e) : detail) {}
};

} // namespace ranarch

namespace std {
template <>
struct is_error_code_enum<ranarch::RanArchError> : true_type {};
}
