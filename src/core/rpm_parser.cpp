// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — .rpm native parser implementation.
#include "rpm_parser.h"

#include "ranarch/error.h"

#include <archive.h>
#include <archive_entry.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ranarch {

namespace {

// ---- Big-endian readers -----------------------------------------------
uint16_t rd_be16(const uint8_t* p) {
    return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);
}
uint32_t rd_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

// ---- RPM tag constants ------------------------------------------------
enum RpmTag : uint32_t {
    // Header (metadata) tags
    RPMTAG_NAME            = 1000,
    RPMTAG_VERSION         = 1001,
    RPMTAG_RELEASE         = 1002,
    RPMTAG_SUMMARY         = 1004,
    RPMTAG_DESCRIPTION     = 1005,
    RPMTAG_FILESIZES       = 1028,
    RPMTAG_FILEMODES       = 1030,
    RPMTAG_FILELINKTO      = 1036,  // s[] symlink targets (parallel to FILEMODES)
    RPMTAG_REQUIREFLAGS    = 1048,
    RPMTAG_REQUIRENAME     = 1049,
    RPMTAG_REQUIREVERSION  = 1050,
    RPMTAG_DIRINDEXES      = 1116,  // l[]
    RPMTAG_BASENAMES       = 1117,  // s[]
    RPMTAG_DIRNAMES        = 1118,  // s[]
    // Full-path array, only present in rpm >= 4.12; otherwise the path is
    // reconstructed from BASENAMES + DIRINDEXES + DIRNAMES.
    RPMTAG_FILENAMES       = 5007,
    // Signature header tags (same numbers, different context)
    RPMSIGTAG_RSAHEADER    = 268,
    RPMSIGTAG_DSAHEADER     = 267,
};

// RPM data types
enum RpmType : uint32_t {
    RPM_NULL        = 0,
    RPM_CHAR        = 1,
    RPM_INT8        = 2,
    RPM_INT16       = 3,
    RPM_INT32       = 4,
    RPM_INT64       = 5,
    RPM_STRING      = 6,
    RPM_BIN         = 7,
    RPM_STRING_ARRAY = 8,
    RPM_I18NSTRING  = 9,
};

// RPM dependency sense flags
enum RpmSense : uint32_t {
    RPMSENSE_LESS      = 0x02,
    RPMSENSE_GREATER   = 0x04,
    RPMSENSE_EQUAL     = 0x08,
    RPMSENSE_INTERP    = 0x100,
    RPMSENSE_RPMLIB    = 0x1000,
};

// ---- Header parsing ---------------------------------------------------
struct IndexEntry {
    uint32_t tag;
    uint32_t type;
    uint32_t offset;
    uint32_t count;
};

struct ParsedHeader {
    uint32_t                 nindex = 0;
    std::vector<IndexEntry>  entries;
    std::vector<uint8_t>      data;

    const IndexEntry* find(uint32_t tag) const {
        for (const auto& e : entries)
            if (e.tag == tag) return &e;
        return nullptr;
    }

    std::string get_string(uint32_t tag) const {
        const auto* e = find(tag);
        if (!e) return "";
        if (e->offset >= data.size()) return "";
        const char* s = reinterpret_cast<const char*>(data.data() + e->offset);
        size_t maxlen = data.size() - e->offset;
        size_t len = strnlen(s, maxlen);
        return std::string(s, len);
    }

    std::string get_first_string(uint32_t tag) const {
        // For I18NSTRING / STRING_ARRAY, return the first element.
        return get_string(tag);
    }

    std::vector<std::string> get_string_array(uint32_t tag) const {
        std::vector<std::string> out;
        const auto* e = find(tag);
        if (!e) return out;
        size_t off = e->offset;
        for (uint32_t i = 0; i < e->count && off < data.size(); ++i) {
            const char* s = reinterpret_cast<const char*>(data.data() + off);
            size_t maxlen = data.size() - off;
            size_t len = strnlen(s, maxlen);
            out.emplace_back(s, len);
            off += len + 1;
        }
        return out;
    }

    std::vector<uint32_t> get_int32_array(uint32_t tag) const {
        std::vector<uint32_t> out;
        const auto* e = find(tag);
        if (!e || e->type != RPM_INT32) return out;
        size_t off = e->offset;
        for (uint32_t i = 0; i < e->count && off + 4 <= data.size(); ++i) {
            out.push_back(rd_be32(data.data() + off));
            off += 4;
        }
        return out;
    }

