/*
 * Handshake offload economic model.
 *
 * A parameterized break-even calculator for asymmetric TLS handshake
 * primitives (X25519, P-256 ECDHE/ECDSA, RSA verify/sign). Unlike the
 * symmetric record cipher, these are tiny-I/O / large-compute operations,
 * so the offload cost is dominated by ARM compute, not the mailbox round
 * trip. The model takes measured or estimated microsecond costs and reports
 * whether offload wins and by how much.
 *
 * Costs are supplied by the caller; the docs/zz9k-crypto-acceleration.md
 * analysis carries the current first-order estimates, and the planned
 * asymmetric micro-benchmark will replace them with hardware measurements.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_HANDSHAKE_MODEL_H
#define ZZ9K_HANDSHAKE_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZZ9KHandshakeOp {
  uint32_t m68k_micros;    /* software cost of one op on the target 68k */
  uint32_t arm_micros;     /* ARM-side compute cost of one op */
  uint32_t mailbox_micros; /* fixed offload submit/complete round trip */
} ZZ9KHandshakeOp;

/* Total wall-clock cost of performing the op via offload. */
static uint32_t zz9k_handshake_offload_micros(const ZZ9KHandshakeOp *op)
{
  if (!op) {
    return 0U;
  }
  return op->arm_micros + op->mailbox_micros;
}

/*
 * Offload speedup over software as fixed-point hundredths (e.g. 1250 means
 * 12.50x). Returns 0 when offload is not strictly faster, which is the
 * handshake analog of the symmetric small-record case: if an op is so cheap
 * that the mailbox round trip dominates, offload does not pay.
 */
static uint32_t zz9k_handshake_speedup_x100(const ZZ9KHandshakeOp *op)
{
  uint32_t offload;

  if (!op) {
    return 0U;
  }
  offload = zz9k_handshake_offload_micros(op);
  if (offload == 0U || op->m68k_micros <= offload) {
    return 0U;
  }
  return (uint32_t)(((uint64_t)op->m68k_micros * 100ULL) / offload);
}

/*
 * Wall-clock microseconds saved per op by offloading. Returns 0 when
 * offload is not faster.
 */
static uint32_t zz9k_handshake_saved_micros(const ZZ9KHandshakeOp *op)
{
  uint32_t offload;

  if (!op) {
    return 0U;
  }
  offload = zz9k_handshake_offload_micros(op);
  if (offload == 0U || op->m68k_micros <= offload) {
    return 0U;
  }
  return op->m68k_micros - offload;
}

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_HANDSHAKE_MODEL_H */
