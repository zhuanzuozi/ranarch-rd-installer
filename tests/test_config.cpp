// SPDX-License-Identifier: MIT
// Minimal self-contained test runner for the config module (no external test
// framework dependency). Returns 0 on success, non-zero on first failure.
#include "core/config.h"
#include "ranarch/error.h"

#include <cassert>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    } else {
        std::cout << "ok: " << what << "\n";
    }
}

} // namespace

int main() {
    using namespace ranarch;

    const std::string ini = R"INI(
# comment
[paths]
db_path = /tmp/ranarch-test.db
staging_dir = /tmp/staging
dep_map_path = /tmp/map.csv
log_file = /tmp/ranarch.log

[log]
level = debug

[signature]
policy = strict

[sandbox]
trial_install = false
backend = nspawn

[deps]
default_action = skip

[pacman]
bin = /usr/local/bin/pacman
)INI";

    Config c = parse_ini(ini);
    check(c.db_path == "/tmp/ranarch-test.db", "db_path");
    check(c.staging_dir == "/tmp/staging", "staging_dir");
    check(c.dep_map_path == "/tmp/map.csv", "dep_map_path");
    check(c.log_file == "/tmp/ranarch.log", "log_file");
    check(c.log_level == "debug", "log_level=debug");
    check(c.signature_policy == SignaturePolicy::Strict, "signature_policy=strict");
    check(!c.sandbox_trial_install, "sandbox_trial_install=false");
    check(c.sandbox_backend == SandboxBackend::Nspawn, "sandbox_backend=nspawn");
    check(c.dep_default_action == DepDefaultAction::Skip, "dep_default_action=skip");
    check(c.pacman_bin == "/usr/local/bin/pacman", "pacman_bin");

    // Defaults when keys absent.
    Config d = parse_ini("");
    check(d.db_path == "/var/lib/ranarch/ranarch.db", "default db_path");
    check(d.signature_policy == SignaturePolicy::Warn, "default signature_policy=warn");
    check(d.sandbox_backend == SandboxBackend::Bwrap, "default sandbox_backend=bwrap");

    // Malformed line should throw.
    bool threw = false;
    try {
        (void)parse_ini("[paths]\nno_equals_here\n");
    } catch (const std::system_error&) {
        threw = true;
    }
    check(threw, "malformed line throws");

    // ConfigHolder snapshot returns defaults before any load().
    (void)ConfigHolder::instance().snapshot();

    // Request/consume reload flag.
    ConfigHolder::instance().request_reload();
    check(ConfigHolder::instance().consume_reload_request(), "reload flag set");
    check(!ConfigHolder::instance().consume_reload_request(), "reload flag cleared");

    // Error helpers exist and round-trip.
    check(std::string(to_string(RanArchError::ParseBadMagic)) == "PARSE_BAD_MAGIC",
          "to_string(ParseBadMagic)");
    check(std::string(default_message(RanArchError::Ok)) == "success",
          "default_message(Ok)");
    check(make_error_code(RanArchError::Io).category().name() ==
          std::string("ranarch"), "error category name");

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all config tests passed\n";
    return 0;
}
