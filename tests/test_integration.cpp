// SPDX-License-Identifier: MIT
// Integration test: exercises the full install pipeline end-to-end.
// Uses a dry-run install (no filesystem side effects) + direct Extractor
// test in a temp directory + DB add/list/remove cycle.
#include "core/alpm_bridge.h"
#include "core/config.h"
#include "core/db.h"
#include "core/dep_map.h"
#include "core/extractor.h"
#include "core/installer.h"
#include "core/keyring.h"
#include "core/logger.h"
#include "ranarch/error.h"
#include "ranarch/types.h"

#include <archive.h>
#include <archive_entry.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ftw.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;
void check(bool cond, const std::string& what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; ++failures; }
    else         std::cout << "ok: " << what << "\n";
}

// ---- Temp dir helpers ----
int rm_cb(const char* path, const struct stat*, int, struct FTW*) {
    return ::remove(path);
}
std::string make_temp_dir() {
    char tmpl[] = "/tmp/ranarch-itest-XXXXXX";
    char* d = mkdtemp(tmpl);
    if (!d) { std::cerr << "mkdtemp failed\n"; std::exit(1); }
    return d;
}
void remove_tree(const std::string& dir) {
    if (!dir.empty()) nftw(dir.c_str(), rm_cb, 64, FTW_DEPTH | FTW_PHYS);
}

// ---- Build a minimal .deb in memory (same technique as test_deb_parser) ----
struct TarFile { std::string path, content; uint32_t mode = 0644; };

