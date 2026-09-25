// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal self-contained test runner for the rpm_parser module.
// Builds a tiny .rpm in-memory (Lead + Signature Header + Header + cpio.gz
// payload) and verifies the parser extracts metadata, deps, and files.
#include "core/rpm_parser.h"
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

void write_be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((v >> 24) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 8)  & 0xFF);
    out.push_back(v & 0xFF);
}
void write_be16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back((v >> 8) & 0xFF);
    out.push_back(v & 0xFF);
}

// ---- RPM Lead (96 bytes) --------------------------------------------
std::vector<uint8_t> build_lead(const std::string& name) {
    std::vector<uint8_t> lead(96, 0);
    lead[0] = 0xed; lead[1] = 0xab; lead[2] = 0xee; lead[3] = 0xdb;
    lead[4] = 3;   // major
    lead[5] = 0;   // minor
    // type=0 (binary), arch=0
    std::strncpy(reinterpret_cast<char*>(lead.data() + 10), name.c_str(), 65);
    lead[76] = 0; lead[77] = 1; // OS=Linux
    lead[78] = 0; lead[79] = 5; // sig type=HEADERSIG
    return lead;
}

// ---- RPM header builder ----------------------------------------------
struct RpmIndexEntry { uint32_t tag, type, offset, count; };

