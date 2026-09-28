/*
 * ZZ9000 OpenSSL provider — AmiSSL registration helpers.
 *
 * Call zz9k_amissl_register() once, after a successful InitAmiSSL(), to make
 * the ZZ9000 provider the preferred backend for the default library context.
 * From then on ordinary AmiSSL/OpenSSL calls (EVP_PKEY_derive, EVP_EncryptUpdate
 * for AES-GCM / ChaCha20-Poly1305, EVP_DigestVerify for ECDSA-P256 and
 * RSA-PKCS1) are routed to the ZZ9000 when it advertises the matching service,
 * and fall back to AmiSSL's software implementation otherwise.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_AMISSL_H
#define ZZ9K_AMISSL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register the built-in ZZ9000 provider with the default library context, load
 * it alongside the default provider (kept for software fallback), and set the
 * default property query to "?provider=zz9000". Returns 1 on success, 0 on
 * failure (in which case nothing is left registered). */
int zz9k_amissl_register(void);

/* Undo zz9k_amissl_register(): clear the default property query and unload the
 * providers. Safe to call even if registration failed or never ran. Call before
 * CleanupAmiSSL(). */
void zz9k_amissl_unregister(void);

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_AMISSL_H */
