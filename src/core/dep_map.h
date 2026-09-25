// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — deb/rpm → Arch package name mapping table.
//
// Loads mappings from a CSV file (format: source_format,source_name,arch_name)
// at startup, falling back to a built-in default table. User-specified
// overrides (from the DB's dep_overrides table) take precedence over both.
#pragma once

#include <map>
#include <optional>
#include <string>

namespace ranarch {

class DepMap {
public:
    // Load mappings from a CSV file. Lines starting with '#' are comments.
    // Returns false if the file could not be opened.
    bool load_file(const std::string& path);

    // Load the built-in default mappings (compiled into the binary).
    void load_builtin();

    // Set a user override (takes precedence over file/builtin mappings).
    void set_override(const std::string& src_fmt, const std::string& src_name,
                      const std::string& arch_name);

    // Look up the Arch package name for a deb/rpm dependency.
    // Returns nullopt if no mapping exists.
    std::optional<std::string> lookup(const std::string& src_fmt,
                                      const std::string& src_name) const;

    // Number of entries currently loaded.
    std::size_t size() const { return m_map.size(); }

private:
    // Key: "fmt:name"  →  value: "arch_pkg_name"
    std::map<std::string, std::string> m_map;

    static std::string make_key(const std::string& fmt, const std::string& name) {
        return fmt + ":" + name;
    }
};

} // namespace ranarch
