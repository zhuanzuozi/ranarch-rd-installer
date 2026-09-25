// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — sandbox trial install (dry-run).
//
// Performs a trial extraction in a temporary directory to preview errors
// (path traversal, permission issues, disk space) without side effects.
// If bubblewrap (bwrap) is available and permitted, runs in a namespace
// sandbox; otherwise falls back to a plain temp-directory trial.
#pragma once

#include "ranarch/types.h"

#include <string>
#include <vector>

namespace ranarch {

class Sandbox {
public:
    // Check whether bwrap is available on the system.
    static bool is_bwrap_available();

    // Run a dry-run trial extraction of `meta` into a throwaway directory, so
    // errors (path traversal, permissions, disk space) surface without side
    // effects on the real filesystem. Large packages are streamed rather than
    // buffered. Returns true if the trial succeeded; on failure `error_msg`
    // carries the diagnostic.
    //
    // `check_path_traversal` mirrors ValidationOptions::path_traversal.
    bool trial_install(const PackageMeta& meta,
                        std::string& error_msg,
                        bool check_path_traversal = true);
};

} // namespace ranarch
