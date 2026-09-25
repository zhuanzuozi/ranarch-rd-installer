// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal self-contained test runner for the db module (no external test
// framework dependency). Returns 0 on success, non-zero on first failure.
#include "core/db.h"
#include "ranarch/types.h"

#include <cassert>
#include <iostream>
#include <string>
#include <unistd.h>

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

    // ----- in-memory open + schema -------------------------------
    Database db;
    db.open(":memory:");
    check(db.is_open(), "open :memory:");

    // signature_status_string helper
    check(std::string(signature_status_string(SignatureStatus::SignedOk)) == "signed_ok",
          "sig str signed_ok");
    check(std::string(signature_status_string(SignatureStatus::Unsigned)) == "unsigned",
          "sig str unsigned");
    check(std::string(signature_status_string(SignatureStatus::Bad)) == "bad",
          "sig str bad");
    check(std::string(signature_status_string(SignatureStatus::Skipped)) == "skipped",
          "sig str skipped");

    // ----- add_package + list_packages ---------------------------
    PackageMeta meta;
    meta.format      = PackageFormat::Deb;
    meta.name        = "hello";
    meta.version     = "2.10-2";
    meta.arch        = "amd64";
    meta.description = "hello example package";
    meta.depends.push_back({"libc6", ">=", "2.34"});
    FileEntry f1; f1.path = "/usr/bin/hello";                  f1.mode = 0755; f1.size = 1234;
    FileEntry f2; f2.path = "/usr/share/doc/hello/README";     f2.mode = 0644; f2.size = 56;
    meta.file_entries = {f1, f2};

    int64_t id = db.add_package(meta, meta.file_entries, "signed_ok", "/tmp/hello.deb");
    check(id > 0, "add_package returns positive id");

    auto pkgs = db.list_packages();
    check(pkgs.size() == 1, "list_packages size==1");
    check(pkgs[0].name        == "hello",          "list_packages name");
    check(pkgs[0].version     == "2.10-2",         "list_packages version");
    check(pkgs[0].source_format == "deb",          "list_packages source_format");
    check(pkgs[0].original_file == "/tmp/hello.deb", "list_packages original_file");
    check(pkgs[0].description == "hello example package", "list_packages description");
    check(pkgs[0].signature_status == "signed_ok", "list_packages signature_status");

    // ----- find_by_name -----------------------------------------
    auto found = db.find_by_name("hello");
    check(found.has_value() && found->id == id, "find_by_name returns the id");
    check(!db.find_by_name("nope").has_value(), "find_by_name miss");

    // ----- list_files -------------------------------------------
    auto files = db.list_files(id);
    check(files.size() == 2, "list_files size==2");
    check(files[0].path == "/usr/bin/hello",              "list_files[0].path (sorted)");
    check(files[1].path == "/usr/share/doc/hello/README", "list_files[1].path (sorted)");
    check(files[0].mode == 0755, "list_files[0].mode");

    // ----- find_owner_of ----------------------------------------
    auto owner = db.find_owner_of("/usr/bin/hello");
    check(owner.has_value() && owner->name == "hello", "find_owner_of hit");
    check(!db.find_owner_of("/nope").has_value(),     "find_owner_of miss");

    // ----- dep overrides (insert + upsert) ----------------------
    check(!db.get_override("deb", "libc6").has_value(), "get_override miss");
    db.set_override("deb", "libc6", "glibc");
    check(db.get_override("deb", "libc6").value() == "glibc", "get_override hit");
    db.set_override("deb", "libc6", "glibc-new");  // upsert
    check(db.get_override("deb", "libc6").value() == "glibc-new", "get_override upsert");
    check(!db.get_override("rpm", "libc6").has_value(), "get_override other format miss");

    // ----- trusted keys -----------------------------------------
    db.add_key("DEADBEEF12345678", "Test signing key");
    check(db.has_key("DEADBEEF12345678"), "has_key hit");
    check(!db.has_key("00000000"),         "has_key miss");
    auto keys = db.list_keys();
    check(keys.size() == 1, "list_keys size==1");
    check(keys[0].fingerprint == "DEADBEEF12345678", "list_keys[0].fingerprint");
    check(keys[0].description  == "Test signing key", "list_keys[0].description");
    // re-add updates description (ON CONFLICT DO UPDATE)
    db.add_key("DEADBEEF12345678", "renamed");
    auto keys2 = db.list_keys();
    check(keys2.size() == 1, "list_keys still size==1 after re-add");
    check(keys2[0].description == "renamed", "list_keys[0].description after update");

    // ----- second package + multiple ownership semantics --------
    PackageMeta meta2 = meta;
    meta2.name = "hello-doc";
    meta2.file_entries = {FileEntry{"/usr/share/doc/hello/README", 0644, 56, false, {}, {}}};
    int64_t id2 = db.add_package(meta2, meta2.file_entries, "unsigned", "/tmp/hello-doc.deb");
    check(id2 != id, "second package gets distinct id");
    check(db.list_packages().size() == 2, "list_packages size==2");
    // find_owner_of returns the most-recent installer of the shared path
    auto owner2 = db.find_owner_of("/usr/share/doc/hello/README");
    check(owner2.has_value() && owner2->name == "hello-doc", "find_owner_of returns latest");

    // ----- remove_package cascades files ------------------------
    db.remove_package(id);
    check(db.list_packages().size() == 1, "remove_package leaves one");
    check(db.list_files(id).empty(),  "remove_package cascades files");
    check(!db.find_owner_of("/usr/bin/hello").has_value(),
          "find_owner_of cleared for removed package");
    // the hello-doc row still owns its file
    auto owner3 = db.find_owner_of("/usr/share/doc/hello/README");
    check(owner3.has_value() && owner3->name == "hello-doc",
          "other package still owns shared path");
    db.remove_package(id2);
    check(db.list_packages().empty(), "remove_package clears all");

    // ----- file-based reopen (schema already exists) ------------
    std::string tmp = "/tmp/ranarch-test-" + std::to_string(getpid()) + ".db";
    ::unlink(tmp.c_str());
    {
        Database dbf;
        dbf.open(tmp);
        dbf.add_package(meta, meta.file_entries, "signed_ok", "/tmp/hello.deb");
        check(dbf.list_packages().size() == 1, "file db has 1 package");
    }
    {
        Database dbf;
        dbf.open(tmp);                       // reopen — schema already exists
        auto list = dbf.list_packages();
        check(list.size() == 1, "reopen finds persisted package");
        check(list[0].name == "hello", "reopen package name");
    }
    ::unlink(tmp.c_str());

    // ----- close + use-after-close safety ------------------------
    db.close();
    check(!db.is_open(), "close sets is_open false");

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all db tests passed\n";
    return 0;
}
