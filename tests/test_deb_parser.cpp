// SPDX-License-Identifier: MIT
// Minimal self-contained test runner for the deb_parser module.
// Builds a tiny .deb in-memory using libarchive's write API, then verifies the
// parser extracts the expected metadata, file entries, and symlinks.
#include "core/deb_parser.h"
#include "ranarch/error.h"
#include "ranarch/types.h"

#include <archive.h>
#include <archive_entry.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
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

// One file to pack into the control/data tar.
struct TarFile {
    std::string path;
    std::string content;
    uint32_t    mode        = 0644;
    bool        is_symlink   = false;
    std::string symlink_target;
};

// Build a gzip-compressed pax-tar in a temp file and return its bytes.
std::vector<uint8_t> build_tar_gz(const std::vector<TarFile>& files) {
    char tmpl[] = "/tmp/ranarch-tar-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) { std::cerr << "mkstemp failed\n"; std::exit(1); }

    struct archive* a = archive_write_new();
    archive_write_set_format_pax(a);
    archive_write_add_filter_gzip(a);
    if (archive_write_open_fd(a, fd) != ARCHIVE_OK) {
        archive_write_free(a); close(fd); ::unlink(tmpl);
        std::cerr << "archive_write_open_fd failed\n"; std::exit(1);
    }
    for (const auto& f : files) {
        struct archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, f.path.c_str());
        archive_entry_set_filetype(e, f.is_symlink ? AE_IFLNK : AE_IFREG);
        archive_entry_set_perm(e, f.mode & 0777);
        if (f.is_symlink) {
            archive_entry_set_symlink(e, f.symlink_target.c_str());
            archive_entry_set_size(e, 0);
        } else {
            archive_entry_set_size(e, f.content.size());
        }
        archive_write_header(a, e);
        if (!f.is_symlink && !f.content.empty())
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

// Append a 60-byte ar member header + data (padded to 2-byte boundary).
void write_ar_member(std::vector<uint8_t>& out, const std::string& name,
                     const std::vector<uint8_t>& data) {
    char hdr[60];
    std::memset(hdr, ' ', sizeof(hdr));
    std::string nm = name + "/";
    if (nm.size() > 16) nm.resize(16);
    std::memcpy(hdr, nm.c_str(), nm.size());
    hdr[16] = '0';                       // mtime
    hdr[28] = '0';                       // uid
    hdr[34] = '0';                       // gid
    std::memcpy(hdr + 40, "100644", 6);  // mode (octal)
    std::string sz = std::to_string(data.size());
    std::memcpy(hdr + 48, sz.c_str(), sz.size());
    hdr[58] = '`';
    hdr[59] = '\n';
    out.insert(out.end(), hdr, hdr + 60);
    out.insert(out.end(), data.begin(), data.end());
    if (data.size() % 2 != 0) out.push_back('\n');
}

// Build a complete .deb in memory.
std::vector<uint8_t> build_deb(const std::vector<TarFile>& control_files,
                                const std::vector<TarFile>& data_files) {
    std::vector<uint8_t> out;
    const char magic[] = "!<arch>\n";
    out.insert(out.end(), magic, magic + 8);

    std::vector<uint8_t> dbin = {'2', '.', '0', '\n'};
    write_ar_member(out, "debian-binary", dbin);

    auto ctrl = build_tar_gz(control_files);
    write_ar_member(out, "control.tar.gz", ctrl);

    auto data = build_tar_gz(data_files);
    write_ar_member(out, "data.tar.gz", data);

    return out;
}

} // namespace