// Build a header (preamble + index + data) from tag entries + data.
std::vector<uint8_t> build_header(const std::vector<RpmIndexEntry>& entries,
                                   const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    // Preamble: magic(3) + version(1) + reserved(4) + il(4,BE) + dl(4,BE) = 16 bytes
    out.push_back(0x8e); out.push_back(0xad); out.push_back(0xe8);
    out.push_back(1);                                    // header version
    write_be32(out, 0);                                  // reserved
    write_be32(out, static_cast<uint32_t>(entries.size())); // il
    write_be32(out, static_cast<uint32_t>(data.size()));    // dl
    // Index entries (16 bytes each)
    for (const auto& e : entries) {
        write_be32(out, e.tag);
        write_be32(out, e.type);
        write_be32(out, e.offset);
        write_be32(out, e.count);
    }
    // Data area
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// Helper to build a data area + index entries from high-level tag values.
struct TagData {
    uint32_t tag;
    uint32_t type;
    std::vector<uint8_t> bytes;
    uint32_t count = 1;
};

std::vector<uint8_t> build_rpm_header(const std::vector<TagData>& tags) {
    std::vector<RpmIndexEntry> entries;
    std::vector<uint8_t> data;
    for (const auto& t : tags) {
        // INT16 aligns to 2, INT32/INT64 to 4/8. Pad to 8 before setting offset.
        while (data.size() % 8 != 0 && (t.type == 3 || t.type == 4 || t.type == 5))
            data.push_back(0);
        RpmIndexEntry e;
        e.tag = t.tag;
        e.type = t.type;
        e.offset = static_cast<uint32_t>(data.size());
        e.count = t.count;
        entries.push_back(e);
        data.insert(data.end(), t.bytes.begin(), t.bytes.end());
    }
    return build_header(entries, data);
}

void add_string(std::vector<TagData>& tags, uint32_t tag, const std::string& s) {
    TagData td; td.tag = tag; td.type = 6; td.count = 1;
    td.bytes.assign(s.begin(), s.end());
    td.bytes.push_back(0);
    tags.push_back(std::move(td));
}

void add_string_array(std::vector<TagData>& tags, uint32_t tag,
                       const std::vector<std::string>& strs) {
    TagData td; td.tag = tag; td.type = 8; td.count = static_cast<uint32_t>(strs.size());
    for (const auto& s : strs) {
        td.bytes.insert(td.bytes.end(), s.begin(), s.end());
        td.bytes.push_back(0);
    }
    tags.push_back(std::move(td));
}

void add_int32_array(std::vector<TagData>& tags, uint32_t tag,
                      const std::vector<uint32_t>& vals) {
    TagData td; td.tag = tag; td.type = 4; td.count = static_cast<uint32_t>(vals.size());
    for (uint32_t v : vals) write_be32(td.bytes, v);
    tags.push_back(std::move(td));
}

void add_int16_array(std::vector<TagData>& tags, uint32_t tag,
                      const std::vector<uint16_t>& vals) {
    TagData td; td.tag = tag; td.type = 3; td.count = static_cast<uint32_t>(vals.size());
    for (uint16_t v : vals) write_be16(td.bytes, v);
    tags.push_back(std::move(td));
}

// ---- Cpio payload (gzip-compressed newc cpio) ------------------------
std::vector<uint8_t> build_cpio_gz(const std::vector<std::string>& paths,
                                    const std::vector<std::string>& contents) {
    char tmpl[] = "/tmp/ranarch-cpio-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) { std::cerr << "mkstemp failed\n"; std::exit(1); }

    struct archive* a = archive_write_new();
    archive_write_set_format_cpio(a);
    archive_write_add_filter_gzip(a);
    if (archive_write_open_fd(a, fd) != ARCHIVE_OK) {
        archive_write_free(a); close(fd); ::unlink(tmpl);
        std::cerr << "cpio open failed\n"; std::exit(1);
    }
    for (size_t i = 0; i < paths.size(); ++i) {
        struct archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, paths[i].c_str());
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_entry_set_size(e, contents[i].size());
        archive_write_header(a, e);
        if (!contents[i].empty())
            archive_write_data(a, contents[i].data(), contents[i].size());
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

// ---- Assemble a full RPM ----------------------------------------------
std::vector<uint8_t> build_rpm(const std::vector<TagData>& header_tags,
                                const std::vector<std::string>& cpio_paths,
                                const std::vector<std::string>& cpio_contents) {
    std::vector<uint8_t> rpm;

    // 1. Lead (96 bytes)
    auto lead = build_lead("hello");
    rpm.insert(rpm.end(), lead.begin(), lead.end());

    // 2. Signature header (minimal: empty, 0 entries, 0 data)
    {
        std::vector<RpmIndexEntry> empty;
        std::vector<uint8_t> empty_data;
        auto sig_hdr = build_header(empty, empty_data);
        rpm.insert(rpm.end(), sig_hdr.begin(), sig_hdr.end());
    }
    // Pad to 8-byte boundary
    while (rpm.size() % 8 != 0) rpm.push_back(0);

    // 3. Header
    auto hdr = build_rpm_header(header_tags);
    rpm.insert(rpm.end(), hdr.begin(), hdr.end());

    // 4. Payload (cpio.gz)
    auto payload = build_cpio_gz(cpio_paths, cpio_contents);
    rpm.insert(rpm.end(), payload.begin(), payload.end());

    return rpm;
}

} // namespace

int main() {
    using namespace ranarch;

    // Build header tags for a minimal "hello" RPM.
    std::vector<TagData> tags;
    add_string(tags, 1000, "hello");            // NAME
    add_string(tags, 1001, "2.10");             // VERSION
    add_string(tags, 1002, "2.el7");            // RELEASE
    add_string(tags, 1022, "x86_64");           // ARCH
    add_string(tags, 1004, "hello example");    // SUMMARY
    add_string(tags, 1005, "A small example package.\n Second line."); // DESCRIPTION

    // Dependencies
    add_string_array(tags, 1049, {"libc.so.6", "libssl.so.3"});   // REQUIRENAME
    add_string_array(tags, 1050, {"2.34", "3.0"});                // REQUIREVERSION
    add_int32_array(tags, 1048, {0x08 | 0x02, 0x08});            // REQUIREFLAGS (= and <=)

    // File entries (FILENAMES=5007 is the full-path array; rpm >= 4.12)
    add_string_array(tags, 5007, {"/usr/bin/hello", "/usr/share/doc/hello/README"});
    add_int16_array(tags, 1030, {0100755, 0100644});             // FILEMODES
    add_int32_array(tags, 1028, {21, 12});                        // FILESIZES

    // Cpio payload contents (matching the file names)
    std::vector<std::string> cpio_paths = {"usr/bin/hello", "usr/share/doc/hello/README"};
    std::vector<std::string> cpio_contents = {"#!/bin/sh\necho hello\n", "Hello world\n"};

    auto rpm = build_rpm(tags, cpio_paths, cpio_contents);

    // --- Parse from memory -------------------------------------
    PackageMeta meta = parse_rpm_memory(rpm.data(), rpm.size());

    check(meta.format == PackageFormat::Rpm, "format==Rpm");
    check(meta.name    == "hello",            "name");
    check(meta.version == "2.10-2.el7",       "version (version-release)");
    check(meta.arch    == "x86_64",           "arch");
    check(meta.summary == "hello example",    "summary");
    check(meta.description.find("A small example package") != std::string::npos,
          "description text");

    // Depends
    check(meta.depends.size() == 2, "depends size==2");
    check(meta.depends[0].name == "libc.so.6", "depends[0].name");
    check(meta.depends[0].op   == "<=",        "depends[0].op (<=)");
    check(meta.depends[0].version == "2.34",    "depends[0].version");
    check(meta.depends[1].name == "libssl.so.3", "depends[1].name");
    check(meta.depends[1].op   == "=",          "depends[1].op (=)");

    // File entries
    check(meta.file_entries.size() == 2, "file_entries size==2");
    const FileEntry* bin  = nullptr;
    const FileEntry* read = nullptr;
    for (const auto& f : meta.file_entries) {
        if (f.path == "/usr/bin/hello")               bin  = &f;
        if (f.path == "/usr/share/doc/hello/README")  read = &f;
    }
    check(bin  != nullptr, "bin entry exists");
    check(read != nullptr, "readme entry exists");
    if (bin)  { check((bin->mode & 0777)  == 0755, "bin mode 0755");
                check(bin->content.size()  == 21,   "bin content size"); }
    if (read) { check((read->mode & 0777) == 0644, "readme mode 0644");
                check(read->content.size() == 12,  "readme content size"); }

    // Signature material: no RSAHEADER/DSAHEADER in this fixture
    check(!meta.embedded_signature.has_value(), "no embedded signature (unsigned fixture)");
    check(!meta.signed_payload.empty(),         "signed_payload (raw header bytes)");

    // --- Second fixture: BASENAMES + DIRINDEXES + DIRNAMES ---------------
    // This is the layout real el8/el9 RPMs use (they carry no FILENAMES=5007).
    // Also exercises symlink detection via the FILELINKTO array.
    {
        std::vector<TagData> t2;
        add_string(t2, 1000, "hello2");         // NAME
        add_string(t2, 1001, "1.0");            // VERSION
        add_string_array(t2, 1117, {"hello", "README", "link-to-readme"});  // BASENAMES
        add_int32_array (t2, 1116, {0, 1, 1});                              // DIRINDEXES
        add_string_array(t2, 1118, {"/usr/bin/", "/usr/share/doc/hello2/"}); // DIRNAMES
        add_int16_array (t2, 1030, {0100755, 0100644, 0120777});             // FILEMODES
        add_int32_array (t2, 1028, {21, 12, 14});                            // FILESIZES
        add_string_array(t2, 1036, {"", "", "README"});                      // FILELINKTO

        std::vector<std::string> p2 = {"usr/bin/hello", "usr/share/doc/hello2/README"};
        std::vector<std::string> c2 = {"#!/bin/sh\necho hello\n", "Hello world\n"};
        auto rpm2 = build_rpm(t2, p2, c2);
        PackageMeta m2 = parse_rpm_memory(rpm2.data(), rpm2.size());

        check(m2.name == "hello2", "fallback: name");
        check(m2.file_entries.size() == 3, "fallback: file_entries size==3");

        const FileEntry* b  = nullptr;
        const FileEntry* r  = nullptr;
        const FileEntry* ln = nullptr;
        for (const auto& f : m2.file_entries) {
            if (f.path == "/usr/bin/hello")                      b  = &f;
            if (f.path == "/usr/share/doc/hello2/README")        r  = &f;
            if (f.path == "/usr/share/doc/hello2/link-to-readme") ln = &f;
        }
        check(b  != nullptr, "fallback: /usr/bin/hello path rebuilt");
        check(r  != nullptr, "fallback: /usr/share/doc/hello2/README path rebuilt");
        check(ln != nullptr, "fallback: symlink entry exists");
        if (b) check(b->content.size() == 21, "fallback: bin content from payload");
        if (r) check(r->content.size() == 12, "fallback: readme content from payload");
        if (ln) {
            check(ln->is_symlink,                            "fallback: symlink detected via mode");
            check(ln->symlink_target == "README",            "fallback: FILELINKTO target");
            check(!ln->content.size(),                       "fallback: symlink has no content");
        }
    }

    // --- Parse from file path ----------------------------------
    std::string tmppath = "/tmp/ranarch-rpm-" + std::to_string(getpid()) + ".rpm";
    {
        std::ofstream ofs(tmppath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(rpm.data()), rpm.size());
    }
    PackageMeta meta2 = parse_rpm(tmppath);
    check(meta2.name    == "hello",      "parse_rpm(path) name");
    check(meta2.version == "2.10-2.el7",  "parse_rpm(path) version");
    check(meta2.file_entries.size() == 2, "parse_rpm(path) file count");
    ::unlink(tmppath.c_str());

    // --- Bad magic: garbage input throws -----------------------
    bool threw = false;
    try {
        uint8_t junk[96] = {};
        (void)parse_rpm_memory(junk, sizeof(junk));
    } catch (const Exception&) { threw = true; }
    check(threw, "garbage input throws ParseBadMagic");

    // --- Too-small file throws --------------------------------
    threw = false;
    try {
        uint8_t tiny[10] = {};
        (void)parse_rpm_memory(tiny, sizeof(tiny));
    } catch (const Exception&) { threw = true; }
    check(threw, "tiny input throws ParseTruncated");

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all rpm_parser tests passed\n";
    return 0;
}
