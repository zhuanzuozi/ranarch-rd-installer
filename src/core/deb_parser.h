// SPDX-License-Identifier: MIT
// RanArch RD Installer — .deb native parser.
//
// A .deb is an `ar` archive with three members:
//   - debian-binary      : "2.0\n"
//   - control.tar.{gz,xz,zst} : control file + maintainer scripts + optional _gpgorigin
//   - data.tar.{gz,xz,zst}    : actual file tree
//
// Two entry points exist, matching the .rpm parser:
//   parse_deb(path)         — production path. Walks the ar members by offset
//                             and streams the data member, so memory stays
//                             O(block buffer) no matter how big the package is.
//                             Payload content is left on disk.
//   parse_deb_memory(buf)   — convenience path for small in-memory inputs
//                             (primarily tests). Embeds content and the
//                             signed payload.
#pragma once

#include "payload.h"
#include "ranarch/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ranarch {

// One ar member located within a .deb file.
struct DebMemberRef {
    std::string name;    // member name, without the trailing '/'
    uint64_t    offset = 0;  // absolute byte offset of the member's data
    uint64_t    size = 0;    // member data length
};

// Walk the ar headers of `path` and return every member's name/offset/size.
// Reads only the 60-byte ar headers — the members themselves are not read.
// Throws RanArchError::ParseBadMagic / ParseTruncated on a malformed archive.
std::vector<DebMemberRef> scan_deb_members(const std::string& path);

// Parse a .deb file at `path` and return its metadata + file entries.
// The data member is streamed for metadata only; content stays on disk and can
// be extracted later with stream_deb_payload(meta.source_path, sink).
PackageMeta parse_deb(const std::string& path);

// Parse a .deb from an in-memory buffer. Embeds each file's content in
// PackageMeta::file_entries and sets content_embedded = true.
PackageMeta parse_deb_memory(const uint8_t* data, std::size_t size);

// Stream a .deb's data payload, invoking `sink` per entry with O(1) memory.
void stream_deb_payload(const std::string& path, const PayloadSink& sink);

// The bytes a .deb's _gpgorigin signature covers, in order:
//   debian-binary || control.tar.* || data.tar.*   (raw ar member bytes)
// Returned as (offset, size) pairs so a verifier can stream them without
// loading the package into memory. Empty if any of the three is missing.
std::vector<DebMemberRef> deb_signed_regions(const std::string& path);

} // namespace ranarch