int main() {
    using namespace ranarch;

    // --- Build a minimal signed-less .deb ------------------------
    const std::string control_text =
        "Package: hello\n"
        "Version: 2.10-2\n"
        "Architecture: amd64\n"
        "Depends: libc6 (>= 2.34), libssl1.1\n"
        "Description: hello example\n"
        " A small example package for testing.\n"
        " Second line of the long description.\n";

    std::vector<TarFile> control = {
        {"control", control_text, 0644, false, ""},
    };
    std::vector<TarFile> data = {
        {"usr/bin/hello",            "#!/bin/sh\necho hello\n", 0755, false, ""},
        {"usr/share/doc/hello/README", "Hello world\n",          0644, false, ""},
        {"usr/lib/hello/libhello.so",  "",                         0777, true, "libhello.so.1"},
    };

    auto deb = build_deb(control, data);

    // --- Parse from memory --------------------------------------
    PackageMeta meta = parse_deb_memory(deb.data(), deb.size());

    check(meta.format == PackageFormat::Deb, "format==Deb");
    check(meta.name             == "hello",          "name");
    check(meta.version          == "2.10-2",         "version");
    check(meta.arch             == "amd64",          "arch");
    check(meta.summary          == "hello example",  "summary (first line)");
    check(meta.description.find("A small example package") != std::string::npos,
          "description long text");
    check(meta.description.find("Second line") != std::string::npos,
          "description second line");

    // Depends
    check(meta.depends.size() == 2, "depends size==2");
    check(meta.depends[0].name == "libc6",    "depends[0].name");
    check(meta.depends[0].op   == ">=",        "depends[0].op");
    check(meta.depends[0].version == "2.34",   "depends[0].version");
    check(meta.depends[1].name == "libssl1.1", "depends[1].name");
    check(meta.depends[1].op.empty(),           "depends[1].op empty");

    // File entries
    check(meta.file_entries.size() == 3, "file_entries size==3");
    // find by path (order may vary)
    const FileEntry* bin   = nullptr;
    const FileEntry* read  = nullptr;
    const FileEntry* link  = nullptr;
    for (const auto& f : meta.file_entries) {
        if (f.path == "/usr/bin/hello")               bin  = &f;
        if (f.path == "/usr/share/doc/hello/README")  read = &f;
        if (f.path == "/usr/lib/hello/libhello.so")   link = &f;
    }
    check(bin  != nullptr,  "bin entry exists");
    check(read != nullptr, "readme entry exists");
    check(link != nullptr, "symlink entry exists");
    if (bin)  { check((bin->mode  & 0777)  == 0755, "bin mode 0755");
                check(bin->content.size()  == 21,   "bin content size");
                check(bin->content[0]  == '#',      "bin content[0]=='#'"); }
    if (read) { check((read->mode & 0777) == 0644, "readme mode 0644");
                check(read->content.size() == 12,  "readme content size"); }
    if (link) { check(link->is_symlink,              "link is_symlink");
                check(link->symlink_target == "libhello.so.1",
                      "link symlink_target"); }

    // No _gpgorigin in this fixture
    check(!meta.embedded_signature.has_value(), "no embedded signature");
    check(!meta.signed_payload.empty(),        "signed_payload populated");
    check(meta.signed_payload.size() >= 4,     "signed_payload has at least debian-binary bytes");

    // --- Parse from a file path ---------------------------------
    std::string tmppath = "/tmp/ranarch-deb-" + std::to_string(getpid()) + ".deb";
    {
        std::ofstream ofs(tmppath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(deb.data()), deb.size());
    }
    PackageMeta meta2 = parse_deb(tmppath);
    check(meta2.name == "hello", "parse_deb(path) name");
    check(meta2.version == "2.10-2", "parse_deb(path) version");
    check(meta2.file_entries.size() == 3, "parse_deb(path) file count");
    ::unlink(tmppath.c_str());

    // --- Bad magic: pass random bytes ---------------------------
    bool threw = false;
    try {
        uint8_t junk[16] = {};
        (void)parse_deb_memory(junk, sizeof(junk));
    } catch (const Exception&) { threw = true; }
    check(threw, "garbage input throws ParseBadMagic");

    // --- Empty buffer throws -----------------------------------
    threw = false;
    try {
        (void)parse_deb_memory(nullptr, 0);
    } catch (const Exception&) { threw = true; }
    check(threw, "empty input throws");

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all deb_parser tests passed\n";
    return 0;
}
