// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — trusted GPG keyring management.
//
// Manages a temporary GnuPG home directory containing the public keys
// trusted for signature verification. Keys are loaded from:
//   - System keyring: /usr/share/ranarch/keyring/ (read-only, root-managed)
//   - Runtime keyring: /var/lib/ranarch/keyring/ (root-writable, for imports)
// Any .gpg, .asc, or .key file in those directories is imported at init().
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ranarch {

class Keyring {
public:
    Keyring();
    ~Keyring();

    Keyring(const Keyring&)            = delete;
    Keyring& operator=(const Keyring&) = delete;

    // Create the temporary GnuPG home directory and, if the directories
    // exist, import all .gpg/.asc/.key files from them.
    void init(const std::string& system_dir  = "",
              const std::string& runtime_dir = "");

    // Import a public key (ASCII-armored or binary) from memory.
    // Returns the fingerprint of the first imported key, or "" on failure.
    std::string import_key(const std::vector<uint8_t>& key_data);

    // Import a public key from a file. Returns the fingerprint or "".
    std::string import_key_file(const std::string& path);

    // Check whether a key with the given fingerprint is in the keyring.
    bool has_key(const std::string& fingerprint) const;

    // The GnuPG home directory path (for sig_verifier context setup).
    const std::string& gnupg_home() const { return m_gnupg_home; }

private:
    std::string m_gnupg_home;
};

} // namespace ranarch
