// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal self-contained test runner for the sig_verifier + keyring modules.
// Uses the `gpg` command-line tool to generate a throwaway key, sign test
// data, and export the public key. Verification is done through the
// SigVerifier class (which uses gpgme internally).
#include "core/config.h"
#include "core/keyring.h"
#include "core/sig_verifier.h"
#include "ranarch/types.h"

#include <cstdlib>
#include <fstream>
#include <ftw.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;
void check(bool cond, const std::string& what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; ++failures; }
    else         std::cout << "ok: " << what << "\n";
}

// Recursively remove a directory tree.
int rm_cb(const char* path, const struct stat*, int, struct FTW*) {
    return ::remove(path);
}
void remove_tree(const std::string& dir) {
    if (!dir.empty()) nftw(dir.c_str(), rm_cb, 64, FTW_DEPTH | FTW_PHYS);
}

std::string make_temp_home() {
    char tmpl[] = "/tmp/ranarch-sigtest-XXXXXX";
    char* d = mkdtemp(tmpl);
    if (!d) { std::cerr << "mkdtemp failed\n"; std::exit(1); }
    // gpg needs 0700 permissions on its home directory
    ::chmod(d, 0700);
    return d;
}

// Run a shell command, returning the exit code.
int run(const std::string& cmd) {
    std::string full = cmd + " 2>/dev/null";
    return std::system(full.c_str());
}

// Write data to a file.
void write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
}
void write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(data.data(), data.size());
}

// Read a file into a byte vector.
std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

// Check if a command (gpg) is available.
bool have_cmd(const std::string& cmd) {
    std::string which = "which " + cmd + " > /dev/null 2>&1";
    return std::system(which.c_str()) == 0;
}

} // namespace

int main() {
    using namespace ranarch;

    // ---- Prerequisite: gpg must be installed ---------------------------
    if (!have_cmd("gpg")) {
        std::cerr << "SKIPPING sig_verifier tests — gpg not found\n";
        return 0;  // not a failure, just can't test
    }

    // ---- 1. Create a signing home + generate a key -------------------
    std::string signing_home = make_temp_home();
    std::string params_file  = signing_home + "/keygen-params";
    write_file(params_file,
        "Key-Type: RSA\n"
        "Key-Length: 2048\n"
        "Name-Real: RanArch Test\n"
        "Name-Email: test@ranarch.local\n"
        "Expire-Date: 0\n"
        "%no-protection\n"
        "%commit\n"
    );
    int rc = run("gpg --batch --homedir " + signing_home +
                 " --gen-key " + params_file);
    ::unlink(params_file.c_str());
    if (rc != 0) {
        std::cerr << "SKIPPING sig_verifier tests — gpg key generation failed (rc="
                  << rc << ")\n";
        remove_tree(signing_home);
        return 0;  // can't test without a key; not a test failure
    }
    check(true, "generated throwaway GPG key");

    // ---- 2. Export the public key ------------------------------------
    std::string pub_file = signing_home + "/pubkey.asc";
    run("gpg --batch --homedir " + signing_home +
        " --export --armor test@ranarch.local > " + pub_file);
    auto pubkey = read_file(pub_file);
    ::unlink(pub_file.c_str());
    check(!pubkey.empty(), "exported public key");

    // ---- 3. Sign some test data --------------------------------------
    std::vector<uint8_t> payload =
        {'H','e','l','l','o',' ','R','a','n','A','r','c','h','!','\n'};
    std::string data_file = signing_home + "/payload.bin";
    std::string sig_file  = signing_home + "/payload.sig";
    write_file(data_file, payload);
    rc = run("gpg --batch --yes --homedir " + signing_home +
             " --detach-sign --output " + sig_file + " " + data_file);
    auto signature = read_file(sig_file);
    ::unlink(data_file.c_str());
    ::unlink(sig_file.c_str());
    check(rc == 0,           "signed test data");
    check(!signature.empty(), "signature non-empty");

    // ---- 4. Create a Keyring + import the public key ------------------
    Keyring keyring;
    keyring.init();
    std::string fp = keyring.import_key(pubkey);
    check(!fp.empty(),             "imported public key into keyring");
    check(keyring.has_key(fp),     "keyring has the imported key");

    // ---- 5. Verify a valid signature → SignedOk ---------------------
    {
        PackageMeta meta;
        meta.embedded_signature = signature;
        meta.signed_payload     = payload;
        SigVerifier sv;
        SignatureStatus result = sv.verify_deb(meta, keyring);
        check(result == SignatureStatus::SignedOk, "valid signature → SignedOk");
    }

    // ---- 6. Verify with modified data → Bad ------------------------
    {
        PackageMeta meta;
        meta.embedded_signature = signature;
        std::vector<uint8_t> modified = payload;
        modified[0] = 'X';  // tamper
        meta.signed_payload = modified;
        SigVerifier sv;
        SignatureStatus result = sv.verify_deb(meta, keyring);
        check(result == SignatureStatus::Bad, "tampered data → Bad");
    }

    // ---- 7. No signature → Unsigned ---------------------------------
    {
        PackageMeta meta;
        meta.embedded_signature.reset();
        meta.signed_payload = payload;
        SigVerifier sv;
        SignatureStatus result = sv.verify_rpm(meta, keyring);
        check(result == SignatureStatus::Unsigned, "no signature → Unsigned");
    }

    // ---- 8. Policy enforcement --------------------------------------
    {
        using SP = SignaturePolicy;
        using SS = SignatureStatus;
        check(SigVerifier::enforce_policy(SS::SignedOk, SP::Strict) == SS::SignedOk,
              "policy strict: SignedOk passes");
        check(SigVerifier::enforce_policy(SS::Unsigned, SP::Strict) == SS::Unsigned,
              "policy strict: Unsigned stays Unsigned");
        check(SigVerifier::enforce_policy(SS::Bad, SP::Strict) == SS::Bad,
              "policy strict: Bad stays Bad");
        check(SigVerifier::enforce_policy(SS::SignedOk, SP::Warn) == SS::SignedOk,
              "policy warn: SignedOk passes");
        check(SigVerifier::enforce_policy(SS::Unsigned, SP::Warn) == SS::Unsigned,
              "policy warn: Unsigned passes with warning");
        check(SigVerifier::enforce_policy(SS::SignedOk, SP::Ignore) == SS::Skipped,
              "policy ignore: always Skipped");
        check(SigVerifier::enforce_policy(SS::Bad, SP::Ignore) == SS::Skipped,
              "policy ignore: Bad → Skipped");
    }

    // ---- 9. Verify rpm with same signature material ------------------
    {
        PackageMeta meta;
        meta.embedded_signature = signature;
        meta.signed_payload     = payload;
        SigVerifier sv;
        SignatureStatus result = sv.verify_rpm(meta, keyring);
        check(result == SignatureStatus::SignedOk, "rpm verify with valid sig → SignedOk");
    }

    // ---- cleanup ----------------------------------------------------
    remove_tree(signing_home);

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all sig_verifier tests passed\n";
    return 0;
}
