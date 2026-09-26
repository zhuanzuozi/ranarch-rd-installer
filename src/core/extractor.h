// SPDX-License-Identifier: MIT
// RanArch RD Installer — file extractor (staging → atomic rename).
//
// Writes package file entries to the target root with path-traversal
// protection, parent-directory creation, symlink handling, and rollback.
#pragma once

#include "ranarch/types.h"

#include <string>
#include <vector>

namespace ranarch {

class Extractor {
public:
    // Extract files to `target_root`, taking content from the in-memory
    // FileEntry::content. Returns the list of installed absolute paths (for
    // rollback). Throws on path traversal or I/O errors.
    //
    // Prefer extract_package() for real packages: it streams instead of
    // holding every file in memory.
    std::vector<std::string> extract(const std::vector<FileEntry>& files,
                                     const std::string& target_root);

    // Extract a parsed package to `target_root`.
    //
    // When meta.source_path is set the payload is streamed straight from the
    // package file to disk with O(1) memory, so multi-hundred-MB packages
    // never have to be buffered. When it is empty (in-memory parse) this falls
    // back to extract(meta.file_entries, target_root).
    //
    // `check_path_traversal` mirrors ValidationOptions::path_traversal.
    std::vector<std::string> extract_package(const PackageMeta& meta,
                                             const std::string& target_root,
                                             bool check_path_traversal = true);

    // Remove files that were installed (rollback).
    static void rollback(const std::vector<std::string>& installed_paths,
                          const std::string& target_root);

    // Validate a path: must be absolute, no traversal above root.
    // Returns the normalized full path (target_root + path), or throws.
    //
    // `enabled` mirrors InstallOptions::check_path_traversal: when false the
    // check is skipped (dangerous — see InstallOptions).
    static std::string resolve_safe(const std::string& path,
                                     const std::string& target_root,
                                     bool enabled = true);
};

} // namespace ranarch
