// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — GPG signature verification implementation.
#include "sig_verifier.h"

#include "ranarch/error.h"
#include "logger.h"

#include <gpgme.h>

#include <string>
#include <vector>

namespace ranarch {

namespace {

// Create a gpgme context configured with the keyring's home directory.
gpgme_ctx_t create_verify_ctx(const Keyring& keyring) {
    static bool gpgme_init = false;
    if (!gpgme_init) {
        gpgme_check_version(nullptr);
        gpgme_set_locale(nullptr, LC_CTYPE, setlocale(LC_CTYPE, nullptr));
        gpgme_init = true;
    }
    gpgme_ctx_t ctx = nullptr;
    gpgme_error_t err = gpgme_new(&ctx);
    if (gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
        throw Exception(RanArchError::Internal,
                        std::string("gpgme_new: ") + gpgme_strerror(err));
    }
    gpgme_set_protocol(ctx, GPGME_PROTOCOL_OpenPGP);
    gpgme_ctx_set_engine_info(ctx, GPGME_PROTOCOL_OpenPGP, nullptr,
                              keyring.gnupg_home().c_str());
    return ctx;
}

// Core detached-signature verification. Returns true if the signature is valid.
// `sig`  = the detached signature bytes (binary or ASCII-armored).
// `data` = the signed payload.
SignatureStatus verify_detached(const std::vector<uint8_t>& sig,
                                 const std::vector<uint8_t>& data,
                                 const Keyring& keyring) {
    if (sig.empty() || data.empty())
        return SignatureStatus::Bad;

    gpgme_ctx_t ctx = create_verify_ctx(keyring);

    gpgme_data_t sig_dh = nullptr;
    gpgme_data_t txt_dh = nullptr;
    gpgme_error_t err;

    err = gpgme_data_new_from_mem(&sig_dh,
        reinterpret_cast<const char*>(sig.data()), sig.size(), 0);
    if (gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
        gpgme_release(ctx);
        return SignatureStatus::Bad;
    }
    err = gpgme_data_new_from_mem(&txt_dh,
        reinterpret_cast<const char*>(data.data()), data.size(), 0);
    if (gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
        gpgme_data_release(sig_dh);
        gpgme_release(ctx);
        return SignatureStatus::Bad;
    }

    // Detached verify: signature + signed text, no plaintext output.
    err = gpgme_op_verify(ctx, sig_dh, txt_dh, nullptr);
    gpgme_data_release(sig_dh);
    gpgme_data_release(txt_dh);

    SignatureStatus result = SignatureStatus::Bad;
    if (gpgme_err_code(err) == GPG_ERR_NO_ERROR) {
        gpgme_verify_result_t vr = gpgme_op_verify_result(ctx);
        if (vr && vr->signatures) {
            // A signature is valid if its status is NO_ERROR.
            gpgme_signature_t s = vr->signatures;
            if (gpgme_err_code(s->status) == GPG_ERR_NO_ERROR) {
                result = SignatureStatus::SignedOk;
            } else if (gpgme_err_code(s->status) == GPG_ERR_NO_PUBKEY) {
                result = SignatureStatus::Bad; // key not in keyring
                RA_LOG_WARN("signature key %s not in trusted keyring",
                            s->fpr ? s->fpr : "?");
            } else {
                RA_LOG_WARN("signature verification failed: %s",
                            gpgme_strerror(s->status));
            }
        }
    } else {
        RA_LOG_WARN("gpgme_op_verify error: %s", gpgme_strerror(err));
    }

    gpgme_release(ctx);
    return result;
}

} // namespace

SignatureStatus SigVerifier::verify_deb(const PackageMeta& meta,
                                        const Keyring& keyring) {
    if (!meta.embedded_signature.has_value())
        return SignatureStatus::Unsigned;
    return verify_detached(meta.embedded_signature.value(),
                           meta.signed_payload, keyring);
}

SignatureStatus SigVerifier::verify_rpm(const PackageMeta& meta,
                                         const Keyring& keyring) {
    if (!meta.embedded_signature.has_value())
        return SignatureStatus::Unsigned;
    return verify_detached(meta.embedded_signature.value(),
                           meta.signed_payload, keyring);
}

SignatureStatus SigVerifier::enforce_policy(SignatureStatus status,
                                             SignaturePolicy policy) {
    switch (policy) {
        case SignaturePolicy::Ignore:
            return SignatureStatus::Skipped;
        case SignaturePolicy::Warn:
            if (status == SignatureStatus::Unsigned)
                RA_LOG_WARN("package is unsigned — proceeding (policy=warn)");
            else if (status == SignatureStatus::Bad)
                RA_LOG_WARN("signature verification failed — proceeding (policy=warn)");
            return status;
        case SignaturePolicy::Strict:
            // Caller checks the returned status and decides to abort.
            return status;
    }
    return status;
}

} // namespace ranarch