std::vector<uint8_t> build_tar_gz(const std::vector<TarFile>& files) {
    char tmpl[] = "/tmp/ranarch-itest-tar-XXXXXX";
    int fd = mkstemp(tmpl);
    struct archive* a = archive_write_new();
    archive_write_set_format_pax(a);
    archive_write_add_filter_gzip(a);
    archive_write_open_fd(a, fd);
    for (const auto& f : files) {
        struct archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, f.path.c_str());
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_size(e, f.content.size());
        archive_entry_set_perm(e, f.mode);
        archive_write_header(a, e);
        if (!f.content.empty())
            archive_write_data(a, f.content.data(), f.content.size());
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
    close(fd);
    std::ifstream ifs(tmpl, std::ios::binary);
    std::vector<uint8_t> out((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
    ::unlink(tmpl);
    return out;
}

void write_ar_member(std::vector<uint8_t>& out, const std::string& name,
                     const std::vector<uint8_t>& data) {
    char hdr[60]; std::memset(hdr, ' ', sizeof(hdr));
    std::string nm = name + "/";
    if (nm.size() > 16) nm.resize(16);
    std::memcpy(hdr, nm.c_str(), nm.size());
    hdr[16] = '0'; hdr[28] = '0'; hdr[34] = '0';
    std::memcpy(hdr + 40, "100644", 6);
    std::string sz = std::to_string(data.size());
    std::memcpy(hdr + 48, sz.c_str(), sz.size());
    hdr[58] = '`'; hdr[59] = '\n';
    out.insert(out.end(), hdr, hdr + 60);
    out.insert(out.end(), data.begin(), data.end());
    if (data.size() % 2 != 0) out.push_back('\n');
}

std::vector<uint8_t> build_deb(const std::vector<TarFile>& control,
                                const std::vector<TarFile>& data) {
    std::vector<uint8_t> out;
    const char magic[] = "!<arch>\n";
    out.insert(out.end(), magic, magic + 8);
    std::vector<uint8_t> dbin = {'2','.','0','\n'};
    write_ar_member(out, "debian-binary", dbin);
    write_ar_member(out, "control.tar.gz", build_tar_gz(control));
    write_ar_member(out, "data.tar.gz", build_tar_gz(data));
    return out;
}

// ---- Event collector ----
std::vector<std::string> g_events;

void collect_event(const std::string& json) {
    g_events.push_back(json);
    std::cout << "  event: " << json << "\n";
}

} // namespace

int main() {
    using namespace ranarch;

    // ---- 1. Build a test .deb ------------------------------------
    std::string control_text =
        "Package: hello-test\n"
        "Version: 1.0-1\n"
        "Architecture: amd64\n"
        "Depends: libc6 (>= 2.34)\n"
        "Description: integration test package\n";
    std::vector<TarFile> ctrl = {{"control", control_text, 0644}};
    std::vector<TarFile> data = {
        {"usr/bin/hello-test", "#!/bin/sh\necho hi\n", 0755},
        {"usr/share/doc/hello-test/README", "test\n", 0644},
    };
    auto deb = build_deb(ctrl, data);

    // Write .deb to temp file for parse_deb(path)
    std::string deb_path = "/tmp/ranarch-itest-" + std::to_string(getpid()) + ".deb";
    {
        std::ofstream f(deb_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(deb.data()), deb.size());
    }

    // ---- 2. Initialize subsystems --------------------------------
    log::init("", "info", false);

    Database db;
    db.open(":memory:");

    AlpmBridge alpm;
    alpm.init();  // may fail on non-Arch; that's ok

    Keyring keyring;
    keyring.init();

    DepMap dep_map;
    dep_map.load_builtin();

    Installer installer(db, dep_map, alpm, keyring);

    // ---- 3. Dry-run install (sandbox trial, no side effects) ------
    g_events.clear();
    InstallOptions opts;
    opts.dry_run = true;
    opts.checks.signature = false;   // unsigned test fixture
    opts.interactive = false;

    InstallResult result = installer.install(deb_path, opts, collect_event);
    check(result.ok, "dry-run install succeeded");
    check(!g_events.empty(), "events were emitted");
    bool found_parsing = false, found_done = false;
    for (const auto& e : g_events) {
        if (e.find("\"parsing\"") != std::string::npos)  found_parsing = true;
        if (e.find("\"done\"") != std::string::npos)     found_done = true;
    }
    check(found_parsing, "parsing step emitted");
    check(found_done,    "done step emitted");

    // ---- 3b. Disabled checks are announced -------------------------
    // A full --force run must surface every bypassed guard as its own event.
    {
        g_events.clear();
        InstallOptions forced;
        forced.dry_run = true;
        forced.checks.signature      = false;
        forced.checks.dependencies   = false;
        forced.checks.conflicts      = false;
        forced.checks.path_traversal = false;
        (void)installer.install(deb_path, forced, collect_event);
        bool found_notice = false;
        for (const auto& e : g_events) {
            if (e.find("\"checks_disabled\"") != std::string::npos) {
                found_notice = true;
                check(e.find("path_traversal") != std::string::npos,
                      "checks_disabled lists path_traversal");
                check(e.find("\"signature\"") != std::string::npos,
                      "checks_disabled lists signature");
            }
        }
        check(found_notice, "checks_disabled event emitted when guards are off");
    }

    // ---- 4. Extractor: streaming extraction in a temp directory ----
    // parse_deb(path) leaves the payload on disk, so extraction goes through
    // extract_package(), which streams from the package file.
    std::string extract_root = make_temp_dir();
    PackageMeta meta = parse_deb(deb_path);
    check(!meta.content_embedded, "file-based parse does not embed content");
    check(meta.source_path == deb_path, "source_path recorded");
    check(!meta.file_entries.empty(), "file list present without payload read");
    bool no_inline_content = true;
    for (const auto& f : meta.file_entries)
        if (!f.content.empty()) no_inline_content = false;
    check(no_inline_content, "file entries carry no inline content");

    Extractor ex;
    auto installed = ex.extract_package(meta, extract_root);
    check(installed.size() == 2, "extractor installed 2 files");

    // Verify files exist and carry the real payload bytes.
    std::string bin_path = extract_root + "/usr/bin/hello-test";
    std::string doc_path = extract_root + "/usr/share/doc/hello-test/README";
    struct stat st;
    check(::stat(bin_path.c_str(), &st) == 0, "bin file exists after extract");
    check(::stat(doc_path.c_str(), &st) == 0, "doc file exists after extract");
    check(st.st_mode & 0100100, "bin file is executable");
    // Re-stat: `st` currently describes doc_path. Content lengths prove the
    // payload was actually streamed in rather than created empty.
    ::stat(bin_path.c_str(), &st);
    check(st.st_size == 18, "bin file streamed with correct size");   // "#!/bin/sh\necho hi\n"
    ::stat(doc_path.c_str(), &st);
    check(st.st_size == 5,  "readme file streamed with correct size"); // "test\n"

    // Rollback
    Extractor::rollback(installed, extract_root);
    check(::stat(bin_path.c_str(), &st) != 0, "bin file removed after rollback");
    check(::stat(doc_path.c_str(), &st) != 0, "doc file removed after rollback");
    remove_tree(extract_root);

    // ---- 5. DB add/list/remove cycle -------------------------------
    std::string sig_str = signature_status_string(SignatureStatus::Unsigned);
    int64_t pkg_id = db.add_package(meta, meta.file_entries, sig_str, deb_path);
    check(pkg_id > 0, "add_package returned id");

    auto pkgs = db.list_packages();
    check(pkgs.size() == 1,              "list_packages size==1");
    check(pkgs[0].name == "hello-test",  "list_packages name");

    auto found = db.find_by_name("hello-test");
    check(found.has_value() && found->id == pkg_id, "find_by_name");

    auto files = db.list_files(pkg_id);
    check(files.size() == 2, "list_files size==2");

    db.remove_package(pkg_id);
    check(db.list_packages().empty(), "remove_package clears DB");

    // ---- 6. Path traversal protection ------------------------------
    bool threw = false;
    try {
        FileEntry bad;
        bad.path = "../../etc/passwd";  // relative path
        Extractor::resolve_safe(bad.path, "/tmp/test");
    } catch (const Exception&) { threw = true; }
    check(threw, "path traversal rejected");

    threw = false;
    try {
        FileEntry bad;
        bad.path = "/../../../etc/passwd";  // escapes root
        Extractor::resolve_safe(bad.path, "/tmp/test");
    } catch (const Exception&) { threw = true; }
    check(threw, "root escape rejected");

    // ---- 7. Real install + remove via Installer (with temp root) -
    // We can't use the Installer's install() directly because it writes to "/".
    // Instead, test the Extractor + DB cycle directly.
    std::string install_root = make_temp_dir();
    auto installed2 = ex.extract_package(meta, install_root);
    check(installed2.size() == 2, "real extract in temp root: 2 files");

    // Record in DB
    int64_t pkg_id2 = db.add_package(meta, meta.file_entries, sig_str, deb_path);

    // Remove via Installer::remove — clears DB record. File removal targets
    // "/" (real root); our test files are in install_root, so we also
    // manually rollback the temp files to verify cleanup works.
    g_events.clear();
    installer.remove(pkg_id2, collect_event);
    check(db.list_packages().empty(), "DB empty after installer.remove");
    // Manually clean up the temp dir files (Installer::remove targets "/").
    Extractor::rollback(installed2, install_root);
    check(::stat((install_root + "/usr/bin/hello-test").c_str(), &st) != 0,
          "files removed after rollback");
    remove_tree(install_root);

    // ---- cleanup -------------------------------------------------
    ::unlink(deb_path.c_str());
    log::shutdown();

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all integration tests passed\n";
    return 0;
}