    std::vector<uint16_t> get_int16_array(uint32_t tag) const {
        std::vector<uint16_t> out;
        const auto* e = find(tag);
        if (!e || e->type != RPM_INT16) return out;
        size_t off = e->offset;
        for (uint32_t i = 0; i < e->count && off + 2 <= data.size(); ++i) {
            out.push_back(rd_be16(data.data() + off));
            off += 2;
        }
        return out;
    }

    std::vector<uint8_t> get_binary(uint32_t tag) const {
        const auto* e = find(tag);
        if (!e || e->type != RPM_BIN) return {};
        if (e->offset + e->count > data.size()) return {};
        return std::vector<uint8_t>(data.data() + e->offset,
                                    data.data() + e->offset + e->count);
    }
};

// Parse a header at `start` in `buf`. Returns the header and its total byte size.
// Preamble is 16 bytes:
//   magic(3) + version(1) + reserved(4) + il(4,BE) + dl(4,BE)
// where il = number of 16-byte index entries, dl = data-area length.
struct HeaderResult {
    ParsedHeader header;
    size_t       total_size = 0;  // 16 + il*16 + dl
};

HeaderResult parse_header(const uint8_t* buf, std::size_t size, std::size_t start) {
    if (start + 16 > size)
        throw Exception(RanArchError::ParseTruncated, "header preamble truncated");
    const uint8_t* p = buf + start;
    // magic: 0x8e 0xad 0xe8, then version byte (1)
    if (p[0] != 0x8e || p[1] != 0xad || p[2] != 0xe8)
        throw Exception(RanArchError::ParseBadMagic, "bad RPM header magic");
    if (p[3] != 1)
        throw Exception(RanArchError::ParseBadHeader, "unsupported RPM header version");

    // il at offset 8, dl at offset 12 (both big-endian).
    uint32_t nindex = rd_be32(p + 8);
    uint32_t hsize  = rd_be32(p + 12);

    std::size_t total = 16 + std::size_t(nindex) * 16 + hsize;
    if (start + total > size)
        throw Exception(RanArchError::ParseTruncated, "header data area truncated");

    HeaderResult hr;
    hr.header.nindex = nindex;
    hr.total_size    = total;

    // Index entries start right after the 16-byte preamble.
    for (uint32_t i = 0; i < nindex; ++i) {
        const uint8_t* ep = p + 16 + i * 16;
        IndexEntry e;
        e.tag    = rd_be32(ep);
        e.type   = rd_be32(ep + 4);
        e.offset = rd_be32(ep + 8);
        e.count  = rd_be32(ep + 12);
        hr.header.entries.push_back(e);
    }

    // Data area
    const uint8_t* dp = p + 16 + nindex * 16;
    hr.header.data.assign(dp, dp + hsize);

    return hr;
}

// ---- Dependency flags → operator string -------------------------------
std::string sense_to_op(uint32_t flags) {
    if (flags & RPMSENSE_RPMLIB) return "skip";  // internal rpmlib requirement
    if (flags & RPMSENSE_INTERP)  return "skip";  // interpreter (ld-linux), skip
    bool lt = flags & RPMSENSE_LESS;
    bool gt = flags & RPMSENSE_GREATER;
    bool eq = flags & RPMSENSE_EQUAL;
    if (lt && eq) return "<=";
    if (gt && eq) return ">=";
    if (lt)       return "<<";
    if (gt)       return ">>";
    if (eq)       return "=";
    return "";
}

