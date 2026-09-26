// SPDX-License-Identifier: MIT
// RanArch RD Installer — .deb native parser implementation.
#include "deb_parser.h"

#include "ranarch/error.h"

#include <archive.h>
#include <archive_entry.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace ranarch {

namespace {

struct ArMember {
    std::string            name;
    std::vector<uint8_t>   data;
};

// Read all data blocks from the current archive entry into a buffer.
std::vector<uint8_t> read_entry_data(struct archive* a) {
    std::vector<uint8_t> buf;
    const void*  blk;
    size_t       size;
    la_int64_t   off;
    while (archive_read_data_block(a, &blk, &size, &off) == ARCHIVE_OK) {
        buf.insert(buf.end(),
                   static_cast<const uint8_t*>(blk),
                   static_cast<const uint8_t*>(blk) + size);
    }
    return buf;
}

// Normalise a tar entry path to an absolute install target path.
// "./usr/bin/foo" → "/usr/bin/foo"; "usr/bin/foo" → "/usr/bin/foo".
std::string normalize_path(std::string p) {
    if (p.size() >= 2 && p[0] == '.' && p[1] == '/') p.erase(0, 2);
    if (!p.empty() && p[0] == '/') p.erase(0, 1);
    while (!p.empty() && p.back() == '/') p.pop_back();
    if (p.empty()) return p;
    return "/" + p;
}

// Parse a single dependency token: "libfoo (>= 1.2)" or just "libfoo".
Dependency parse_dep_token(const std::string& token) {
    Dependency d;
    auto lp = token.find('(');
    if (lp == std::string::npos) {
        d.name = token;
        while (!d.name.empty() && d.name.back()  == ' ') d.name.pop_back();
        return d;
    }
    d.name = token.substr(0, lp);
    while (!d.name.empty() && d.name.back()  == ' ') d.name.pop_back();
    auto rp = token.find(')', lp);
    if (rp == std::string::npos) return d;
    std::string c = token.substr(lp + 1, rp - lp - 1);
    while (!c.empty() && c.front() == ' ') c.erase(0, 1);
    while (!c.empty() && c.back()  == ' ') c.pop_back();
    size_t i = 0;
    while (i < c.size() && (c[i] == '>' || c[i] == '<' || c[i] == '=')) ++i;
    d.op      = c.substr(0, i);
    d.version = c.substr(i);
    while (!d.version.empty() && d.version.front() == ' ') d.version.erase(0, 1);
    while (!d.version.empty() && d.version.back()  == ' ') d.version.pop_back();
    return d;
}

// Parse a comma-separated Depends field into Dependency entries.
std::vector<Dependency> parse_depends_field(const std::string& field) {
    std::vector<Dependency> deps;
    std::string tok;
    for (char c : field) {
        if (c == ',') {
            while (!tok.empty() && tok.front() == ' ') tok.erase(0, 1);
            while (!tok.empty() && tok.back()  == ' ') tok.pop_back();
            if (!tok.empty()) deps.push_back(parse_dep_token(tok));
            tok.clear();
        } else {
            tok += c;
        }
    }
    while (!tok.empty() && tok.front() == ' ') tok.erase(0, 1);
    while (!tok.empty() && tok.back()  == ' ') tok.pop_back();
    if (!tok.empty()) deps.push_back(parse_dep_token(tok));
    return deps;
}

// Parse a Debian control file (first paragraph only) into a key→value map.
std::map<std::string, std::string> parse_control(const std::string& text) {
    std::map<std::string, std::string> fields;
    std::istringstream ss(text);
    std::string line, key, val;
    auto flush = [&]() {
        if (key.empty()) return;
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
            val.pop_back();
        fields[key] = val;
        key.clear(); val.clear();
    };
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) { flush(); if (!fields.empty()) break; continue; }
        if (line[0] == ' ' || line[0] == '\t') {
            val += "\n" + line.substr(1);
            continue;
        }
        flush();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        key = line.substr(0, colon);
        val = line.substr(colon + 1);
        while (!val.empty() && val.front() == ' ') val.erase(0, 1);
    }
    flush();
    return fields;
}

