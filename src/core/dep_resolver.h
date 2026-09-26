// SPDX-License-Identifier: MIT
// RanArch RD Installer — dependency resolver.
//
// Takes a list of parsed Dependencies (from a .deb/.rpm) and resolves each
// to a DepStatus: checks the dep_map for an Arch package name candidate,
// checks alpm_bridge to see if that candidate is already installed, and
// consults the DB's dep_overrides table for user-specified mappings.
//
// Unmapped or not-yet-installed deps are surfaced to the caller (installer)
// for user prompting.
#pragma once

#include "alpm_bridge.h"
#include "db.h"
#include "dep_map.h"
#include "ranarch/types.h"

#include <vector>

namespace ranarch {

class DepResolver {
public:
    // `alpm` and `db` may be nullptr (e.g., in tests without a running pacman
    // or an open database). When nullptr, installation checks are skipped and
    // overrides are not consulted.
    DepResolver(DepMap& dep_map, AlpmBridge* alpm = nullptr, Database* db = nullptr);

    // Resolve a list of dependencies from a .deb or .rpm package.
    std::vector<DepStatus> resolve(const std::vector<Dependency>& deps,
                                    PackageFormat format);

private:
    DepMap&      m_dep_map;
    AlpmBridge*  m_alpm;
    Database*    m_db;
};

} // namespace ranarch