// ---- Cpio payload extraction (libarchive) -----------------------------
std::vector<FileEntry> extract_cpio_files(const uint8_t* data, std::size_t size) {
    struct archive* a = archive_read_new();
    archive_read_support_format_cpio(a);
    archive_read_support_filter_all(a);
    if (archive_read_open_memory(a, data, size) != ARCHIVE_OK) {
        archive_read_free(a);
        throw Exception(RanArchError::ParseBadHeader, "cpio open failed");
    }
    std::vector<FileEntry> entries;
    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        auto ft = archive_entry_filetype(entry);
        if (ft == AE_IFDIR) continue;  // skip directories

        FileEntry fe;
        const char* raw = archive_entry_pathname(entry);
        if (!raw) continue;
        // RPM cpio paths start with "./" — normalise.
        std::string path = raw;
        if (path.size() >= 2 && path[0] == '.' && path[1] == '/') path.erase(0, 2);
        if (!path.empty() && path[0] == '/') path.erase(0, 1);
        if (path.empty()) continue;
        fe.path = "/" + path;
        fe.mode = static_cast<uint32_t>(archive_entry_mode(entry));
        fe.size = static_cast<uint64_t>(archive_entry_size(entry));

        if (ft == AE_IFLNK) {
            fe.is_symlink = true;
            const char* sym = archive_entry_symlink(entry);
            fe.symlink_target = sym ? sym : "";
        } else if (ft == AE_IFREG) {
            const void* blk;
            size_t bsize;
            la_int64_t off;
            while (archive_read_data_block(a, &blk, &bsize, &off) == ARCHIVE_OK) {
                fe.content.insert(fe.content.end(),
                    static_cast<const uint8_t*>(blk),
                    static_cast<const uint8_t*>(blk) + bsize);
            }
        }
        entries.push_back(std::move(fe));
    }
    archive_read_free(a);
    return entries;
}

// Normalise a cpio path from the payload: "./usr/bin/tree" → "/usr/bin/tree".
std::string normalize_rpm_path(const char* raw) {
    std::string path = raw ? raw : "";
    if (path.size() >= 2 && path[0] == '.' && path[1] == '/') path.erase(0, 2);
    if (!path.empty() && path[0] == '/') path.erase(0, 1);
    while (!path.empty() && path.back() == '/') path.pop_back();
    if (path.empty()) return "";
    return "/" + path;
}

// Normalise an RPM path: BASENAMES+DIRNAMES → full path.
std::string join_dir_base(const std::string& dir, const std::string& base) {
    std::string d = dir;
    if (!d.empty() && d.back() == '/') d.pop_back();
    return d + "/" + base;
}

} // namespace

