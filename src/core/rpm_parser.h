// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — .rpm native parser (no librpm dependency).
//
// RPM file layout:
//   1. Lead           (96 bytes; magic 0xed 0xab 0xee 0xdb) — verified, ignored.
//   2. Signature Header (preamble + index + data; padded to 8-byte boundary).
//   3. Header         (preamble + index + data) — the real metadata.
//   4. Payload         (cpio archive, gzip/xz/zstd compressed) — file tree.
//
// Header structure (both sig + regular):
//   12-byte preamble: magic(3)=0x8e 0xad 0xe8 + reserved(1) + nindex(4,BE) + hsize(4,BE)
//   then nindex * 16-byte index entries: tag(4) + type(4) + offset(4) + count(4) (all BE)
//   then hsize bytes of data area.
//
// Extracts: name, version (version-release), arch, summary, description, depends,
// file_entries, and (optionally) embedded_signature (RSAHEADER/DSAHEADER blob) +
// signed_payload (raw header bytes for immutable-region verification).
#pragma once

#include "payload.h"
#include "ranarch/types.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace ranarch {

// Parse a .rpm file at `path` and return its metadata + file entries.
// Only the header region is read — the payload is left on disk and can be
// streamed later with stream_rpm_payload(meta.source_path, meta.payload_offset).
// Throws RanArchError::ParseBadMagic / ParseTruncated / ParseBadHeader.
PackageMeta parse_rpm(const std::string& path);

// Parse a .rpm from an in-memory buffer. Convenience entry point for small
// inputs (primarily tests): it embeds each file's content in
// PackageMeta::file_entries and sets content_embedded = true.
PackageMeta parse_rpm_memory(const uint8_t* data, std::size_t size);

// Stream the cpio payload of a .rpm, invoking `sink` per entry with O(1) memory.
// `payload_offset` comes from PackageMeta::payload_offset.
void stream_rpm_payload(const std::string& path, uint64_t payload_offset,
                        const PayloadSink& sink);

} // namespace ranarch
