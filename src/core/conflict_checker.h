// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — file conflict detection.
//
// For each file a package wants to install, checks whether it conflicts with:
//   1. An existing ranarch-managed package (queried via the SQLite DB).
//   2. A pacman-managed package (queried via AlpmBridge's reverse index).
// Returns a list of Conflict entries for the caller (installer) to resolve.
#pragma once

#include "alpm_bridge.h"
#include "db.h"
#include "ranarch/types.h"

#include <vector>

namespace ranarch {

class ConflictChecker {
public:
    // `alpm` may be nullptr; when so, pacman-side checks are skipped.
    ConflictChecker(Database& db, AlpmBridge* alpm = nullptr);

    // Check the given file entries for conflicts with existing packages.
    // Returns one Conflict per conflicting path (no entry for clean paths).
    std::vector<Conflict> check(const std::vector<FileEntry>& files);

private:
    Database&    m_db;
    AlpmBridge*  m_alpm;
};

} // namespace ranarch
