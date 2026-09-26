// SPDX-License-Identifier: MIT
#include "ranarch/error.h"

namespace ranarch {

namespace {
struct ErrorCategory : std::error_category {
    const char* name() const noexcept override { return "ranarch"; }
    std::string message(int ev) const override {
        return default_message(static_cast<RanArchError>(ev));
    }
};

ErrorCategory g_category;
} // namespace

const char* to_string(RanArchError e) {
    switch (e) {
        case RanArchError::Ok:                       return "OK";
        case RanArchError::ParseBadMagic:           return "PARSE_BAD_MAGIC";
        case RanArchError::ParseTruncated:          return "PARSE_TRUNCATED";
        case RanArchError::ParseUnsupportedCompression: return "PARSE_UNSUPPORTED_COMPRESSION";
        case RanArchError::ParseBadHeader:          return "PARSE_BAD_HEADER";
        case RanArchError::ConflictPacman:          return "CONFLICT_PACMAN";
        case RanArchError::ConflictRanArch:         return "CONFLICT_RANARCH";
        case RanArchError::ConflictFs:              return "CONFLICT_FS";
        case RanArchError::DepUnmapped:             return "DEP_UNMAPPED";
        case RanArchError::DepUnresolved:           return "DEP_UNRESOLVED";
        case RanArchError::DepInstallFailed:        return "DEP_INSTALL_FAILED";
        case RanArchError::ExtractDiskFull:         return "EXTRACT_DISK_FULL";
        case RanArchError::ExtractPermission:       return "EXTRACT_PERMISSION";
        case RanArchError::ExtractPathTraversal:    return "EXTRACT_PATH_TRAVERSAL";
        case RanArchError::SignatureBad:            return "SIGNATURE_BAD";
        case RanArchError::SignatureMissing:        return "SIGNATURE_MISSING";
        case RanArchError::SignatureKeyMissing:     return "SIGNATURE_KEY_MISSING";
        case RanArchError::SandboxUnavailable:     return "SANDBOX_UNAVAILABLE";
        case RanArchError::SandboxTrialFailed:     return "SANDBOX_TRIAL_FAILED";
        case RanArchError::DbCorrupt:               return "DB_CORRUPT";
        case RanArchError::DbLocked:                return "DB_LOCKED";
        case RanArchError::IpcAuth:                  return "IPC_AUTH";
        case RanArchError::IpcProtocol:              return "IPC_PROTOCOL";
        case RanArchError::IpcSessionNotFound:      return "IPC_SESSION_NOT_FOUND";
        case RanArchError::Internal:                 return "INTERNAL";
        case RanArchError::Io:                      return "IO";
        case RanArchError::Cancelled:               return "CANCELLED";
        case RanArchError::BlockedByConflict:       return "BLOCKED_BY_CONFLICT";
        case RanArchError::NeedsUserInput:          return "NEEDS_USER_INPUT";
        case RanArchError::AuthDenied:               return "AUTH_DENIED";
    }
    return "UNKNOWN";
}

const char* default_message(RanArchError e) {
    switch (e) {
        case RanArchError::Ok:                       return "success";
        case RanArchError::ParseBadMagic:           return "unrecognised file magic (not a .deb or .rpm)";
        case RanArchError::ParseTruncated:          return "package file is truncated";
        case RanArchError::ParseUnsupportedCompression: return "payload uses an unsupported compression";
        case RanArchError::ParseBadHeader:          return "malformed package header";
        case RanArchError::ConflictPacman:          return "file conflicts with a pacman-managed package";
        case RanArchError::ConflictRanArch:         return "file conflicts with another ranarch-managed package";
        case RanArchError::ConflictFs:              return "file conflicts with existing filesystem entry";
        case RanArchError::DepUnmapped:             return "dependency name has no known Arch mapping";
        case RanArchError::DepUnresolved:          return "dependency could not be resolved";
        case RanArchError::DepInstallFailed:        return "installing a dependency via pacman failed";
        case RanArchError::ExtractDiskFull:         return "not enough disk space to extract";
        case RanArchError::ExtractPermission:       return "permission denied writing a target file";
        case RanArchError::ExtractPathTraversal:    return "refused a path that escapes the install root";
        case RanArchError::SignatureBad:            return "package signature failed verification";
        case RanArchError::SignatureMissing:        return "package is not signed and policy requires a signature";
        case RanArchError::SignatureKeyMissing:     return "signing key is not present in the trusted keyring";
        case RanArchError::SandboxUnavailable:     return "sandbox backend is not available";
        case RanArchError::SandboxTrialFailed:     return "sandbox trial install failed";
        case RanArchError::DbCorrupt:               return "tracking database is corrupt";
        case RanArchError::DbLocked:                return "tracking database is locked by another process";
        case RanArchError::IpcAuth:                 return "not authorised to perform this operation";
        case RanArchError::IpcProtocol:            return "IPC protocol error";
        case RanArchError::IpcSessionNotFound:     return "no such install session";
        case RanArchError::Internal:                return "internal error";
        case RanArchError::Io:                      return "input/output error";
        case RanArchError::Cancelled:               return "operation cancelled";
        case RanArchError::BlockedByConflict:       return "installation blocked by file conflicts";
        case RanArchError::NeedsUserInput:          return "user input is required to continue";
        case RanArchError::AuthDenied:               return "not authorised (polkit denied or unavailable)";
    }
    return "unknown error";
}

std::error_code make_error_code(RanArchError e) {
    return {static_cast<int>(e), g_category};
}

} // namespace ranarch
