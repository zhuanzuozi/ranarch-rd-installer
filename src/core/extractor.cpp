// SPDX-License-Identifier: MIT
// RanArch RD Installer — file extractor implementation.
#include "extractor.h"

#include "deb_parser.h"
#include "payload.h"
#include "ranarch/error.h"
#include "rpm_parser.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ranarch {

namespace {

// Split a path into parent directory + leaf name.
std::string parent_dir(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos || pos == 0) return "/";
    return path.substr(0, pos);
}

// Create all parent directories (like mkdir -p).
bool mkdir_p(const std::string& path, mode_t mode) {
    std::string current;
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] == '/') {
            current = path.substr(0, i);
            if (!current.empty()) {
                ::mkdir(current.c_str(), mode);
                // Ignore EEXIST — the directory may already be there.
            }
        }
    }
    ::mkdir(path.c_str(), mode);
    return true;
}

} // namespace

std::string Extractor::resolve_safe(const std::string& path,
                                      const std::string& target_root,
                                      bool enabled) {
    // Path must be absolute.
    if (path.empty() || path[0] != '/')
        throw Exception(RanArchError::ExtractPathTraversal,
                        "path is not absolute: " + path);

    // Join target_root + path.
    std::string full = target_root;
    if (!full.empty() && full.back() == '/') full.pop_back();
    full += path;

    // Normalize: resolve ".." and "." components. The join above already made
    // `full` start with target_root, so ".." segments here can only be the
    // archive trying to climb out.
    std::vector<std::string> parts;
    std::string part;
    for (size_t i = 0; i < full.size(); ++i) {
        if (full[i] == '/') {
            if (!part.empty()) {
                if (part == "..") {
                    // Refuse to pop past target_root. Only meaningful when the
                    // traversal check is on; otherwise we keep the ".." so the
                    // path resolves relative to the root as the archive asked.
                    if (!parts.empty() && parts.back() != "..")
                        parts.pop_back();
                    else if (enabled)
                        throw Exception(RanArchError::ExtractPathTraversal,
                                        "path escapes target root: " + path);
                    else
                        parts.push_back("..");
                } else if (part != ".") {
                    parts.push_back(part);
                }
                part.clear();
            }
        } else {
            part += full[i];
        }
    }
    if (!part.empty() && part != "." && part != "..") parts.push_back(part);

    std::string result;
    for (const auto& p : parts) {
        result += "/" + p;
    }
    if (result.empty()) result = "/";

    if (!enabled) return result;  // check disabled by the caller

    // Verify result starts with target_root (normalised).
    std::string root = target_root;
    if (!root.empty() && root.back() == '/') root.pop_back();
    if (result.size() < root.size() || result.compare(0, root.size(), root) != 0) {
        throw Exception(RanArchError::ExtractPathTraversal,
                        "path escapes target root: " + path);
    }

    return result;
}

std::vector<std::string> Extractor::extract(const std::vector<FileEntry>& files,
                                              const std::string& target_root) {
    std::vector<std::string> installed;
    installed.reserve(files.size());

    for (const auto& f : files) {
        std::string full = resolve_safe(f.path, target_root);

        // Create parent directories.
        std::string parent = parent_dir(full);
        mkdir_p(parent, 0755);

        if (f.is_symlink) {
            // Remove existing entry at the target path.
            ::unlink(full.c_str());
            if (symlink(f.symlink_target.c_str(), full.c_str()) != 0) {
                throw Exception(RanArchError::ExtractPermission,
                                "symlink failed: " + full + ": " +
                                std::strerror(errno));
            }
        } else {
            // Write file content.
            ::unlink(full.c_str());  // remove existing
            std::ofstream ofs(full, std::ios::binary);
            if (!ofs)
                throw Exception(RanArchError::ExtractPermission,
                                "open failed: " + full);
            if (!f.content.empty())
                ofs.write(reinterpret_cast<const char*>(f.content.data()),
                          f.content.size());
            ofs.close();
            // Set permissions.
            ::chmod(full.c_str(), f.mode & 0777);
        }

        installed.push_back(full);
    }

    return installed;
}

std::vector<std::string> Extractor::extract_package(const PackageMeta& meta,
                                                     const std::string& target_root,
                                                     bool check_path_traversal) {
    // In-memory parse (no source file): fall back to the buffered path.
    if (meta.source_path.empty())
        return extract(meta.file_entries, target_root);

    std::vector<std::string> installed;
    // (path, mode) pairs applied after the whole payload is written, so that a
    // read-only directory cannot block files created inside it.
    std::vector<std::pair<std::string, uint32_t>> pending_chmod;

    PayloadSink sink = [&](const PayloadEntry& pe) -> int {
        std::string full = resolve_safe(pe.path, target_root, check_path_traversal);

        if (pe.is_dir) {
            mkdir_p(full, 0755);
            pending_chmod.emplace_back(full, pe.mode & 07777);
            installed.push_back(full);
            return -1;
        }

        std::string parent = parent_dir(full);
        mkdir_p(parent, 0755);
        ::unlink(full.c_str());  // replace whatever was there

        if (pe.is_symlink) {
            if (::symlink(pe.symlink_target.c_str(), full.c_str()) != 0) {
                throw Exception(RanArchError::ExtractPermission,
                                "symlink failed: " + full + ": " +
                                std::strerror(errno));
            }
            installed.push_back(full);
            return -1;
        }

        // Regular file: hand the driver a fresh fd; it streams the content in.
        int fd = ::open(full.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) {
            throw Exception(RanArchError::ExtractPermission,
                            "open failed: " + full + ": " +
                            std::strerror(errno));
        }
        installed.push_back(full);
        pending_chmod.emplace_back(full, pe.mode & 07777);
        return fd;   // driver writes here and closes it
    };

    if (meta.format == PackageFormat::Rpm) {
        stream_rpm_payload(meta.source_path, meta.payload_offset, sink);
    } else if (meta.format == PackageFormat::Deb) {
        stream_deb_payload(meta.source_path, sink);
    } else {
        throw Exception(RanArchError::ParseBadMagic,
                        "unknown package format for " + meta.source_path);
    }

    for (const auto& [path, mode] : pending_chmod) {
        if (mode != 0) ::chmod(path.c_str(), mode);
    }
    return installed;
}

void Extractor::rollback(const std::vector<std::string>& installed_paths,
                           const std::string& target_root) {
    for (auto it = installed_paths.rbegin(); it != installed_paths.rend(); ++it) {
        // Only remove if the path is within target_root (safety check).
        try {
            resolve_safe(*it, target_root);
            ::unlink(it->c_str());
        } catch (...) {
            // Skip paths that fail the safety check.
        }
    }
}

} // namespace ranarch