// Read all ar members from an opened ar archive.
std::vector<ArMember> read_ar_members(struct archive* a) {
    std::vector<ArMember> members;
    struct archive_entry* entry;
    while (true) {
        int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r != ARCHIVE_OK) {
            throw Exception(RanArchError::ParseBadHeader,
                            archive_error_string(a) ? archive_error_string(a)
                                                    : "ar read error");
        }
        ArMember m;
        m.name = archive_entry_pathname(entry) ? archive_entry_pathname(entry) : "";
        m.data = read_entry_data(a);
        members.push_back(std::move(m));
    }
    return members;
}

const ArMember* find_member_prefix(const std::vector<ArMember>& ms,
                                    const std::string& prefix) {
    for (const auto& m : ms)
        if (m.name.size() >= prefix.size() &&
            m.name.compare(0, prefix.size(), prefix) == 0)
            return &m;
    return nullptr;
}

const ArMember* find_member_exact(const std::vector<ArMember>& ms,
                                    const std::string& name) {
    for (const auto& m : ms)
        if (m.name == name) return &m;
    return nullptr;
}

// Parse a tar+compression stream from memory and return its archive handle.
// Caller must archive_read_free() the returned handle.
struct archive* open_tar_memory(const uint8_t* data, std::size_t size) {
    struct archive* a = archive_read_new();
    archive_read_support_format_tar(a);
    archive_read_support_filter_all(a);
    if (archive_read_open_memory(a, data, size) != ARCHIVE_OK) {
        std::string err = archive_error_string(a) ? archive_error_string(a)
                                                  : "tar open failed";
        archive_read_free(a);
        throw Exception(RanArchError::ParseBadHeader, err);
    }
    return a;
}

// Extract the `control` file text from a control.tar.* member's bytes.
std::string extract_control_text(const uint8_t* data, std::size_t size) {
    struct archive* a = open_tar_memory(data, size);
    std::string control_text;
    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        std::string name = archive_entry_pathname(entry)
                         ? archive_entry_pathname(entry) : "";
        if (name.size() >= 2 && name[0] == '.' && name[1] == '/') name.erase(0, 2);
        if (name == "control") {
            auto bytes = read_entry_data(a);
            control_text.assign(reinterpret_cast<const char*>(bytes.data()),
                                bytes.size());
            break;
        }
    }
    archive_read_free(a);
    if (control_text.empty())
        throw Exception(RanArchError::ParseBadHeader,
                        "no control file in control.tar");
    return control_text;
}

// Extract file entries from a data.tar.* member.
std::vector<FileEntry> extract_file_entries(const ArMember& m) {
    struct archive* a = open_tar_memory(m.data.data(), m.data.size());
    std::vector<FileEntry> entries;
    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        auto ft = archive_entry_filetype(entry);
        if (ft == AE_IFDIR) continue;  // directories are created on the fly

        FileEntry fe;
        const char* raw = archive_entry_pathname(entry);
        if (!raw) continue;
        fe.path = normalize_path(raw);
        if (fe.path.empty()) continue;
        fe.mode = static_cast<uint32_t>(archive_entry_mode(entry));
        fe.size = static_cast<uint64_t>(archive_entry_size(entry));

        if (ft == AE_IFLNK) {
            fe.is_symlink   = true;
            fe.symlink_target = archive_entry_symlink(entry)
                              ? archive_entry_symlink(entry) : "";
        } else if (ft == AE_IFREG) {
            fe.content = read_entry_data(a);
        }
        entries.push_back(std::move(fe));
    }
    archive_read_free(a);
    return entries;
}

} // namespace

