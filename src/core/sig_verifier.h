// SPDX-License-Identifier: MIT
// RanArch RD Installer — GPG signature verification.
//
// Uses gpgme to verify detached signatures on .deb and .rpm packages.
//   .deb : the _gpgorigin ar member is a detached signature over the
//          concatenation of debian-binary + control.tar.* + data.tar.*
//          (stored in PackageMeta::embedded_signature / signed_payload).
//   .rpm : the RSAHEADER/DSAHEADER tag in the Signature Header is a binary
//          PGP detached signature over the Header's immutable region
//          (stored in PackageMeta::embedded_signature / signed_payload).
//
// The signature policy (strict / warn / ignore) is applied to the result.
#pragma once

#include "keyring.h"
#include "config.h"            // SignaturePolicy
#include "ranarch/types.h"

namespace ranarch {

class SigVerifier {
public:
    // Verify a .deb's _gpgorigin detached signature.
    SignatureStatus verify_deb(const PackageMeta& meta, const Keyring& keyring);

    // Verify a .rpm's RSAHEADER/DSAHEADER detached signature.
    SignatureStatus verify_rpm(const PackageMeta& meta, const Keyring& keyring);

    // Apply the configured policy to a raw verification result.
    //   Strict  : unsigned/bad → returns the bad/unsigned status (caller rejects)
    //   Warn    : logs and returns the original status (caller may proceed)
    //   Ignore  : always returns SignatureStatus::Skipped
    static SignatureStatus enforce_policy(SignatureStatus status,
                                          SignaturePolicy  policy);
};

} // namespace ranarch
