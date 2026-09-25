// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — deb/rpm → Arch package name mapping table impl.
#include "dep_map.h"

#include <fstream>
#include <sstream>

namespace ranarch {

namespace {

// Built-in default mapping table. This is the fallback when no CSV file is
// found. It mirrors data/dep_map.csv and covers the most common deb/rpm
// dependency names. Extend both the CSV and this table together.
const char* BUILTIN_MAP =
    "deb,libc6,glibc\n"
    "deb,libssl1.1,openssl-1.1\n"
    "deb,libssl3,openssl\n"
    "deb,libcurl4,curl\n"
    "deb,libxml2,libxml2\n"
    "deb,libzstd1,zstd\n"
    "deb,zlib1g,zlib\n"
    "deb,libgcc-s1,libgcc\n"
    "deb,libstdc++6,gcc-libs\n"
    "deb,libsystemd0,systemd-libs\n"
    "deb,libdbus-1-3,dbus\n"
    "deb,libglib2.0-0,glib2\n"
    "deb,libpcre3,pcre\n"
    "deb,libexpat1,expat\n"
    "deb,libffi8,libffi\n"
    "deb,libpng16-16,libpng\n"
    "deb,libjpeg-turbo8,libjpeg-turbo\n"
    "deb,libfreetype6,freetype2\n"
    "deb,libfontconfig1,fontconfig\n"
    "deb,libx11-6,libx11\n"
    "deb,libxcb1,libxcb\n"
    "deb,python3,python\n"
    "deb,perl,perl\n"
    "deb,bash,bash\n"
    "deb,coreutils,coreutils\n"
    "deb,libncursesw6,ncurses\n"
    "deb,libreadline8,readline\n"
    "deb,libsqlite3-0,sqlite\n"
    "deb,libarchive13,libarchive\n"
    "deb,libgpgme11,gpgme\n"
    "deb,libalpm13,pacman\n"
    "rpm,glibc,glibc\n"
    "rpm,openssl-libs,openssl\n"
    "rpm,libcurl,curl\n"
    "rpm,libxml2,libxml2\n"
    "rpm,zstd-libs,zstd\n"
    "rpm,zlib,zlib\n"
    "rpm,libgcc,libgcc\n"
    "rpm,libstdc++,gcc-libs\n"
    "rpm,systemd-libs,systemd-libs\n"
    "rpm,dbus-libs,dbus\n"
    "rpm,glib2,glib2\n"
    "rpm,pcre2,pcre2\n"
    "rpm,expat,expat\n"
    "rpm,libffi,libffi\n"
    "rpm,libpng,libpng\n"
    "rpm,libjpeg-turbo,libjpeg-turbo\n"
    "rpm,freetype,freetype2\n"
    "rpm,fontconfig,fontconfig\n"
    "rpm,libX11,libx11\n"
    "rpm,libxcb,libxcb\n"
    "rpm,python3,python\n"
    "rpm,perl,perl\n"
    "rpm,bash,bash\n"
    "rpm,coreutils,coreutils\n"
    "rpm,ncurses-libs,ncurses\n"
    "rpm,readline,readline\n"
    "rpm,sqlite-libs,sqlite\n"
    "rpm,libarchive,libarchive\n"
    "rpm,gpgme,gpgme\n";

// Parse one CSV line: "fmt,name,arch" and insert into the map.
void parse_csv_line(DepMap& map, const std::string& line) {
    // Skip comments and empty lines.
    if (line.empty() || line[0] == '#') return;
    // Find the three comma-separated fields.
    auto c1 = line.find(',');
    if (c1 == std::string::npos) return;
    auto c2 = line.find(',', c1 + 1);
    if (c2 == std::string::npos) return;
    std::string fmt  = line.substr(0, c1);
    std::string name = line.substr(c1 + 1, c2 - c1 - 1);
    std::string arch = line.substr(c2 + 1);
    // Trim trailing whitespace/newline.
    while (!arch.empty() && (arch.back() == '\n' || arch.back() == '\r' ||
                             arch.back() == ' ' || arch.back() == '\t'))
        arch.pop_back();
    while (!fmt.empty()  && (fmt.back()  == ' ' || fmt.back()  == '\t'))  fmt.pop_back();
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    if (fmt.empty() || name.empty() || arch.empty()) return;
    map.set_override(fmt, name, arch);
}

} // namespace

bool DepMap::load_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line))
        parse_csv_line(*this, line);
    return true;
}

void DepMap::load_builtin() {
    std::istringstream ss(BUILTIN_MAP);
    std::string line;
    while (std::getline(ss, line))
        parse_csv_line(*this, line);
}

void DepMap::set_override(const std::string& src_fmt, const std::string& src_name,
                          const std::string& arch_name) {
    m_map[make_key(src_fmt, src_name)] = arch_name;
}

std::optional<std::string> DepMap::lookup(const std::string& src_fmt,
                                          const std::string& src_name) const {
    auto it = m_map.find(make_key(src_fmt, src_name));
    if (it == m_map.end()) return std::nullopt;
    return it->second;
}

} // namespace ranarch