PackageMeta parse_deb_memory(const uint8_t* data, std::size_t size) {
    // --- outer ar archive -----------------------------------------
    struct archive* a = archive_read_new();
    archive_read_support_format_ar(a);
    if (archive_read_open_memory(a, data, size) != ARCHIVE_OK) {
        std::string err = archive_error_string(a) ? archive_error_string(a)
                                                  : "not an ar archive";
        archive_read_free(a);
        throw Exception(RanArchError::ParseBadMagic, err);
    }
    std::vector<ArMember> members;
    try {
        members = read_ar_members(a);
    } catch (...) {
        archive_read_free(a);
        throw;
    }
    archive_read_free(a);

    if (members.empty())
        throw Exception(RanArchError::ParseTruncated, "deb has no ar members");

    // --- locate the three required members ------------------------
    const ArMember* dbin = find_member_exact(members, "debian-binary");
    if (!dbin)
        throw Exception(RanArchError::ParseBadMagic,
                        "missing debian-binary member");
    if (dbin->data.empty() || dbin->data[0] != '2')
        throw Exception(RanArchError::ParseBadHeader,
                        "unsupported deb format version");

    const ArMember* ctrl = find_member_prefix(members, "control.tar");
    if (!ctrl)
        throw Exception(RanArchError::ParseBadHeader, "missing control.tar member");

    const ArMember* datam = find_member_prefix(members, "data.tar");
    if (!datam)
        throw Exception(RanArchError::ParseBadHeader, "missing data.tar member");

    // --- build PackageMeta ----------------------------------------
    PackageMeta meta;
    meta.format = PackageFormat::Deb;

    std::string control_text = extract_control_text(ctrl->data.data(),
                                                     ctrl->data.size());
    auto fields = parse_control(control_text);
    if (auto it = fields.find("Package");       it != fields.end()) meta.name        = it->second;
    if (auto it = fields.find("Version");       it != fields.end()) meta.version     = it->second;
    if (auto it = fields.find("Architecture");  it != fields.end()) meta.arch        = it->second;
    if (auto it = fields.find("Description");   it != fields.end()) {
        meta.description = it->second;
        auto nl = meta.description.find('\n');
        meta.summary = (nl == std::string::npos) ? meta.description
                                                 : meta.description.substr(0, nl);
    }
    if (auto it = fields.find("Depends");        it != fields.end())
        meta.depends = parse_depends_field(it->second);

    meta.file_entries = extract_file_entries(*datam);

    // --- signature material --------------------------------------
    if (const ArMember* sig = find_member_exact(members, "_gpgorigin")) {
        meta.embedded_signature = sig->data;
    }
    // signed_payload = debian-binary || control.tar.* || data.tar.* (raw member
    // bytes, in ar order). This is what _gpgorigin signs as a detached sig.
    meta.signed_payload.clear();
    meta.signed_payload.insert(meta.signed_payload.end(),
        dbin->data.begin(), dbin->data.end());
    meta.signed_payload.insert(meta.signed_payload.end(),
        ctrl->data.begin(),  ctrl->data.end());
    meta.signed_payload.insert(meta.signed_payload.end(),
        datam->data.begin(), datam->data.end());

    meta.content_embedded = true;
    return meta;
}

// ===========================================================================
// Streaming (file-based) path
// ===========================================================================

