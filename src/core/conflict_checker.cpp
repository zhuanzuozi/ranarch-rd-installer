// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — file conflict detection implementation.
#include "conflict_checker.h"

namespace ranarch {

ConflictChecker::ConflictChecker(Database& db, AlpmBridge* alpm)
    : m_db(db), m_alpm(alpm) {}

std::vector<Conflict> ConflictChecker::check(const std::vector<FileEntry>& files) {
    std::vector<Conflict> conflicts;

    for (const auto& f : files) {
        // 1. Check ranarch DB: is this path already owned by another package?
        auto ranarch_owner = m_db.find_owner_of(f.path);
        if (ranarch_owner.has_value()) {
            Conflict c;
            c.path         = f.path;
            c.owner_pkg    = ranarch_owner->name;
            c.owner_source = "ranarch";
            conflicts.push_back(std::move(c));
            continue;  // one conflict per path is enough
        }

        // 2. Check pacman: is this path owned by an installed pacman package?
        if (m_alpm && m_alpm->is_ready()) {
            auto pacman_owner = m_alpm->find_owner(f.path);
            if (pacman_owner.has_value()) {
                Conflict c;
                c.path         = f.path;
                c.owner_pkg    = pacman_owner.value();
                c.owner_source = "pacman";
                conflicts.push_back(std::move(c));
            }
        }
    }

    return conflicts;
}

} // namespace ranarch
