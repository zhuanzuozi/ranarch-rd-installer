// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — dependency resolver implementation.
#include "dep_resolver.h"

namespace ranarch {

DepResolver::DepResolver(DepMap& dep_map, AlpmBridge* alpm, Database* db)
    : m_dep_map(dep_map), m_alpm(alpm), m_db(db) {}

std::vector<DepStatus> DepResolver::resolve(const std::vector<Dependency>& deps,
                                             PackageFormat format) {
    const char* fmt_str = format == PackageFormat::Deb ? "deb"
                        : format == PackageFormat::Rpm ? "rpm"
                                                        : "unknown";

    std::vector<DepStatus> out;
    out.reserve(deps.size());

    for (const auto& dep : deps) {
        DepStatus ds;
        ds.raw_name = dep.name;
        ds.op       = dep.op;
        ds.version  = dep.version;
        ds.installed = false;
        ds.state     = DepState::Unmapped;

        // 1. Check DB overrides first (user-specified mappings take precedence).
        if (m_db) {
            auto ov = m_db->get_override(fmt_str, dep.name);
            if (ov.has_value()) {
                ds.arch_candidate = ov.value();
            }
        }

        // 2. Fall back to the dep_map.
        if (ds.arch_candidate.empty()) {
            auto mapped = m_dep_map.lookup(fmt_str, dep.name);
            if (mapped.has_value()) {
                ds.arch_candidate = mapped.value();
            }
        }

        // 3. Determine the state based on whether we have a candidate.
        if (ds.arch_candidate.empty()) {
            ds.state = DepState::Unmapped;
        } else if (m_alpm && m_alpm->is_ready()) {
            auto ver = m_alpm->is_installed(ds.arch_candidate);
            if (ver.has_value()) {
                ds.installed = true;
                ds.state     = DepState::Satisfied;
            } else {
                ds.state = DepState::Mapped;  // known but not installed
            }
        } else {
            // No alpm available — can't check, mark as Mapped.
            ds.state = DepState::Mapped;
        }

        out.push_back(std::move(ds));
    }

    return out;
}

} // namespace ranarch
