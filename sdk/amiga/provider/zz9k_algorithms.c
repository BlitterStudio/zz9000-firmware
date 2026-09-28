/*
 * ZZ9000 OpenSSL provider — combined KEYMGMT and KEYEXCH algorithm tables.
 *
 * KEYMGMT spans several key types (X25519, EC/P-256, and later RSA), and
 * KEYEXCH now spans two implementations (X25519, ECDH), each implemented in
 * its own translation unit, so the tables that the provider's query-operation
 * callback hands back for OSSL_OP_KEYMGMT / OSSL_OP_KEYEXCH are assembled
 * here. The cipher and signature tables live with their single
 * implementations.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k_prov_local.h"

/* X25519 is owned end-to-end (keygen, key exchange, param round-trips), so it
 * is unconditionally advertised. EC and RSA are now ALSO owned end-to-end in
 * the sense that matters for TLS: the provider's EC KEYMGMT accelerates
 * P-256 ECDHE keygen/derive and P-256 ECDSA cert-verify directly, and the RSA
 * KEYMGMT accelerates RSA-2048 PKCS#1 v1.5/SHA-256 cert-verify directly; both
 * DELEGATE every other curve/size/padding to a shadow EVP_PKEY built against
 * the default provider at import time (see zz9k_ecdsa.c / zz9k_rsa.c). That
 * delegation is what makes it safe to shadow the whole "EC"/"RSA" keytypes via
 * the "?provider=zz9000" default property query, even though a single keytype
 * spans every curve/size/padding: non-accelerated certificate keys (e.g.
 * P-384/P-521 certs, RSA-PSS — the TLS1.3 RSA signature scheme) still verify
 * correctly, just not on the hardware. Production additionally gates X25519,
 * EC and RSA independently on their own firmware capability flags
 * (zz9k_provider.c), hence the single/paired-algorithm variants below
 * alongside the fully combined table. */
const OSSL_ALGORITHM zz9k_keymgmt_algorithms[] = {
  { "X25519", "provider=zz9000", zz9k_x25519_keymgmt_functions,
    "ZZ9000 X25519 key management" },
  { "EC", "provider=zz9000", zz9k_ec_keymgmt_functions,
    "ZZ9000 EC (P-256 accelerated, other curves delegated) key management" },
  { "RSA", "provider=zz9000", zz9k_rsa_keymgmt_functions,
    "ZZ9000 RSA (2048 PKCS#1 accelerated, else delegated) key management" },
  { NULL, NULL, NULL, NULL }
};

const OSSL_ALGORITHM zz9k_keymgmt_algorithms_x25519_only[] = {
  { "X25519", "provider=zz9000", zz9k_x25519_keymgmt_functions,
    "ZZ9000 X25519 key management" },
  { NULL, NULL, NULL, NULL }
};

const OSSL_ALGORITHM zz9k_keymgmt_algorithms_ec_only[] = {
  { "EC", "provider=zz9000", zz9k_ec_keymgmt_functions,
    "ZZ9000 EC (P-256 accelerated, other curves delegated) key management" },
  { NULL, NULL, NULL, NULL }
};

const OSSL_ALGORITHM zz9k_keymgmt_algorithms_rsa_only[] = {
  { "RSA", "provider=zz9000", zz9k_rsa_keymgmt_functions,
    "ZZ9000 RSA (2048 PKCS#1 accelerated, else delegated) key management" },
  { NULL, NULL, NULL, NULL }
};

const OSSL_ALGORITHM zz9k_keymgmt_algorithms_x25519_ec[] = {
  { "X25519", "provider=zz9000", zz9k_x25519_keymgmt_functions,
    "ZZ9000 X25519 key management" },
  { "EC", "provider=zz9000", zz9k_ec_keymgmt_functions,
    "ZZ9000 EC (P-256 accelerated, other curves delegated) key management" },
  { NULL, NULL, NULL, NULL }
};

const OSSL_ALGORITHM zz9k_keymgmt_algorithms_x25519_rsa[] = {
  { "X25519", "provider=zz9000", zz9k_x25519_keymgmt_functions,
    "ZZ9000 X25519 key management" },
  { "RSA", "provider=zz9000", zz9k_rsa_keymgmt_functions,
    "ZZ9000 RSA (2048 PKCS#1 accelerated, else delegated) key management" },
  { NULL, NULL, NULL, NULL }
};

const OSSL_ALGORITHM zz9k_keymgmt_algorithms_ec_rsa[] = {
  { "EC", "provider=zz9000", zz9k_ec_keymgmt_functions,
    "ZZ9000 EC (P-256 accelerated, other curves delegated) key management" },
  { "RSA", "provider=zz9000", zz9k_rsa_keymgmt_functions,
    "ZZ9000 RSA (2048 PKCS#1 accelerated, else delegated) key management" },
  { NULL, NULL, NULL, NULL }
};

/* The KEYEXCH table is returned as a whole whenever X25519-or-EC applies
 * (zz9k_provider.c): each algorithm is fetched by name, and a peer/own key
 * can only reach ECDH if it was created by our EC KEYMGMT in the first
 * place, which is independently gated on its own capability flags. So an
 * inert "ECDH" entry when only X25519 is offloaded (or vice versa) is
 * unreachable, not unsafe. */
const OSSL_ALGORITHM zz9k_keyexch_algorithms[] = {
  { "X25519", "provider=zz9000", zz9k_x25519_keyexch_functions,
    "ZZ9000 X25519 key exchange" },
  { "ECDH", "provider=zz9000", zz9k_ecdh_keyexch_functions,
    "ZZ9000 P-256 ECDH key exchange" },
  { NULL, NULL, NULL, NULL }
};

/* ECDSA gates on CRYPTO_ECDSA_P256, RSA on CRYPTO_RSA_2048 (zz9k_provider.c);
 * since they share this one table, production returns it whenever EITHER
 * capability applies. Each algorithm is fetched by name via a key created by
 * its own (independently gated) KEYMGMT, so an entry left unreachable
 * because its own capability is absent is inert, not unsafe — same reasoning
 * as zz9k_keyexch_algorithms above. */
const OSSL_ALGORITHM zz9k_signature_algorithms[] = {
  { "ECDSA", "provider=zz9000", zz9k_ecdsa_signature_functions,
    "ZZ9000 ECDSA verify (P-256 accelerated, other curves delegated)" },
  { "RSA", "provider=zz9000", zz9k_rsa_signature_functions,
    "ZZ9000 RSA verify (2048 PKCS#1 accelerated, else delegated)" },
  { NULL, NULL, NULL, NULL }
};
