// SPDX-License-Identifier: MIT
// RanArch RD Installer — shared payload streaming primitives.
//
// Both the .deb and .rpm parsers need to walk their payload (tar / cpio)
// without buffering it, and hand each entry to the extractor. This header
// holds the entry type, the sink contract, and a libarchive driver they share.
//
// Memory profile: O(libarchive's block buffer) regardless of package size.
// File content never passes through this process's heap — libarchive writes it
// straight into the fd the sink supplies.
#pragma once

#include <archive.h>
#include <archive_entry.h>

#include <cstdint>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace ranarch {

// One payload entry as seen while streaming.
struct PayloadEntry {
    std::string path;            // normalised absolute path, e.g. /usr/bin/tree
    uint32_t    mode = 0;
    uint64_t    size = 0;
    bool        is_dir = false;
    bool        is_symlink = false;
    std::string symlink_target;  // valid iff is_symlink
};

// Called once per payload entry, in archive order.
//
// For a regular file the sink must return a writable fd; the driver then
// streams that entry's bytes straight into it and closes it. Return -1 to skip
// the content (e.g. when only collecting metadata). For directories and
// symlinks the sink is invoked with no fd to write and its return is ignored.
using PayloadSink = std::function<int(const PayloadEntry&)>;

// Sequential read callback for libarchive. The stream is stack-allocated by the
// caller and only has to outlive archive_read_free(), which happens before the
// owning function returns. Supplying no seek callback makes libarchive treat
// the stream as non-seekable, so it never rewinds — essential because the
// stream begins partway into the package file.
struct FdStream {
    int      fd = -1;
    uint64_t remaining = UINT64_MAX;  // bytes still readable (UINT64_MAX = to EOF)
};

inline ssize_t payload_fd_read(struct archive*, void* client, const void** buff) {
    static thread_local std::vector<uint8_t> buf(64 * 1024);
    auto* s = static_cast<FdStream*>(client);
    if (s->remaining == 0) return 0;
    std::size_t want = buf.size();
    if (s->remaining != UINT64_MAX && s->remaining < want)
        want = static_cast<std::size_t>(s->remaining);
    ssize_t n = ::read(s->fd, buf.data(), want);
    if (n <= 0) return n;
    s->remaining -= static_cast<uint64_t>(n);
    *buff = buf.data();
    return n;
}

// Drive libarchive over an already-open payload archive, invoking `sink` per
// entry. `normalize` maps an archive path to an absolute install path (and
// returns "" to skip the entry).
inline void run_payload_loop(struct archive* a, const PayloadSink& sink,
                             std::string (*normalize)(const char*)) {
    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        auto ft = archive_entry_filetype(entry);

        PayloadEntry pe;
        pe.path = normalize(archive_entry_pathname(entry));
        if (pe.path.empty()) continue;

        if (ft == AE_IFDIR) {
            pe.is_dir = true;
            pe.mode = static_cast<uint32_t>(archive_entry_mode(entry));
            sink(pe);
            continue;
        }
        if (ft == AE_IFLNK) {
            pe.is_symlink = true;
            pe.mode = static_cast<uint32_t>(archive_entry_mode(entry));
            const char* t = archive_entry_symlink(entry);
            pe.symlink_target = t ? t : "";
            sink(pe);
            continue;
        }
        if (ft != AE_IFREG) continue;  // skip devices / fifos / sockets

        pe.mode = static_cast<uint32_t>(archive_entry_mode(entry));
        pe.size = static_cast<uint64_t>(archive_entry_size(entry));
        int out = sink(pe);
        if (out >= 0) {
            // Stream the entry's bytes straight to the fd. A short write here
            // means a truncated install, so it is an error, not a warning.
            la_ssize_t written = archive_read_data_into_fd(a, out);
            ::close(out);
            if (written < 0) {
                const char* err = archive_error_string(a);
                throw std::runtime_error(
                    std::string("failed to write ") + pe.path + ": " +
                    (err ? err : "unknown archive error"));
            }
        }
    }
}

} // namespace ranarch
