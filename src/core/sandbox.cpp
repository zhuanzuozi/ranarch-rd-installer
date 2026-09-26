// SPDX-License-Identifier: MIT
// RanArch RD Installer — sandbox trial install implementation.
#include "sandbox.h"

#include "extractor.h"
#include "ranarch/error.h"

#include <cstdlib>
#include <cstring>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ranarch {

namespace {

int rm_cb(const char* path, const struct stat*, int, struct FTW*) {
    return ::remove(path);
}

std::string make_temp_dir() {
    char tmpl[] = "/tmp/ranarch-sandbox-XXXXXX";
    char* d = mkdtemp(tmpl);
    if (!d) throw Exception(RanArchError::SandboxUnavailable, "mkdtemp failed");
    return d;
}

void remove_tree(const std::string& dir) {
    if (!dir.empty()) nftw(dir.c_str(), rm_cb, 64, FTW_DEPTH | FTW_PHYS);
}

} // namespace

bool Sandbox::is_bwrap_available() {
    return std::system("which bwrap > /dev/null 2>&1") == 0;
}

bool Sandbox::trial_install(const PackageMeta& meta,
                             std::string& error_msg,
                             bool check_path_traversal) {
    // Extract into a throwaway directory and see whether it succeeds. A full
    // bwrap namespace would isolate this further, but the check that matters —
    // "can every entry be written without traversal or permission errors?" —
    // is the same, and streaming keeps memory flat for large packages.
    std::string temp_root = make_temp_dir();
    bool ok = false;
    try {
        Extractor ex;
        auto installed = ex.extract_package(meta, temp_root, check_path_traversal);
        ok = !installed.empty() || meta.file_entries.empty();
    } catch (const std::exception& e) {
        error_msg = e.what();
        ok = false;
    }
    remove_tree(temp_root);
    return ok;
}

} // namespace ranarch