namespace {

// Normalise a tar path from the payload: "./usr/bin/foo" → "/usr/bin/foo".
std::string normalize_tar_path(const char* raw) {
    std::string p = raw ? raw : "";
    if (p.size() >= 2 && p[0] == '.' && p[1] == '/') p.erase(0, 2);
    if (!p.empty() && p[0] == '/') p.erase(0, 1);
    while (!p.empty() && p.back() == '/') p.pop_back();
    if (p.empty()) return "";
    return "/" + p;
}

// Read exactly [off, off+len) from `fd`.
std::vector<uint8_t> read_file_region(int fd, uint64_t off, uint64_t len) {
    std::vector<uint8_t> buf(static_cast<std::size_t>(len));
    std::size_t got = 0;
    while (got < buf.size()) {
        ssize_t n = ::pread(fd, buf.data() + got, buf.size() - got,
                             static_cast<off_t>(off + got));
        if (n <= 0)
            throw Exception(RanArchError::ParseTruncated, "short read in .deb member");
        got += static_cast<std::size_t>(n);
    }
    return buf;
}

// First member whose name starts with `prefix` (e.g. "control.tar").
const DebMemberRef* find_deb_member(const std::vector<DebMemberRef>& ms,
                                     const std::string& prefix) {
    for (const auto& m : ms)
        if (m.name.size() >= prefix.size() &&
            m.name.compare(0, prefix.size(), prefix) == 0)
            return &m;
    return nullptr;
}

const DebMemberRef* find_deb_member_exact(const std::vector<DebMemberRef>& ms,
                                           const std::string& name) {
    for (const auto& m : ms)
        if (m.name == name) return &m;
    return nullptr;
}

// Map a Debian control field block onto PackageMeta.
void apply_control_fields(PackageMeta& meta, const std::string& control_text) {
    auto fields = parse_control(control_text);
    if (auto it = fields.find("Package");      it != fields.end()) meta.name    = it->second;
    if (auto it = fields.find("Version");      it != fields.end()) meta.version = it->second;
    if (auto it = fields.find("Architecture"); it != fields.end()) meta.arch    = it->second;
    if (auto it = fields.find("Description");  it != fields.end()) {
        meta.description = it->second;
        auto nl = meta.description.find('\n');
        meta.summary = (nl == std::string::npos) ? meta.description
                                                 : meta.description.substr(0, nl);
    }
    if (auto it = fields.find("Depends"); it != fields.end())
        meta.depends = parse_depends_field(it->second);
}

} // namespace