namespace {

// Header-region parse shared by the memory and file entry points. `data` must
// start at byte offset 0 and cover at least the lead + signature header + main
// header; the payload is never touched here.
struct RpmHeaderRegion {
    PackageMeta meta;
    std::size_t payload_offset = 0;  // absolute offset of the payload
};

RpmHeaderRegion parse_rpm_header_region(const uint8_t* data, std::size_t size) {
    if (size < 96)
        throw Exception(RanArchError::ParseTruncated, "file smaller than RPM lead");

    // --- Lead (96 bytes) ----------------------------------------
    if (data[0] != 0xed || data[1] != 0xab ||
        data[2] != 0xee || data[3] != 0xdb)
        throw Exception(RanArchError::ParseBadMagic, "bad RPM lead magic");

    // --- Signature Header --------------------------------------
    std::size_t sig_start = 96;
    HeaderResult sig_hr = parse_header(data, size, sig_start);

    // Extract signature blob (RSAHEADER or DSAHEADER) for sig_verifier.
    std::vector<uint8_t> sig_blob;
    {
        auto rsa = sig_hr.header.get_binary(RPMSIGTAG_RSAHEADER);
        if (!rsa.empty()) sig_blob = std::move(rsa);
        else {
            auto dsa = sig_hr.header.get_binary(RPMSIGTAG_DSAHEADER);
            if (!dsa.empty()) sig_blob = std::move(dsa);
        }
    }

    // Pad to 8-byte alignment after the signature header.
    std::size_t sig_end = sig_start + sig_hr.total_size;
    std::size_t padding = (8 - (sig_end % 8)) % 8;
    std::size_t hdr_start = sig_end + padding;

    // --- Header (metadata) -------------------------------------
    HeaderResult hdr_hr = parse_header(data, size, hdr_start);

    // The raw header bytes are the signed payload for sig_verifier.
    const uint8_t* hdr_bytes = data + hdr_start;

    // --- Build PackageMeta -------------------------------------
    PackageMeta meta;
    meta.format = PackageFormat::Rpm;

    meta.name    = hdr_hr.header.get_string(RPMTAG_NAME);
    std::string ver  = hdr_hr.header.get_string(RPMTAG_VERSION);
    std::string rel  = hdr_hr.header.get_string(RPMTAG_RELEASE);
    meta.version = rel.empty() ? ver : (ver + "-" + rel);
    // Architecture (tag 1022) — optional, may be empty.
    meta.arch        = hdr_hr.header.get_string(1022);
    meta.summary     = hdr_hr.header.get_first_string(RPMTAG_SUMMARY);
    meta.description = hdr_hr.header.get_first_string(RPMTAG_DESCRIPTION);

    // --- Dependencies ------------------------------------------
    auto req_names    = hdr_hr.header.get_string_array(RPMTAG_REQUIRENAME);
    auto req_versions = hdr_hr.header.get_string_array(RPMTAG_REQUIREVERSION);
    auto req_flags    = hdr_hr.header.get_int32_array(RPMTAG_REQUIREFLAGS);

    std::size_t dep_count = req_names.size();
    for (std::size_t i = 0; i < dep_count; ++i) {
        uint32_t flags = i < req_flags.size() ? req_flags[i] : 0;
        std::string op = sense_to_op(flags);
        if (op == "skip") continue;  // rpmlib/interp internals

        Dependency d;
        d.name = req_names[i];
        if (d.name.substr(0, 7) == "rpmlib(" || d.name.substr(0, 5) == "rtld(")
            continue;
        d.op = op;
        if (i < req_versions.size()) d.version = req_versions[i];
        // Don't add deps with empty names (can happen with malformed data).
        if (d.name.empty()) continue;
        meta.depends.push_back(std::move(d));
    }

    // --- File entries ------------------------------------------
    // Resolve the per-file paths first: prefer the full-path FILENAMES array
    // (rpm >= 4.12), otherwise rebuild from BASENAMES + DIRINDEXES + DIRNAMES.
    // NB: do NOT confuse FILENAMES with FILELANGS(1097) — the latter is a
    // parallel array of per-file language tags and is empty for most packages.
    std::vector<std::string> paths;
    auto fnames = hdr_hr.header.get_string_array(RPMTAG_FILENAMES);
    if (!fnames.empty()) {
        for (const auto& p : fnames)
            paths.push_back(!p.empty() && p[0] == '/' ? p : "/" + p);
    } else {
        auto bases    = hdr_hr.header.get_string_array(RPMTAG_BASENAMES);
        auto dir_idxs = hdr_hr.header.get_int32_array(RPMTAG_DIRINDEXES);
        auto dirs     = hdr_hr.header.get_string_array(RPMTAG_DIRNAMES);
        for (std::size_t i = 0; i < bases.size(); ++i) {
            uint32_t di = i < dir_idxs.size() ? dir_idxs[i] : 0;
            std::string dir = di < dirs.size() ? dirs[di] : "";
            std::string p = join_dir_base(dir, bases[i]);
            if (!p.empty() && p[0] != '/') p = "/" + p;
            paths.push_back(std::move(p));
        }
    }

    auto fmodes = hdr_hr.header.get_int16_array(RPMTAG_FILEMODES);
    auto fsizes = hdr_hr.header.get_int32_array(RPMTAG_FILESIZES);
    auto flinks = hdr_hr.header.get_string_array(RPMTAG_FILELINKTO);

    for (std::size_t i = 0; i < paths.size(); ++i) {
        FileEntry fe;
        fe.path = paths[i];
        fe.mode = i < fmodes.size() ? fmodes[i] : 0;
        fe.size = i < fsizes.size() ? fsizes[i] : 0;
        // S_IFLNK == 0120000 — the target lives in the parallel FILELINKTO array.
        if ((fe.mode & 0170000) == 0120000 && i < flinks.size()) {
            fe.is_symlink     = true;
            fe.symlink_target = flinks[i];
        }
        meta.file_entries.push_back(std::move(fe));
    }

    // --- Signature material -----------------------------------
    if (!sig_blob.empty()) {
        meta.embedded_signature = std::move(sig_blob);
    }
    // signed_payload = raw header bytes (the immutable region lives here).
    meta.signed_payload.assign(hdr_bytes, hdr_bytes + hdr_hr.total_size);

    RpmHeaderRegion out;
    out.payload_offset = hdr_start + hdr_hr.total_size;
    out.meta = std::move(meta);
    return out;
}

// Read the lead + signature header + main header from `fd` (a few KB in
// practice) without touching the payload, so that multi-hundred-MB packages
// never have to be buffered. Uses pread, so no file offset is disturbed.
std::vector<uint8_t> read_rpm_header_region(int fd) {
    std::vector<uint8_t> buf;
    auto read_at = [&](std::size_t off, std::size_t len) {
        // Sanity bound: a header region is small; refuse absurd sizes so a
        // corrupt/lying il/dl cannot make us allocate gigabytes.
        if (len > 64u * 1024 * 1024)
            throw Exception(RanArchError::ParseBadHeader, "implausible RPM header size");
        buf.resize(off + len);
        std::size_t got = 0;
        while (got < len) {
            ssize_t n = ::pread(fd, buf.data() + off + got, len - got,
                                 static_cast<off_t>(off + got));
            if (n <= 0)
                throw Exception(RanArchError::ParseTruncated, "short read in RPM header");
            got += static_cast<std::size_t>(n);
        }
    };

    read_at(0, 96);                       // Lead
    read_at(96, 16);                      // Signature header preamble
    uint32_t il1 = rd_be32(buf.data() + 96 + 8);
    uint32_t dl1 = rd_be32(buf.data() + 96 + 12);
    std::size_t sig_total = 16 + std::size_t(il1) * 16 + dl1;
    read_at(96, sig_total);

    std::size_t sig_end  = 96 + sig_total;
    std::size_t pad      = (8 - (sig_end % 8)) % 8;
    std::size_t hdr_start = sig_end + pad;

    read_at(hdr_start, 16);               // Main header preamble
    uint32_t il2 = rd_be32(buf.data() + hdr_start + 8);
    uint32_t dl2 = rd_be32(buf.data() + hdr_start + 12);
    read_at(hdr_start, 16 + std::size_t(il2) * 16 + dl2);
    return buf;
}

} // namespace   // (header-region parsing)

