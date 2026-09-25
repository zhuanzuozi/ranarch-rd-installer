// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal self-contained test runner for dep_map + dep_resolver + alpm_bridge.
#include "core/alpm_bridge.h"
#include "core/db.h"
#include "core/dep_map.h"
#include "core/dep_resolver.h"
#include "ranarch/types.h"

#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;
void check(bool cond, const std::string& what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; ++failures; }
    else         std::cout << "ok: " << what << "\n";
}

} // namespace

int main() {
    using namespace ranarch;

    // ===== DepMap: builtin mappings =================================
    DepMap dm;
    dm.load_builtin();
    check(dm.size() >= 60, "builtin map has >=60 entries");

    auto r1 = dm.lookup("deb", "libc6");
    check(r1.has_value() && r1.value() == "glibc", "deb libc6 → glibc");

    auto r2 = dm.lookup("rpm", "openssl-libs");
    check(r2.has_value() && r2.value() == "openssl", "rpm openssl-libs → openssl");

    auto r3 = dm.lookup("deb", "libstdc++6");
    check(r3.has_value() && r3.value() == "gcc-libs", "deb libstdc++6 → gcc-libs");

    auto r4 = dm.lookup("deb", "nonexistent-package");
    check(!r4.has_value(), "unknown dep → nullopt");

    auto r5 = dm.lookup("rpm", "gpgme");
    check(r5.has_value() && r5.value() == "gpgme", "rpm gpgme → gpgme");

    // ===== DepMap: override ========================================
    dm.set_override("deb", "custom-lib", "arch-custom");
    auto r6 = dm.lookup("deb", "custom-lib");
    check(r6.has_value() && r6.value() == "arch-custom", "override set");

    // Override existing entry
    dm.set_override("deb", "libc6", "my-glibc");
    auto r7 = dm.lookup("deb", "libc6");
    check(r7.has_value() && r7.value() == "my-glibc", "override replaces existing");

    // ===== DepMap: CSV file loading ================================
    std::string csv_path = "/tmp/ranarch-depmap-" + std::to_string(getpid()) + ".csv";
    {
        std::ofstream f(csv_path);
        f << "# comment line\n"
          << "deb,libc6,glibc\n"
          << "deb,custom-csv,arch-from-csv\n"
          << "rpm,zlib,zlib\n";
    }
    DepMap dm2;
    check(dm2.load_file(csv_path), "load_file returns true");
    check(dm2.size() >= 3, "csv map has >=3 entries");
    auto r8 = dm2.lookup("deb", "custom-csv");
    check(r8.has_value() && r8.value() == "arch-from-csv", "csv lookup custom-csv");
    ::unlink(csv_path.c_str());

    // load_file on nonexistent file returns false
    DepMap dm3;
    check(!dm3.load_file("/tmp/nonexistent-" + std::to_string(getpid()) + ".csv"),
          "load_file nonexistent → false");

    // ===== DepResolver: resolve with builtin map (no alpm/db) ======
    DepMap dm4;
    dm4.load_builtin();
    DepResolver resolver(dm4, nullptr, nullptr);

    std::vector<Dependency> deps = {
        {"libc6",       ">=", "2.34"},      // mapped → glibc
        {"libssl1.1",   "",    ""},          // mapped → openssl-1.1
        {"unknown-lib", "",    ""},          // unmapped
    };

    auto results = resolver.resolve(deps, PackageFormat::Deb);
    check(results.size() == 3, "resolve returns 3 results");
    check(results[0].raw_name == "libc6",         "result[0].raw_name");
    check(results[0].arch_candidate == "glibc",   "result[0].arch_candidate");
    check(results[0].state == DepState::Mapped,    "result[0] Mapped (no alpm)");
    check(results[0].op == ">=" && results[0].version == "2.34",
          "result[0] version constraint preserved");
    check(results[1].arch_candidate == "openssl-1.1", "result[1].arch_candidate");
    check(results[1].state == DepState::Mapped,       "result[1] Mapped");
    check(results[2].arch_candidate.empty(),           "result[2] no candidate");
    check(results[2].state == DepState::Unmapped,      "result[2] Unmapped");

    // ===== DepResolver: with DB overrides ==========================
    Database db;
    db.open(":memory:");
    db.set_override("deb", "libc6", "my-glibc-override");
    DepResolver resolver2(dm4, nullptr, &db);
    auto results2 = resolver2.resolve({{"libc6", "", ""}}, PackageFormat::Deb);
    check(results2.size() == 1,                       "db override resolve size==1");
    check(results2[0].arch_candidate == "my-glibc-override",
          "db override takes precedence over dep_map");

    // ===== AlpmBridge: real pacman queries (if available) ==========
    AlpmBridge alpm;
    bool ok = alpm.init();
    if (ok) {
        // pacman is almost certainly installed on an Arch system
        auto pac = alpm.is_installed("pacman");
        check(pac.has_value(), "pacman is installed");
        check(!pac.value().empty(),   "pacman has a version string");

        // glibc should also be installed
        auto glibc = alpm.is_installed("glibc");
        check(glibc.has_value(),      "glibc is installed");

        // nonexistent package
        auto nope = alpm.is_installed("ranarch-does-not-exist-pkg");
        check(!nope.has_value(),       "nonexistent package → nullopt");

        // get_files for pacman (should include /usr/bin/pacman)
        auto files = alpm.get_files("pacman");
        check(!files.empty(),          "pacman has files");
        bool found_pacman_bin = false;
        for (const auto& f : files) {
            if (f.find("bin/pacman") != std::string::npos) {
                found_pacman_bin = true;
                break;
            }
        }
        check(found_pacman_bin,       "pacman files include bin/pacman");

        // ===== DepResolver with real alpm ==========================
        DepResolver resolver3(dm4, &alpm, nullptr);
        auto results3 = resolver3.resolve({{"libc6", "", ""}}, PackageFormat::Deb);
        check(results3.size() == 1,                          "real alpm resolve size==1");
        check(results3[0].arch_candidate == "glibc",        "real alpm arch_candidate==glibc");
        check(results3[0].installed,                         "real alpm glibc installed");
        check(results3[0].state == DepState::Satisfied,     "real alpm glibc Satisfied");

        // A mapped but not-installed package (use a fake dep that maps to
        // a real but likely-not-installed package)
        dm4.set_override("deb", "test-not-installed-dep", "ranarch-fake-pkg-999");
        auto results4 = resolver3.resolve({{"test-not-installed-dep", "", ""}},
                                           PackageFormat::Deb);
        check(results4.size() == 1,                          "not-installed resolve size==1");
        check(results4[0].arch_candidate == "ranarch-fake-pkg-999",
              "not-installed arch_candidate");
        check(!results4[0].installed,                        "not-installed → installed=false");
        check(results4[0].state == DepState::Mapped,        "not-installed → Mapped");
    } else {
        std::cout << "ok: alpm init failed (non-Arch?) — skipping alpm tests\n";
    }

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all dep_map tests passed\n";
    return 0;
}