std::vector<DebMemberRef> scan_deb_members(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw Exception(RanArchError::Io, "cannot open " + path);

    std::vector<DebMemberRef> out;
    try {
        struct stat st{};
        if (::fstat(fd, &st) != 0 || st.st_size < 8)
            throw Exception(RanArchError::ParseTruncated, "file too small: " + path);
        uint64_t file_size = static_cast<uint64_t>(st.st_size);

        uint8_t magic[8];
        if (::pread(fd, magic, 8, 0) != 8 || std::memcmp(magic, "!<arch>\n", 8) != 0)
            throw Exception(RanArchError::ParseBadMagic, "not an ar archive: " + path);

        uint64_t pos = 8;
        while (pos + 60 <= file_size) {
            uint8_t hdr[60];
            if (::pread(fd, hdr, 60, static_cast<off_t>(pos)) != 60) break;
            if (hdr[58] != '`' || hdr[59] != '\n') break;  // malformed header terminator

            std::string name(reinterpret_cast<const char*>(hdr), 16);
            while (!name.empty() && (name.back() == ' ' || name.back() == '/'))
                name.pop_back();
            // A leading '/' or "#1/" means the GNU/BSD long-name table, which
            // .deb never uses — stopping here is safe.
            if (name.empty() || name[0] == '/') break;

            std::string size_str(reinterpret_cast<const char*>(hdr + 48), 10);
            while (!size_str.empty() && size_str.back() == ' ') size_str.pop_back();
            if (size_str.empty()) break;
            uint64_t size = std::strtoull(size_str.c_str(), nullptr, 10);

            DebMemberRef ref;
            ref.name   = name;
            ref.offset = pos + 60;
            ref.size   = size;
            if (ref.offset + ref.size > file_size) break;  // truncated member
            out.push_back(std::move(ref));

            pos = ref.offset + ref.size + (ref.size % 2);  // members are 2-byte aligned
        }
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
    return out;
}

std::vector<DebMemberRef> deb_signed_regions(const std::string& path) {
    auto members = scan_deb_members(path);
    const DebMemberRef* dbin = find_deb_member_exact(members, "debian-binary");
    const DebMemberRef* ctrl = find_deb_member(members, "control.tar");
    const DebMemberRef* data = find_deb_member(members, "data.tar");
    if (!dbin || !ctrl || !data) return {};
    return {*dbin, *ctrl, *data};
}

void stream_deb_payload(const std::string& path, const PayloadSink& sink) {
    auto members = scan_deb_members(path);
    const DebMemberRef* data = find_deb_member(members, "data.tar");
    if (!data)
        throw Exception(RanArchError::ParseBadHeader, "missing data.tar member: " + path);

    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw Exception(RanArchError::Io, "cannot open " + path);
    if (::lseek(fd, static_cast<off_t>(data->offset), SEEK_SET) < 0) {
        ::close(fd);
        throw Exception(RanArchError::Io, "seek to data.tar failed: " + path);
    }

    struct archive* a = archive_read_new();
    archive_read_support_format_tar(a);
    archive_read_support_filter_all(a);

    FdStream st{fd, data->size};  // stack lifetime covers the whole call
    if (archive_read_open2(a, &st, nullptr, payload_fd_read, nullptr,
                            nullptr) != ARCHIVE_OK) {
        std::string err = archive_error_string(a) ? archive_error_string(a)
                                                  : "data.tar open failed";
        archive_read_free(a);
        ::close(fd);
        throw Exception(RanArchError::ParseBadHeader, err);
    }

    run_payload_loop(a, sink, normalize_tar_path);

    archive_read_free(a);
    ::close(fd);
}

PackageMeta parse_deb(const std::string& path) {
    auto members = scan_deb_members(path);

    const DebMemberRef* dbin = find_deb_member_exact(members, "debian-binary");
    if (!dbin)
        throw Exception(RanArchError::ParseBadMagic, "missing debian-binary member");
    const DebMemberRef* ctrl = find_deb_member(members, "control.tar");
    if (!ctrl)
        throw Exception(RanArchError::ParseBadHeader, "missing control.tar member");
    const DebMemberRef* data = find_deb_member(members, "data.tar");
    if (!data)
        throw Exception(RanArchError::ParseBadHeader, "missing data.tar member");

    PackageMeta meta;
    meta.format           = PackageFormat::Deb;
    meta.source_path      = path;
    meta.content_embedded = false;

    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw Exception(RanArchError::Io, "cannot open " + path);
    try {
        // debian-binary and control.tar are small; read them into memory.
        auto dbin_bytes = read_file_region(fd, dbin->offset, dbin->size);
        if (dbin_bytes.empty() || dbin_bytes[0] != '2')
            throw Exception(RanArchError::ParseBadHeader,
                            "unsupported deb format version");

        auto ctrl_bytes = read_file_region(fd, ctrl->offset, ctrl->size);
        apply_control_fields(meta,
            extract_control_text(ctrl_bytes.data(), ctrl_bytes.size()));

        if (const DebMemberRef* sig = find_deb_member_exact(members, "_gpgorigin"))
            meta.embedded_signature = read_file_region(fd, sig->offset, sig->size);
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);

    // The file list lives in data.tar, so stream it for metadata only. The
    // sink returns -1, telling the driver to skip each entry's content.
    std::vector<FileEntry> entries;
    stream_deb_payload(path, [&entries](const PayloadEntry& pe) -> int {
        FileEntry fe;
        fe.path           = pe.path;
        fe.mode           = pe.mode;
        fe.size           = pe.size;
        fe.is_symlink     = pe.is_symlink;
        fe.symlink_target = pe.symlink_target;
        entries.push_back(std::move(fe));
        return -1;  // metadata only
    });
    meta.file_entries = std::move(entries);

    // signed_payload stays empty for the file-based path; SigVerifier falls
    // back to streaming the ar members via deb_signed_regions().
    return meta;
}

} // namespace ranarch