PackageMeta parse_rpm_memory(const uint8_t* data, std::size_t size) {
    RpmHeaderRegion hr = parse_rpm_header_region(data, size);
    PackageMeta meta = std::move(hr.meta);

    // In-memory entry point: embed each file's content for the caller. Only
    // intended for small inputs (tests, buffers). Production code parses from a
    // file path so the payload can be streamed straight to disk instead.
    if (hr.payload_offset < size) {
        auto cpio_entries = extract_cpio_files(data + hr.payload_offset,
                                                size - hr.payload_offset);
        for (auto& ce : cpio_entries) {
            for (auto& fe : meta.file_entries) {
                if (fe.path == ce.path) {
                    fe.is_symlink     = ce.is_symlink;
                    fe.symlink_target = ce.symlink_target;
                    fe.content        = std::move(ce.content);
                    if (fe.mode == 0) fe.mode = ce.mode;
                    fe.size = ce.size;
                    break;
                }
            }
        }
    }
    meta.content_embedded = true;
    return meta;
}

PackageMeta parse_rpm(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw Exception(RanArchError::Io, "cannot open " + path);
    std::vector<uint8_t> hdr;
    try {
        hdr = read_rpm_header_region(fd);
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);

    RpmHeaderRegion hr = parse_rpm_header_region(hdr.data(), hdr.size());
    PackageMeta meta = std::move(hr.meta);
    meta.source_path      = path;
    meta.payload_offset   = hr.payload_offset;
    meta.content_embedded = false;
    return meta;
}

void stream_rpm_payload(const std::string& path, uint64_t payload_offset,
                        const PayloadSink& sink) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw Exception(RanArchError::Io, "cannot open " + path);
    if (::lseek(fd, static_cast<off_t>(payload_offset), SEEK_SET) < 0) {
        ::close(fd);
        throw Exception(RanArchError::Io, "seek to payload failed: " + path);
    }

    struct archive* a = archive_read_new();
    archive_read_support_format_cpio(a);
    archive_read_support_filter_all(a);

    FdStream st{fd, UINT64_MAX};  // stack lifetime covers the whole call
    if (archive_read_open2(a, &st, nullptr, payload_fd_read, nullptr,
                            nullptr) != ARCHIVE_OK) {
        std::string err = archive_error_string(a) ? archive_error_string(a)
                                                  : "payload open failed";
        archive_read_free(a);
        ::close(fd);
        throw Exception(RanArchError::ParseBadHeader, err);
    }

    run_payload_loop(a, sink, normalize_rpm_path);

    archive_read_free(a);
    ::close(fd);
}

} // namespace ranarch
