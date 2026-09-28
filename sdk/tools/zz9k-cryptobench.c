/*
 * ChaCha20-Poly1305 offload break-even benchmark and X25519 timing section.
 *
 * Sweeps TLS-sized records and compares the portable software reference
 * (the m68k baseline) against the ZZ9000 offload AEAD, reporting per-size
 * throughput and the break-even record size at which offload overtakes
 * software. This answers the central Phase 0 question: is symmetric offload
 * worth the mailbox round-trip at realistic record sizes?
 *
 * The X25519 section times software vs. offload for a single scalar multiply,
 * reporting milliseconds per operation and the speedup ratio.
 *
 * The software sweeps run on any target. The offload paths need the crypto
 * service and are skipped when the capability is absent (e.g. on the host).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/caps.h"
#include "zz9k/crypto.h"
#include "zz9k/host.h"
#include "zz9k/shared.h"
#include "zz9k-crypto-soft.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_CRYPTOBENCH_AMIGA 1
#include <devices/timer.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/timer.h>
#else
#include <time.h>
#endif

#define ZZ9K_CRYPTOBENCH_SWEEP_COUNT 9U
#define ZZ9K_CRYPTOBENCH_MAX_RECORD_BYTES 16384U
#define ZZ9K_CRYPTOBENCH_DEFAULT_ITERATIONS 16U
#define ZZ9K_CRYPTOBENCH_MAX_ITERATIONS 4096U
#define ZZ9K_CRYPTOBENCH_TAG_BYTES 16U
#define ZZ9K_CRYPTOBENCH_KEY_BYTES 32U
#define ZZ9K_CRYPTOBENCH_NONCE_BYTES 12U

typedef uint64_t ZZ9KCryptoBenchTick;

/* Record size for sweep index 0..ZZ9K_CRYPTOBENCH_SWEEP_COUNT-1. */
static uint32_t zz9k_cryptobench_sweep_size(uint32_t index)
{
  if (index >= ZZ9K_CRYPTOBENCH_SWEEP_COUNT) {
    return 0U;
  }
  return 64U << index; /* 64, 128, 256 ... 16384 */
}

static uint32_t zz9k_cryptobench_parse_iterations(int argc, char **argv)
{
  unsigned long value;

  if (argc < 2) {
    return ZZ9K_CRYPTOBENCH_DEFAULT_ITERATIONS;
  }
  value = strtoul(argv[1], 0, 0);
  if (value < 1UL) {
    return 1U;
  }
  if (value > ZZ9K_CRYPTOBENCH_MAX_ITERATIONS) {
    return ZZ9K_CRYPTOBENCH_MAX_ITERATIONS;
  }
  return (uint32_t)value;
}

static uint32_t zz9k_cryptobench_kib_per_second(uint32_t bytes,
                                                ZZ9KCryptoBenchTick ticks,
                                                uint32_t ticks_per_second)
{
  uint64_t rate;

  if (ticks == 0 || ticks_per_second == 0) {
    return 0;
  }
  rate = ((uint64_t)bytes * ticks_per_second) / ((uint64_t)ticks * 1024ULL);
  if (rate > UINT32_MAX) {
    return UINT32_MAX;
  }
  return (uint32_t)rate;
}

/*
 * Smallest record size at which offload throughput meets or beats software,
 * or 0 if offload never wins across the sampled sizes. Arrays are parallel
 * and `count` entries long, ordered by ascending size.
 */
static uint32_t zz9k_cryptobench_breakeven_size(const uint32_t *sizes,
                                                const uint32_t *software_kib,
                                                const uint32_t *offload_kib,
                                                uint32_t count)
{
  uint32_t i;

  if (!sizes || !software_kib || !offload_kib) {
    return 0U;
  }
  for (i = 0; i < count; i++) {
    if (offload_kib[i] != 0U && offload_kib[i] >= software_kib[i]) {
      return sizes[i];
    }
  }
  return 0U;
}

#if ZZ9K_CRYPTOBENCH_AMIGA
struct Device *TimerBase;
#endif

typedef struct ZZ9KCryptoBenchTimer {
  uint32_t ticks_per_second;
  int high_resolution;
#if ZZ9K_CRYPTOBENCH_AMIGA
  struct MsgPort *timer_port;
  struct timerequest *timer_request;
#endif
} ZZ9KCryptoBenchTimer;

static void zz9k_cryptobench_timer_close(ZZ9KCryptoBenchTimer *timer)
{
#if ZZ9K_CRYPTOBENCH_AMIGA
  if (!timer) {
    return;
  }
  if (timer->timer_request) {
    if (timer->high_resolution) {
      CloseDevice((struct IORequest *)timer->timer_request);
    }
    DeleteIORequest(timer->timer_request);
  }
  if (timer->timer_port) {
    DeleteMsgPort(timer->timer_port);
  }
  if (TimerBase && timer->high_resolution) {
    TimerBase = 0;
  }
#else
  (void)timer;
#endif
}

static void zz9k_cryptobench_timer_open(ZZ9KCryptoBenchTimer *timer)
{
  memset(timer, 0, sizeof(*timer));
#if ZZ9K_CRYPTOBENCH_AMIGA
  timer->timer_port = CreateMsgPort();
  if (timer->timer_port) {
    timer->timer_request = (struct timerequest *)CreateIORequest(
        timer->timer_port, sizeof(*timer->timer_request));
  }
  if (timer->timer_request &&
      OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                 (struct IORequest *)timer->timer_request, 0) == 0) {
    struct EClockVal value;

    TimerBase = (struct Device *)timer->timer_request->tr_node.io_Device;
    timer->ticks_per_second = ReadEClock(&value);
    if (timer->ticks_per_second != 0) {
      timer->high_resolution = 1;
      return;
    }
    CloseDevice((struct IORequest *)timer->timer_request);
  }
  zz9k_cryptobench_timer_close(timer);
  memset(timer, 0, sizeof(*timer));
  timer->ticks_per_second = 50U;
#else
  timer->ticks_per_second = (uint32_t)CLOCKS_PER_SEC;
  timer->high_resolution = 1;
#endif
}

static ZZ9KCryptoBenchTick zz9k_cryptobench_timer_now(
    const ZZ9KCryptoBenchTimer *timer)
{
#if ZZ9K_CRYPTOBENCH_AMIGA
  if (timer && timer->high_resolution) {
    struct EClockVal value;

    ReadEClock(&value);
    return ((ZZ9KCryptoBenchTick)value.ev_hi << 32) | value.ev_lo;
  }
  return 0;
#else
  (void)timer;
  return (ZZ9KCryptoBenchTick)clock();
#endif
}

/*
 * Returns elapsed time in milliseconds * 100 (fixed-point hundredths) for
 * `count` X25519 operations, or 0 on timer error. Used for both software and
 * offload paths so the caller can compute a consistent speedup ratio.
 */
static uint32_t zz9k_cryptobench_ms_x100_per_op(ZZ9KCryptoBenchTick elapsed,
                                                 uint32_t count,
                                                 uint32_t ticks_per_second)
{
  uint64_t numerator;

  if (elapsed == 0 || count == 0 || ticks_per_second == 0) {
    return 0U;
  }
  /* ms * 100 = (elapsed * 100000) / (count * ticks_per_second) */
  numerator = (uint64_t)elapsed * 100000ULL;
  return (uint32_t)(numerator / ((uint64_t)count * ticks_per_second));
}

#ifndef ZZ9K_CRYPTOBENCH_NO_MAIN

static void zz9k_cryptobench_fill_pattern(uint8_t *data, uint32_t length)
{
  uint32_t i;

  for (i = 0; i < length; i++) {
    data[i] = (uint8_t)((i * 37U + 11U) & 0xffU);
  }
}

/*
 * RFC 7748 Section 5.2 test vector 1, shared by the software and offload
 * timing paths so both always measure identical inputs and either result
 * can be verified against the known shared secret.
 */
static const uint8_t
zz9k_cryptobench_x25519_scalar[ZZ9K_CRYPTO_X25519_KEY_BYTES] = {
  0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d,
  0x3b, 0x16, 0x15, 0x4b, 0x82, 0x46, 0x5e, 0xdd,
  0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc, 0x5a, 0x18,
  0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4
};
static const uint8_t
zz9k_cryptobench_x25519_point[ZZ9K_CRYPTO_X25519_KEY_BYTES] = {
  0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb,
  0x35, 0x94, 0xc1, 0xa4, 0x24, 0xb1, 0x5f, 0x7c,
  0x72, 0x66, 0x24, 0xec, 0x26, 0xb3, 0x35, 0x3b,
  0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c
};
static const uint8_t
zz9k_cryptobench_x25519_shared[ZZ9K_CRYPTO_X25519_SHARED_BYTES] = {
  0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90,
  0x8e, 0x94, 0xea, 0x4d, 0xf2, 0x8d, 0x08, 0x4f,
  0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c, 0x71, 0xf7,
  0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52
};

/* P-256 ECDH, ECDSA-P256 and RSA-2048 verify vectors (OpenSSL-generated).
 * The RSA key buffer is the 256-byte modulus followed by the 4-byte big-endian
 * exponent (65537), matching the firmware verify ABI. */
static const uint8_t zz9k_cryptobench_p256_scalar[32] = {
  0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88,
  0x09, 0xcf, 0x4f, 0x3c, 0x76, 0x2e, 0x71, 0x60, 0xf3, 0x8b, 0x4d, 0xa5,
  0x6a, 0x78, 0x4d, 0x90, 0x45, 0x19, 0x0c, 0xfe
};
static const uint8_t zz9k_cryptobench_p256_point[65] = {
  0x04, 0xb9, 0x63, 0x1d, 0x89, 0x02, 0xbe, 0x4d, 0x3d, 0x9a, 0xb3, 0xd8,
  0x6d, 0x04, 0x9e, 0x47, 0x4d, 0x6a, 0xec, 0xdc, 0x72, 0x26, 0xc5, 0xc4,
  0xe9, 0x03, 0x08, 0xbe, 0xec, 0x49, 0x32, 0x41, 0x0f, 0x9e, 0x80, 0x91,
  0x8b, 0xba, 0xa4, 0x79, 0x2c, 0x01, 0x51, 0x70, 0x26, 0xac, 0xba, 0xc0,
  0x7b, 0xee, 0x08, 0x33, 0x97, 0x3a, 0x0e, 0x32, 0xa5, 0xc7, 0x62, 0x88,
  0x7a, 0xba, 0xcb, 0x0b, 0xae
};
static const uint8_t zz9k_cryptobench_p256_shared[32] = {
  0x18, 0xb1, 0x8b, 0x3a, 0xe8, 0x5e, 0x3c, 0x3a, 0xae, 0xd2, 0x41, 0x55,
  0x6b, 0x2a, 0xcf, 0x25, 0x25, 0x0b, 0x2f, 0x1b, 0xbb, 0xf0, 0x47, 0x2c,
  0x30, 0x3a, 0x75, 0x74, 0x72, 0x59, 0xed, 0x7b
};
static const uint8_t zz9k_cryptobench_ecdsa_pub[65] = {
  0x04, 0xee, 0x73, 0x55, 0x2a, 0x69, 0x91, 0xe6, 0xd2, 0x91, 0xda, 0x32,
  0x92, 0xb3, 0xa9, 0xe7, 0xd5, 0x43, 0x72, 0xb9, 0x60, 0x78, 0x94, 0xa2,
  0xe8, 0x9c, 0xb3, 0x25, 0xd0, 0x8c, 0xd2, 0xe8, 0xcf, 0x49, 0x29, 0xdb,
  0x22, 0x0e, 0x29, 0x21, 0x17, 0xcb, 0xdc, 0xb8, 0x36, 0x9e, 0xa1, 0x26,
  0x0b, 0x40, 0xcd, 0x76, 0x85, 0x56, 0x20, 0xfb, 0x6e, 0x22, 0x9c, 0x2b,
  0xc9, 0x26, 0x9a, 0x82, 0x0e
};
static const uint8_t zz9k_cryptobench_ecdsa_hash[32] = {
  0x00, 0xfe, 0x30, 0x42, 0x50, 0x29, 0x55, 0x24, 0xec, 0xe6, 0xc1, 0x8b,
  0xad, 0x53, 0x0d, 0xab, 0x60, 0xa5, 0xcb, 0x97, 0xca, 0x05, 0x39, 0xa4,
  0x25, 0x7f, 0x81, 0x35, 0x65, 0x72, 0xc2, 0xdb
};
static const uint8_t zz9k_cryptobench_ecdsa_sig[64] = {
  0xac, 0x4a, 0xb3, 0xaf, 0xcb, 0x78, 0xc0, 0xdb, 0x25, 0x9e, 0x12, 0x99,
  0x75, 0x7c, 0x1c, 0x54, 0x4b, 0xf0, 0x3b, 0xcf, 0xbd, 0x4b, 0x87, 0xae,
  0x44, 0x41, 0xef, 0x57, 0x9f, 0xcc, 0x04, 0x7b, 0x72, 0xe4, 0x4a, 0xe9,
  0x51, 0xc0, 0x13, 0x9d, 0x58, 0xe4, 0xca, 0xb4, 0xfe, 0x65, 0xad, 0x69,
  0xd4, 0x48, 0x3f, 0x07, 0x8b, 0x7a, 0x57, 0xd8, 0x74, 0xac, 0x1b, 0x42,
  0x38, 0xd8, 0x8a, 0xc3
};
static const uint8_t zz9k_cryptobench_rsa_mod[256] = {
  0xa6, 0x18, 0xe4, 0x52, 0x7e, 0xa9, 0x3b, 0xb1, 0x0b, 0x6d, 0xc6, 0x09,
  0xb3, 0x8e, 0xac, 0x9e, 0xe8, 0x64, 0xea, 0x6f, 0x47, 0x33, 0xb3, 0x99,
  0x7c, 0x06, 0x1e, 0x62, 0xc0, 0xb8, 0x87, 0x38, 0x79, 0x63, 0x8f, 0x9a,
  0xa2, 0x18, 0xb4, 0x06, 0x91, 0x74, 0xcf, 0x95, 0x3f, 0x61, 0xf8, 0x85,
  0xd9, 0x5c, 0x38, 0xe2, 0x4c, 0xc3, 0x9e, 0x4a, 0x71, 0xba, 0x2e, 0xc3,
  0x46, 0x78, 0x9f, 0x77, 0xd9, 0x07, 0xd0, 0x88, 0x55, 0x4b, 0xf9, 0x1f,
  0xf4, 0x4a, 0x68, 0xaa, 0x90, 0x9a, 0x9c, 0x83, 0x94, 0x4f, 0xd9, 0x35,
  0x58, 0x3e, 0xb9, 0xa4, 0xe2, 0x98, 0x80, 0x95, 0x13, 0xfa, 0x9e, 0x94,
  0x3a, 0xe5, 0x7f, 0xfd, 0x95, 0xd4, 0x44, 0xfb, 0x0d, 0xa2, 0xb3, 0x9c,
  0x83, 0x36, 0x9c, 0xef, 0xa0, 0xd5, 0x4b, 0x26, 0x86, 0x35, 0x72, 0x60,
  0x32, 0x9b, 0x8d, 0x35, 0xc7, 0xfa, 0xd8, 0xff, 0xe9, 0x6e, 0x7b, 0xec,
  0xcb, 0xd6, 0xb5, 0x71, 0x6b, 0x18, 0x98, 0x9c, 0xb8, 0xaa, 0xcc, 0x14,
  0x92, 0x52, 0xde, 0xb5, 0xe7, 0x7b, 0x06, 0x21, 0x20, 0x0c, 0x17, 0xa8,
  0x0a, 0x53, 0x3a, 0xec, 0x58, 0xeb, 0x96, 0x23, 0xdd, 0x50, 0x79, 0x1c,
  0xe0, 0xca, 0x72, 0x43, 0x54, 0x84, 0x9c, 0xec, 0x75, 0xfb, 0x2d, 0xd7,
  0x7f, 0xc9, 0x56, 0xb1, 0xf2, 0x23, 0xfa, 0x3f, 0x56, 0xfa, 0xdd, 0xda,
  0x88, 0x73, 0x71, 0x7e, 0x98, 0xcb, 0x88, 0x7b, 0x78, 0x75, 0xf9, 0xab,
  0x1d, 0x5c, 0x70, 0xf2, 0xf4, 0x2d, 0x53, 0x88, 0x2c, 0x5e, 0xca, 0x73,
  0x85, 0x63, 0xfb, 0xa0, 0x69, 0x31, 0x30, 0x46, 0x04, 0x07, 0xd8, 0xf3,
  0x33, 0xa5, 0x34, 0x39, 0x3b, 0xf2, 0x9f, 0xa6, 0xc1, 0xa3, 0x85, 0x28,
  0x75, 0x09, 0x72, 0x6b, 0xc8, 0x3e, 0xd4, 0xca, 0xb4, 0x97, 0x75, 0x08,
  0xae, 0x40, 0x74, 0xeb
};
static const uint8_t zz9k_cryptobench_rsa_exp[4] = {
  0x00, 0x01, 0x00, 0x01
};
static const uint8_t zz9k_cryptobench_rsa_sig[256] = {
  0x38, 0x23, 0x64, 0xe9, 0x4a, 0xe3, 0xcd, 0xb5, 0xf8, 0xde, 0x84, 0xa1,
  0x14, 0xb1, 0x5f, 0x05, 0x19, 0x2a, 0x0d, 0x6f, 0x89, 0xfb, 0x08, 0x58,
  0x6b, 0x80, 0xc8, 0x34, 0xdf, 0xfb, 0x32, 0x22, 0xb4, 0xe7, 0x1d, 0xe5,
  0x09, 0x1c, 0xb6, 0x9a, 0xb5, 0x72, 0x0a, 0xbe, 0xf3, 0xbb, 0x24, 0xca,
  0xdf, 0xfe, 0x3c, 0x10, 0x44, 0xdc, 0x0e, 0x20, 0x72, 0x75, 0xe1, 0x59,
  0x7d, 0xea, 0x55, 0xcf, 0xf5, 0x15, 0x0a, 0x6b, 0xdb, 0x1b, 0xf4, 0xa8,
  0x93, 0x77, 0x4f, 0x7d, 0x5c, 0xd9, 0xe8, 0x7c, 0x12, 0x23, 0x21, 0xfb,
  0xa0, 0xda, 0x33, 0x8c, 0xdb, 0x3b, 0xf4, 0x70, 0x8d, 0xbd, 0x47, 0x8d,
  0x16, 0x79, 0x69, 0x64, 0x0d, 0x5f, 0x8e, 0xae, 0x61, 0x5d, 0x20, 0xa3,
  0xab, 0x86, 0x61, 0x07, 0xbe, 0x6b, 0x32, 0x54, 0xec, 0xb3, 0x2f, 0x44,
  0x3b, 0x33, 0x0f, 0x4c, 0x63, 0x01, 0x80, 0x4f, 0x1a, 0x2c, 0xca, 0xf2,
  0xd1, 0xee, 0x84, 0x59, 0xa7, 0xaf, 0x84, 0xb6, 0xb7, 0x26, 0xaa, 0x40,
  0xa3, 0x5e, 0x32, 0xc3, 0x4f, 0xe5, 0x83, 0xfa, 0x43, 0x68, 0xe0, 0x80,
  0xad, 0x89, 0xb3, 0xd2, 0x88, 0x2d, 0xf3, 0x28, 0x9b, 0x70, 0x0e, 0x60,
  0xbe, 0x67, 0xa4, 0x58, 0x6f, 0x50, 0x50, 0x8b, 0x93, 0x79, 0x4b, 0x8f,
  0xf3, 0xdc, 0xba, 0xba, 0xa7, 0x1d, 0x69, 0x57, 0x05, 0xcc, 0xe2, 0x18,
  0x7e, 0x9a, 0xda, 0x9b, 0xa1, 0x31, 0x2a, 0x15, 0xb7, 0x0b, 0x10, 0x17,
  0x55, 0x5d, 0x47, 0xd3, 0xd2, 0x96, 0x38, 0x34, 0x60, 0x2a, 0x35, 0xbf,
  0xc9, 0xcb, 0xd4, 0xc4, 0x22, 0xf6, 0xd2, 0x3d, 0xb5, 0x0e, 0x16, 0xbd,
  0xac, 0xde, 0x5c, 0x08, 0xe1, 0x3a, 0x10, 0x14, 0x76, 0x25, 0xa2, 0x4f,
  0x17, 0x01, 0x26, 0x3d, 0x00, 0xf2, 0x78, 0x35, 0xc9, 0xdb, 0x35, 0x41,
  0xe6, 0xf5, 0x18, 0xe5
};
static const uint8_t zz9k_cryptobench_rsa_hash[32] = {
  0xae, 0xa6, 0xcd, 0x65, 0xd5, 0x18, 0xcf, 0x68, 0x49, 0x28, 0x37, 0x86,
  0xf7, 0x8a, 0xd1, 0x0c, 0xe7, 0x1a, 0xfb, 0x89, 0x23, 0x00, 0x8d, 0x33,
  0x00, 0x59, 0x60, 0x7b, 0xec, 0x75, 0x05, 0x8e
};

/*
 * Times software X25519 over `count` operations using the RFC 7748 section 5.2
 * test vector.  Returns ms*100 per op (hundredths of a millisecond) so the
 * caller can print "X.XX ms/op" without floating-point, or 0 if the computed
 * shared secret does not match the known answer.
 */
static uint32_t zz9k_cryptobench_x25519_soft_ms_x100(
    const ZZ9KCryptoBenchTimer *timer, uint32_t count)
{
  uint8_t out[ZZ9K_CRYPTO_X25519_SHARED_BYTES];
  ZZ9KCryptoBenchTick start;
  ZZ9KCryptoBenchTick elapsed;
  uint32_t i;

  start = zz9k_cryptobench_timer_now(timer);
  for (i = 0; i < count; i++) {
    zz9k_soft_x25519(out, zz9k_cryptobench_x25519_scalar,
                     zz9k_cryptobench_x25519_point);
  }
  elapsed = zz9k_cryptobench_timer_now(timer) - start;
  if (memcmp(out, zz9k_cryptobench_x25519_shared, sizeof(out)) != 0) {
    printf("x25519 software: RFC 7748 verification FAILED\n");
    return 0U;
  }
  return zz9k_cryptobench_ms_x100_per_op(elapsed, count,
                                          timer->ticks_per_second);
}

/*
 * Times ZZ9000 offload X25519 over `count` operations and verifies the
 * computed shared secret against the RFC 7748 known answer.
 * Returns ms*100 per op, or 0 on failure or verification mismatch.
 */
static uint32_t zz9k_cryptobench_x25519_offload_ms_x100(
    ZZ9KContext *ctx, const ZZ9KCryptoBenchTimer *timer, uint32_t count)
{
  ZZ9KSharedBuffer scalar_buf;
  ZZ9KSharedBuffer point_buf;
  ZZ9KSharedBuffer out_buf;
  ZZ9KCryptoKxDesc desc;
  ZZ9KCryptoResult result;
  ZZ9KCryptoBenchTick start;
  ZZ9KCryptoBenchTick elapsed;
  uint32_t ms_x100 = 0U;
  uint32_t i;
  int status;

  memset(&scalar_buf, 0, sizeof(scalar_buf));
  memset(&point_buf,  0, sizeof(point_buf));
  memset(&out_buf,    0, sizeof(out_buf));

  status = zz9k_alloc_shared(ctx, ZZ9K_CRYPTO_X25519_KEY_BYTES, 16U, 0,
                             &scalar_buf);
  if (status != ZZ9K_STATUS_OK) {
    printf("x25519 offload scalar alloc: %s (%d)\n",
           zz9k_status_name(status), status);
    goto out;
  }
  status = zz9k_alloc_shared(ctx, ZZ9K_CRYPTO_X25519_KEY_BYTES, 16U, 0,
                             &point_buf);
  if (status != ZZ9K_STATUS_OK) {
    printf("x25519 offload point alloc: %s (%d)\n",
           zz9k_status_name(status), status);
    goto out;
  }
  status = zz9k_alloc_shared(ctx, ZZ9K_CRYPTO_X25519_SHARED_BYTES, 16U, 0,
                             &out_buf);
  if (status != ZZ9K_STATUS_OK) {
    printf("x25519 offload output alloc: %s (%d)\n",
           zz9k_status_name(status), status);
    goto out;
  }

  memcpy((void *)scalar_buf.data, zz9k_cryptobench_x25519_scalar,
         ZZ9K_CRYPTO_X25519_KEY_BYTES);
  memcpy((void *)point_buf.data, zz9k_cryptobench_x25519_point,
         ZZ9K_CRYPTO_X25519_KEY_BYTES);

  if (!zz9k_crypto_build_x25519_desc(&desc,
                                     scalar_buf.handle, 0U,
                                     point_buf.handle,  0U,
                                     out_buf.handle,    0U)) {
    printf("x25519 offload descriptor build failed\n");
    goto out;
  }

  start = zz9k_cryptobench_timer_now(timer);
  for (i = 0; i < count; i++) {
    memset(&result, 0, sizeof(result));
    status = zz9k_crypto_kx(ctx, &desc, &result);
    if (status != ZZ9K_STATUS_OK) {
      printf("x25519 offload: %s (%d)\n", zz9k_status_name(status), status);
      goto out;
    }
  }
  elapsed = zz9k_cryptobench_timer_now(timer) - start;

  if (memcmp((const void *)out_buf.data, zz9k_cryptobench_x25519_shared,
             ZZ9K_CRYPTO_X25519_SHARED_BYTES) != 0) {
    printf("x25519 offload: RFC 7748 verification FAILED "
           "(firmware computed a wrong shared secret)\n");
    goto out;
  }

  ms_x100 = zz9k_cryptobench_ms_x100_per_op(elapsed, count,
                                              timer->ticks_per_second);

out:
  if (out_buf.handle != 0 && out_buf.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, out_buf.handle);
  }
  if (point_buf.handle != 0 && point_buf.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, point_buf.handle);
  }
  if (scalar_buf.handle != 0 && scalar_buf.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, scalar_buf.handle);
  }
  return ms_x100;
}

/* Times software P-256 keygen (public = scalar*G) over `count` ops and checks
 * the public point is well-formed. Returns ms*100 per op, or 0 on failure.
 * Keygen is the offload op a TLS client needs for its ephemeral P-256 key
 * share; it is the one asymmetric primitive the other benches here do not
 * exercise (ECDH derives, it does not generate). */
static uint32_t zz9k_cryptobench_p256_keygen_soft_ms_x100(
    const ZZ9KCryptoBenchTimer *timer, uint32_t count)
{
  uint8_t pub[ZZ9K_CRYPTO_P256_POINT_BYTES];
  ZZ9KCryptoBenchTick start;
  ZZ9KCryptoBenchTick elapsed;
  uint32_t i;
  int ok = 1;

  start = zz9k_cryptobench_timer_now(timer);
  for (i = 0; i < count; i++) {
    if (!zz9k_soft_p256_keygen(pub, zz9k_cryptobench_p256_scalar)) {
      ok = 0;
      break;
    }
  }
  elapsed = zz9k_cryptobench_timer_now(timer) - start;
  if (!ok || pub[0] != 0x04) {
    printf("p256 keygen software: keygen FAILED\n");
    return 0U;
  }
  return zz9k_cryptobench_ms_x100_per_op(elapsed, count,
                                         timer->ticks_per_second);
}

/* Times ZZ9000 offload P-256 keygen and verifies the firmware-computed public
 * point against the software reference for the same scalar. Returns ms*100 per
 * op, or 0 on failure or point mismatch. */
static uint32_t zz9k_cryptobench_p256_keygen_offload_ms_x100(
    ZZ9KContext *ctx, const ZZ9KCryptoBenchTimer *timer, uint32_t count)
{
  ZZ9KSharedBuffer scalar_buf;
  ZZ9KSharedBuffer out_buf;
  ZZ9KCryptoKxDesc desc;
  ZZ9KCryptoResult result;
  uint8_t expected[ZZ9K_CRYPTO_P256_POINT_BYTES];
  ZZ9KCryptoBenchTick start;
  ZZ9KCryptoBenchTick elapsed;
  uint32_t ms_x100 = 0U;
  uint32_t i;
  int status;

  memset(&scalar_buf, 0, sizeof(scalar_buf));
  memset(&out_buf, 0, sizeof(out_buf));

  if (!zz9k_soft_p256_keygen(expected, zz9k_cryptobench_p256_scalar)) {
    printf("p256 keygen offload: software reference failed\n");
    return 0U;
  }

  status = zz9k_alloc_shared(ctx, ZZ9K_CRYPTO_P256_PRIVATE_BYTES, 16U, 0,
                             &scalar_buf);
  if (status != ZZ9K_STATUS_OK) {
    printf("p256 keygen offload scalar alloc: %s (%d)\n",
           zz9k_status_name(status), status);
    goto out;
  }
  status = zz9k_alloc_shared(ctx, ZZ9K_CRYPTO_P256_POINT_BYTES, 16U, 0,
                             &out_buf);
  if (status != ZZ9K_STATUS_OK) {
    printf("p256 keygen offload output alloc: %s (%d)\n",
           zz9k_status_name(status), status);
    goto out;
  }

  memcpy((void *)scalar_buf.data, zz9k_cryptobench_p256_scalar,
         ZZ9K_CRYPTO_P256_PRIVATE_BYTES);

  if (!zz9k_crypto_build_p256_keygen_desc(&desc, scalar_buf.handle, 0U,
                                          out_buf.handle, 0U)) {
    printf("p256 keygen offload descriptor build failed\n");
    goto out;
  }

  start = zz9k_cryptobench_timer_now(timer);
  for (i = 0; i < count; i++) {
    memset(&result, 0, sizeof(result));
    status = zz9k_crypto_kx(ctx, &desc, &result);
    if (status != ZZ9K_STATUS_OK) {
      printf("p256 keygen offload: %s (%d) on iteration %lu of %lu\n",
             zz9k_status_name(status), status, (unsigned long)(i + 1U),
             (unsigned long)count);
      goto out;
    }
    /* Verify every iteration, not just the last: an intermittent wrong point
     * (e.g. a coherency glitch on the 65-byte result buffer) is exactly the
     * fault we are hunting, and an end-of-loop check would miss it whenever a
     * later iteration happens to come back correct. */
    if (memcmp((const void *)out_buf.data, expected,
               ZZ9K_CRYPTO_P256_POINT_BYTES) != 0) {
      printf("p256 keygen offload: public-point MISMATCH on iteration %lu of "
             "%lu (firmware computed a wrong point)\n",
             (unsigned long)(i + 1U), (unsigned long)count);
      goto out;
    }
  }
  elapsed = zz9k_cryptobench_timer_now(timer) - start;

  ms_x100 = zz9k_cryptobench_ms_x100_per_op(elapsed, count,
                                            timer->ticks_per_second);

out:
  if (out_buf.handle != 0 && out_buf.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, out_buf.handle);
  }
  if (scalar_buf.handle != 0 && scalar_buf.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, scalar_buf.handle);
  }
  return ms_x100;
}

/* ---- Asymmetric handshake timing: P-256 ECDH, ECDSA-P256, RSA-2048 ---- */

/* Times software P-256 ECDH over `count` ops and verifies the shared X
 * coordinate against the known answer. Returns ms*100 per op, or 0. */
static uint32_t zz9k_cryptobench_p256_soft_ms_x100(
    const ZZ9KCryptoBenchTimer *timer, uint32_t count)
{
  uint8_t out[ZZ9K_CRYPTO_P256_SHARED_BYTES];
  ZZ9KCryptoBenchTick start;
  ZZ9KCryptoBenchTick elapsed;
  uint32_t i;

  start = zz9k_cryptobench_timer_now(timer);
  for (i = 0; i < count; i++) {
    zz9k_soft_p256_ecdh(out, zz9k_cryptobench_p256_scalar,
                        zz9k_cryptobench_p256_point);
  }
  elapsed = zz9k_cryptobench_timer_now(timer) - start;
  if (memcmp(out, zz9k_cryptobench_p256_shared, sizeof(out)) != 0) {
    printf("p256 software: shared-secret verification FAILED\n");
    return 0U;
  }
  return zz9k_cryptobench_ms_x100_per_op(elapsed, count,
                                         timer->ticks_per_second);
}

/* Times ZZ9000 offload P-256 ECDH and verifies the shared X coordinate. */
static uint32_t zz9k_cryptobench_p256_offload_ms_x100(
    ZZ9KContext *ctx, const ZZ9KCryptoBenchTimer *timer, uint32_t count)
{
  ZZ9KSharedBuffer scalar_buf;
  ZZ9KSharedBuffer point_buf;
  ZZ9KSharedBuffer out_buf;
  ZZ9KCryptoKxDesc desc;
  ZZ9KCryptoResult result;
  ZZ9KCryptoBenchTick start;
  ZZ9KCryptoBenchTick elapsed;
  uint32_t ms_x100 = 0U;
  uint32_t i;
  int status;

  memset(&scalar_buf, 0, sizeof(scalar_buf));
  memset(&point_buf, 0, sizeof(point_buf));
  memset(&out_buf, 0, sizeof(out_buf));

  status = zz9k_alloc_shared(ctx, ZZ9K_CRYPTO_P256_PRIVATE_BYTES, 16U, 0,
                             &scalar_buf);
  if (status != ZZ9K_STATUS_OK) {
    printf("p256 offload scalar alloc: %s (%d)\n", zz9k_status_name(status),
           status);
    goto out;
  }
  status = zz9k_alloc_shared(ctx, ZZ9K_CRYPTO_P256_POINT_BYTES, 16U, 0,
                             &point_buf);
  if (status != ZZ9K_STATUS_OK) {
    printf("p256 offload point alloc: %s (%d)\n", zz9k_status_name(status),
           status);
    goto out;
  }
  status = zz9k_alloc_shared(ctx, ZZ9K_CRYPTO_P256_SHARED_BYTES, 16U, 0,
                             &out_buf);
  if (status != ZZ9K_STATUS_OK) {
    printf("p256 offload output alloc: %s (%d)\n", zz9k_status_name(status),
           status);
    goto out;
  }

  memcpy((void *)scalar_buf.data, zz9k_cryptobench_p256_scalar,
         ZZ9K_CRYPTO_P256_PRIVATE_BYTES);
  memcpy((void *)point_buf.data, zz9k_cryptobench_p256_point,
         ZZ9K_CRYPTO_P256_POINT_BYTES);

  if (!zz9k_crypto_build_p256_desc(&desc, scalar_buf.handle, 0U,
                                   point_buf.handle, 0U, out_buf.handle, 0U)) {
    printf("p256 offload descriptor build failed\n");
    goto out;
  }

  start = zz9k_cryptobench_timer_now(timer);
  for (i = 0; i < count; i++) {
    memset(&result, 0, sizeof(result));
    status = zz9k_crypto_kx(ctx, &desc, &result);
    if (status != ZZ9K_STATUS_OK) {
      printf("p256 offload: %s (%d)\n", zz9k_status_name(status), status);
      goto out;
    }
  }
  elapsed = zz9k_cryptobench_timer_now(timer) - start;

  if (memcmp((const void *)out_buf.data, zz9k_cryptobench_p256_shared,
             ZZ9K_CRYPTO_P256_SHARED_BYTES) != 0) {
    printf("p256 offload: shared-secret verification FAILED\n");
    goto out;
  }
  ms_x100 = zz9k_cryptobench_ms_x100_per_op(elapsed, count,
                                            timer->ticks_per_second);

out:
  if (out_buf.handle != 0 && out_buf.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, out_buf.handle);
  }
  if (point_buf.handle != 0 && point_buf.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, point_buf.handle);
  }
  if (scalar_buf.handle != 0 && scalar_buf.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, scalar_buf.handle);
  }
  return ms_x100;
}

/* Times software ECDSA-P256 verification over `count` ops. Returns ms*100/op,
 * or 0 if the (valid) signature fails to verify. */
static uint32_t zz9k_cryptobench_ecdsa_soft_ms_x100(
    const ZZ9KCryptoBenchTimer *timer, uint32_t count)
{
  ZZ9KCryptoBenchTick start;
  ZZ9KCryptoBenchTick elapsed;
  uint32_t i;
  int valid = 0;

  start = zz9k_cryptobench_timer_now(timer);
  for (i = 0; i < count; i++) {
    valid = zz9k_soft_ecdsa_verify_p256(zz9k_cryptobench_ecdsa_sig,
                                        zz9k_cryptobench_ecdsa_sig + 32,
                                        zz9k_cryptobench_ecdsa_hash,
                                        zz9k_cryptobench_ecdsa_pub);
  }
  elapsed = zz9k_cryptobench_timer_now(timer) - start;
  if (!valid) {
    printf("ecdsa software: verification FAILED\n");
    return 0U;
  }
  return zz9k_cryptobench_ms_x100_per_op(elapsed, count,
                                         timer->ticks_per_second);
}

/* Times software RSA-2048 PKCS#1 v1.5 SHA-256 verification over `count` ops. */
static uint32_t zz9k_cryptobench_rsa_soft_ms_x100(
    const ZZ9KCryptoBenchTimer *timer, uint32_t count)
{
  ZZ9KCryptoBenchTick start;
  ZZ9KCryptoBenchTick elapsed;
  uint32_t i;
  int valid = 0;

  start = zz9k_cryptobench_timer_now(timer);
  for (i = 0; i < count; i++) {
    valid = zz9k_soft_rsa_verify_pkcs1_sha256(
        zz9k_cryptobench_rsa_sig, 256U, zz9k_cryptobench_rsa_hash,
        zz9k_cryptobench_rsa_mod, 2048U, 65537U);
  }
  elapsed = zz9k_cryptobench_timer_now(timer) - start;
  if (!valid) {
    printf("rsa software: verification FAILED\n");
    return 0U;
  }
  return zz9k_cryptobench_ms_x100_per_op(elapsed, count,
                                         timer->ticks_per_second);
}

/* Times ZZ9000 offload signature verification (ECDSA or RSA) over `count` ops.
 * The key buffer is `key`/`key_len`, signature `sig`/`sig_len`, digest 32-byte
 * SHA-256. Returns ms*100/op, or 0 on error or if a valid signature is reported
 * invalid. */
static uint32_t zz9k_cryptobench_verify_offload_ms_x100(
    ZZ9KContext *ctx, const ZZ9KCryptoBenchTimer *timer, uint32_t count,
    uint32_t algorithm, const uint8_t *hash, const uint8_t *sig,
    uint32_t sig_len, const uint8_t *key, uint32_t key_len, const char *label)
{
  ZZ9KSharedBuffer hash_buf;
  ZZ9KSharedBuffer sig_buf;
  ZZ9KSharedBuffer key_buf;
  ZZ9KCryptoVerifyDesc desc;
  ZZ9KCryptoBenchTick start;
  ZZ9KCryptoBenchTick elapsed;
  uint32_t ms_x100 = 0U;
  uint32_t i;
  int status;
  int valid = 0;

  memset(&hash_buf, 0, sizeof(hash_buf));
  memset(&sig_buf, 0, sizeof(sig_buf));
  memset(&key_buf, 0, sizeof(key_buf));

  status = zz9k_alloc_shared(ctx, ZZ9K_SOFT_SHA256_DIGEST_SIZE, 16U, 0,
                             &hash_buf);
  if (status != ZZ9K_STATUS_OK) {
    printf("%s offload hash alloc: %s (%d)\n", label, zz9k_status_name(status),
           status);
    goto out;
  }
  status = zz9k_alloc_shared(ctx, sig_len, 16U, 0, &sig_buf);
  if (status != ZZ9K_STATUS_OK) {
    printf("%s offload sig alloc: %s (%d)\n", label, zz9k_status_name(status),
           status);
    goto out;
  }
  status = zz9k_alloc_shared(ctx, key_len, 16U, 0, &key_buf);
  if (status != ZZ9K_STATUS_OK) {
    printf("%s offload key alloc: %s (%d)\n", label, zz9k_status_name(status),
           status);
    goto out;
  }

  memcpy((void *)hash_buf.data, hash, ZZ9K_SOFT_SHA256_DIGEST_SIZE);
  memcpy((void *)sig_buf.data, sig, sig_len);
  memcpy((void *)key_buf.data, key, key_len);

  if (!zz9k_crypto_build_verify_desc(&desc, algorithm, hash_buf.handle, 0U,
                                     ZZ9K_SOFT_SHA256_DIGEST_SIZE,
                                     sig_buf.handle, 0U, sig_len,
                                     key_buf.handle, 0U, key_len)) {
    printf("%s offload descriptor build failed\n", label);
    goto out;
  }

  start = zz9k_cryptobench_timer_now(timer);
  for (i = 0; i < count; i++) {
    valid = 0;
    status = zz9k_crypto_verify(ctx, &desc, &valid);
    if (status != ZZ9K_STATUS_OK) {
      printf("%s offload: %s (%d)\n", label, zz9k_status_name(status), status);
      goto out;
    }
  }
  elapsed = zz9k_cryptobench_timer_now(timer) - start;

  if (!valid) {
    printf("%s offload: verification FAILED "
           "(firmware reported a valid signature as invalid)\n", label);
    goto out;
  }
  ms_x100 = zz9k_cryptobench_ms_x100_per_op(elapsed, count,
                                            timer->ticks_per_second);

out:
  if (key_buf.handle != 0 && key_buf.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, key_buf.handle);
  }
  if (sig_buf.handle != 0 && sig_buf.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, sig_buf.handle);
  }
  if (hash_buf.handle != 0 && hash_buf.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, hash_buf.handle);
  }
  return ms_x100;
}

/* Prints a software-vs-offload asymmetric result block. off_ms_x100 == 0 means
 * the offload path was unavailable or failed; `note` (if non-NULL) explains. */
static void zz9k_cryptobench_report_asym(uint32_t soft_ms_x100,
                                         uint32_t off_ms_x100,
                                         const char *note)
{
  printf("  Software (m68k):  %lu.%02lu ms/op\n",
         (unsigned long)(soft_ms_x100 / 100U),
         (unsigned long)(soft_ms_x100 % 100U));
  if (off_ms_x100 > 0U && soft_ms_x100 > 0U) {
    uint32_t speedup_x100 =
        (uint32_t)(((uint64_t)soft_ms_x100 * 100ULL) / off_ms_x100);
    printf("  Offload (ZZ9000): %lu.%02lu ms/op  [%lu.%02lux speedup]\n",
           (unsigned long)(off_ms_x100 / 100U),
           (unsigned long)(off_ms_x100 % 100U),
           (unsigned long)(speedup_x100 / 100U),
           (unsigned long)(speedup_x100 % 100U));
  } else if (off_ms_x100 > 0U) {
    printf("  Offload (ZZ9000): %lu.%02lu ms/op\n",
           (unsigned long)(off_ms_x100 / 100U),
           (unsigned long)(off_ms_x100 % 100U));
  } else if (note != 0) {
    printf("  Offload (ZZ9000): %s\n", note);
  }
}

/* Times the software AEAD over `iterations` records of `size` bytes. */
static uint32_t zz9k_cryptobench_software_rate(
    const ZZ9KCryptoBenchTimer *timer, uint32_t size, uint32_t iterations,
    uint8_t *plaintext, uint8_t *ciphertext)
{
  static const uint8_t key[ZZ9K_CRYPTOBENCH_KEY_BYTES] = {
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f
  };
  static const uint8_t nonce[ZZ9K_CRYPTOBENCH_NONCE_BYTES] = {
    0x07, 0x00, 0x00, 0x00, 0x40, 0x41,
    0x42, 0x43, 0x44, 0x45, 0x46, 0x47
  };
  uint8_t tag[ZZ9K_CRYPTOBENCH_TAG_BYTES];
  ZZ9KCryptoBenchTick start;
  ZZ9KCryptoBenchTick elapsed;
  uint32_t i;

  start = zz9k_cryptobench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    zz9k_soft_chacha20_poly1305_encrypt(ciphertext, tag, plaintext, size,
                                        0, 0U, key, nonce);
  }
  elapsed = zz9k_cryptobench_timer_now(timer) - start;
  return zz9k_cryptobench_kib_per_second(size * iterations, elapsed,
                                         timer->ticks_per_second);
}

/* Times the ZZ9000 offload AEAD over `iterations` records of `size` bytes. */
static uint32_t zz9k_cryptobench_offload_rate(
    ZZ9KContext *ctx, const ZZ9KCryptoBenchTimer *timer, uint32_t size,
    uint32_t iterations, const ZZ9KSharedBuffer *input,
    const ZZ9KSharedBuffer *output, const ZZ9KSharedBuffer *key,
    const ZZ9KSharedBuffer *nonce)
{
  ZZ9KCryptoAeadDesc desc;
  ZZ9KCryptoResult result;
  ZZ9KCryptoBenchTick start;
  ZZ9KCryptoBenchTick elapsed;
  uint32_t i;
  int status;

  if (!zz9k_crypto_build_chacha20_poly1305_desc(
          &desc, input->handle, 0U, size, output->handle, 0U, 0U, 0U, 0U,
          key->handle, 0U, nonce->handle, 0U)) {
    printf("offload %5lu: descriptor build failed\n", (unsigned long)size);
    return 0;
  }

  start = zz9k_cryptobench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    memset(&result, 0, sizeof(result));
    status = zz9k_crypto_aead(ctx, &desc, &result);
    if (status != ZZ9K_STATUS_OK) {
      printf("offload %5lu: %s (%d)\n", (unsigned long)size,
             zz9k_status_name(status), status);
      return 0;
    }
  }
  elapsed = zz9k_cryptobench_timer_now(timer) - start;
  return zz9k_cryptobench_kib_per_second(size * iterations, elapsed,
                                         timer->ticks_per_second);
}

/* ---- AES-128-GCM record cipher: software vs synchronous vs batched offload ----
 *
 * Phase 0 found symmetric offload is mailbox-latency-bound: a synchronous call
 * per record loses to software below ~2 KB. Batching amortises the round trip
 * over many records via zz9k_crypto_aead_batch. This section quantifies that
 * directly for AES-128-GCM, the dominant TLS record cipher. */

#define ZZ9K_CRYPTOBENCH_GCM_BATCH 16U

static const uint8_t zz9k_cryptobench_aes_key[16] = {
  0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
  0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};

/* Software AES-128-GCM encryption rate over `iterations` records of `size`. */
static uint32_t zz9k_cryptobench_aes_gcm_software_rate(
    const ZZ9KCryptoBenchTimer *timer, uint32_t size, uint32_t iterations,
    uint8_t *plaintext, uint8_t *ciphertext)
{
  static const uint8_t nonce[ZZ9K_SOFT_AES_GCM_NONCE_BYTES] = {
    0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47
  };
  uint8_t tag[ZZ9K_SOFT_AES_GCM_TAG_BYTES];
  ZZ9KCryptoBenchTick start;
  ZZ9KCryptoBenchTick elapsed;
  uint32_t i;

  start = zz9k_cryptobench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    zz9k_soft_aes_gcm_encrypt(ciphertext, tag, plaintext, size, 0, 0U,
                              zz9k_cryptobench_aes_key, 16U, nonce);
  }
  elapsed = zz9k_cryptobench_timer_now(timer) - start;
  return zz9k_cryptobench_kib_per_second(size * iterations, elapsed,
                                         timer->ticks_per_second);
}

/* Synchronous AES-128-GCM offload rate (one mailbox round trip per record). */
static uint32_t zz9k_cryptobench_aes_gcm_offload_rate(
    ZZ9KContext *ctx, const ZZ9KCryptoBenchTimer *timer, uint32_t size,
    uint32_t iterations, const ZZ9KSharedBuffer *input,
    const ZZ9KSharedBuffer *output, const ZZ9KSharedBuffer *key,
    const ZZ9KSharedBuffer *nonce)
{
  ZZ9KCryptoAeadDesc desc;
  ZZ9KCryptoResult result;
  ZZ9KCryptoBenchTick start;
  ZZ9KCryptoBenchTick elapsed;
  uint32_t i;
  int status;

  if (!zz9k_crypto_build_aes_gcm_desc(&desc, input->handle, 0U, size,
          output->handle, 0U, 0U, 0U, 0U, key->handle, 0U,
          ZZ9K_CRYPTO_AES128_KEY_BYTES, nonce->handle, 0U)) {
    printf("aes-gcm offload %5lu: descriptor build failed\n",
           (unsigned long)size);
    return 0;
  }
  start = zz9k_cryptobench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    memset(&result, 0, sizeof(result));
    status = zz9k_crypto_aead(ctx, &desc, &result);
    if (status != ZZ9K_STATUS_OK) {
      printf("aes-gcm offload %5lu: %s (%d)\n", (unsigned long)size,
             zz9k_status_name(status), status);
      return 0;
    }
  }
  elapsed = zz9k_cryptobench_timer_now(timer) - start;
  return zz9k_cryptobench_kib_per_second(size * iterations, elapsed,
                                         timer->ticks_per_second);
}

/* Batched AES-128-GCM offload rate (ZZ9K_CRYPTOBENCH_GCM_BATCH records per
 * mailbox submission, pipelined). */
static uint32_t zz9k_cryptobench_aes_gcm_batch_rate(
    ZZ9KContext *ctx, const ZZ9KCryptoBenchTimer *timer, uint32_t size,
    uint32_t iterations, const ZZ9KSharedBuffer *input,
    const ZZ9KSharedBuffer *output, const ZZ9KSharedBuffer *key,
    const ZZ9KSharedBuffer *nonce)
{
  ZZ9KCryptoAeadDesc descs[ZZ9K_CRYPTOBENCH_GCM_BATCH];
  ZZ9KCryptoResult results[ZZ9K_CRYPTOBENCH_GCM_BATCH];
  ZZ9KCryptoBenchTick start;
  ZZ9KCryptoBenchTick elapsed;
  uint32_t i, b;
  int status;

  for (b = 0; b < ZZ9K_CRYPTOBENCH_GCM_BATCH; b++) {
    if (!zz9k_crypto_build_aes_gcm_desc(&descs[b], input->handle, 0U, size,
            output->handle, 0U, 0U, 0U, 0U, key->handle, 0U,
            ZZ9K_CRYPTO_AES128_KEY_BYTES, nonce->handle, 0U)) {
      printf("aes-gcm batch %5lu: descriptor build failed\n",
             (unsigned long)size);
      return 0;
    }
  }
  start = zz9k_cryptobench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    status = zz9k_crypto_aead_batch(ctx, descs, results,
                                    ZZ9K_CRYPTOBENCH_GCM_BATCH,
                                    ZZ9K_CRYPTOBENCH_GCM_BATCH,
                                    ZZ9K_DEFAULT_TIMEOUT_TICKS);
    if (status != ZZ9K_STATUS_OK) {
      printf("aes-gcm batch %5lu: %s (%d)\n", (unsigned long)size,
             zz9k_status_name(status), status);
      return 0;
    }
  }
  elapsed = zz9k_cryptobench_timer_now(timer) - start;
  return zz9k_cryptobench_kib_per_second(
      size * ZZ9K_CRYPTOBENCH_GCM_BATCH * iterations, elapsed,
      timer->ticks_per_second);
}

/* Allocate buffers and run the AES-128-GCM software/sync/batch sweep. The
 * offload columns are filled only when the crypto service advertises AES-GCM. */
static void zz9k_cryptobench_run_aes_gcm(
    ZZ9KContext *ctx, const ZZ9KCryptoBenchTimer *timer, uint32_t iterations,
    const uint32_t *sizes, uint8_t *plaintext, uint8_t *ciphertext,
    int offload_ok)
{
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer output;
  ZZ9KSharedBuffer key;
  ZZ9KSharedBuffer nonce;
  uint32_t i;
  int have_buffers = 0;
  int status;

  memset(&input, 0, sizeof(input));
  memset(&output, 0, sizeof(output));
  memset(&key, 0, sizeof(key));
  memset(&nonce, 0, sizeof(nonce));

  if (offload_ok) {
    status = zz9k_alloc_shared(ctx, ZZ9K_CRYPTOBENCH_MAX_RECORD_BYTES, 16U, 0,
                               &input);
    if (status == ZZ9K_STATUS_OK) {
      status = zz9k_alloc_shared(
          ctx, ZZ9K_CRYPTOBENCH_MAX_RECORD_BYTES + ZZ9K_CRYPTOBENCH_TAG_BYTES,
          16U, 0, &output);
    }
    if (status == ZZ9K_STATUS_OK) {
      status = zz9k_alloc_shared(ctx, ZZ9K_CRYPTO_AES128_KEY_BYTES, 16U, 0,
                                 &key);
    }
    if (status == ZZ9K_STATUS_OK) {
      status = zz9k_alloc_shared(ctx, ZZ9K_CRYPTO_AES_GCM_NONCE_BYTES, 16U, 0,
                                 &nonce);
    }
    if (status == ZZ9K_STATUS_OK) {
      have_buffers = 1;
      zz9k_cryptobench_fill_pattern((uint8_t *)input.data,
                                    ZZ9K_CRYPTOBENCH_MAX_RECORD_BYTES);
      memcpy((void *)key.data, zz9k_cryptobench_aes_key,
             ZZ9K_CRYPTO_AES128_KEY_BYTES);
      zz9k_cryptobench_fill_pattern((uint8_t *)nonce.data,
                                    ZZ9K_CRYPTO_AES_GCM_NONCE_BYTES);
    }
  }

  printf("\nAES-128-GCM record cipher (%lu iterations/size, batch depth %lu)\n",
         (unsigned long)iterations,
         (unsigned long)ZZ9K_CRYPTOBENCH_GCM_BATCH);
  printf("%6s  %10s  %10s  %12s\n", "bytes", "soft KiB/s", "sync KiB/s",
         "batch KiB/s");
  for (i = 0; i < ZZ9K_CRYPTOBENCH_SWEEP_COUNT; i++) {
    uint32_t soft = zz9k_cryptobench_aes_gcm_software_rate(
        timer, sizes[i], iterations, plaintext, ciphertext);
    if (have_buffers) {
      uint32_t sync = zz9k_cryptobench_aes_gcm_offload_rate(
          ctx, timer, sizes[i], iterations, &input, &output, &key, &nonce);
      uint32_t batch = zz9k_cryptobench_aes_gcm_batch_rate(
          ctx, timer, sizes[i], iterations, &input, &output, &key, &nonce);
      printf("%6lu  %10lu  %10lu  %12lu\n", (unsigned long)sizes[i],
             (unsigned long)soft, (unsigned long)sync, (unsigned long)batch);
    } else {
      printf("%6lu  %10lu  %10s  %12s\n", (unsigned long)sizes[i],
             (unsigned long)soft, "-", "-");
    }
  }

  if (nonce.handle != 0 && nonce.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, nonce.handle);
  }
  if (key.handle != 0 && key.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, key.handle);
  }
  if (output.handle != 0 && output.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, output.handle);
  }
  if (input.handle != 0 && input.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, input.handle);
  }
}

static int zz9k_cryptobench_run_offload_sweep(
    ZZ9KContext *ctx, const ZZ9KCryptoBenchTimer *timer, uint32_t iterations,
    const uint32_t *sizes, const uint32_t *software_kib,
    uint32_t *offload_kib)
{
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer output;
  ZZ9KSharedBuffer key;
  ZZ9KSharedBuffer nonce;
  uint32_t i;
  int status;
  int rc = 1;

  memset(&input, 0, sizeof(input));
  memset(&output, 0, sizeof(output));
  memset(&key, 0, sizeof(key));
  memset(&nonce, 0, sizeof(nonce));

  status = zz9k_alloc_shared(ctx, ZZ9K_CRYPTOBENCH_MAX_RECORD_BYTES, 16U, 0,
                             &input);
  if (status != ZZ9K_STATUS_OK) {
    printf("offload input alloc: %s (%d)\n", zz9k_status_name(status),
           status);
    goto out;
  }
  status = zz9k_alloc_shared(
      ctx, ZZ9K_CRYPTOBENCH_MAX_RECORD_BYTES + ZZ9K_CRYPTOBENCH_TAG_BYTES,
      16U, 0, &output);
  if (status != ZZ9K_STATUS_OK) {
    printf("offload output alloc: %s (%d)\n", zz9k_status_name(status),
           status);
    goto out;
  }
  status = zz9k_alloc_shared(ctx, ZZ9K_CRYPTOBENCH_KEY_BYTES, 16U, 0, &key);
  if (status != ZZ9K_STATUS_OK) {
    printf("offload key alloc: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }
  status = zz9k_alloc_shared(ctx, ZZ9K_CRYPTOBENCH_NONCE_BYTES, 16U, 0,
                             &nonce);
  if (status != ZZ9K_STATUS_OK) {
    printf("offload nonce alloc: %s (%d)\n", zz9k_status_name(status),
           status);
    goto out;
  }

  zz9k_cryptobench_fill_pattern((uint8_t *)input.data,
                                ZZ9K_CRYPTOBENCH_MAX_RECORD_BYTES);
  zz9k_cryptobench_fill_pattern((uint8_t *)key.data,
                                ZZ9K_CRYPTOBENCH_KEY_BYTES);
  zz9k_cryptobench_fill_pattern((uint8_t *)nonce.data,
                                ZZ9K_CRYPTOBENCH_NONCE_BYTES);

  for (i = 0; i < ZZ9K_CRYPTOBENCH_SWEEP_COUNT; i++) {
    offload_kib[i] = zz9k_cryptobench_offload_rate(
        ctx, timer, sizes[i], iterations, &input, &output, &key, &nonce);
    printf("%6lu  %10lu  %10lu\n", (unsigned long)sizes[i],
           (unsigned long)software_kib[i], (unsigned long)offload_kib[i]);
  }
  rc = 0;

out:
  if (nonce.handle != 0 && nonce.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, nonce.handle);
  }
  if (key.handle != 0 && key.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, key.handle);
  }
  if (output.handle != 0 && output.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, output.handle);
  }
  if (input.handle != 0 && input.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, input.handle);
  }
  return rc;
}

int main(int argc, char **argv)
{
  static uint8_t plaintext[ZZ9K_CRYPTOBENCH_MAX_RECORD_BYTES];
  static uint8_t ciphertext[ZZ9K_CRYPTOBENCH_MAX_RECORD_BYTES +
                            ZZ9K_CRYPTOBENCH_TAG_BYTES];
  uint32_t sizes[ZZ9K_CRYPTOBENCH_SWEEP_COUNT];
  uint32_t software_kib[ZZ9K_CRYPTOBENCH_SWEEP_COUNT];
  uint32_t offload_kib[ZZ9K_CRYPTOBENCH_SWEEP_COUNT];
  ZZ9KCryptoBenchTimer timer;
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  uint32_t iterations;
  uint32_t i;
  uint32_t breakeven;
  int status;

  iterations = zz9k_cryptobench_parse_iterations(argc, argv);
  zz9k_cryptobench_fill_pattern(plaintext, sizeof(plaintext));
  zz9k_cryptobench_timer_open(&timer);

  for (i = 0; i < ZZ9K_CRYPTOBENCH_SWEEP_COUNT; i++) {
    sizes[i] = zz9k_cryptobench_sweep_size(i);
    offload_kib[i] = 0U;
  }

  /* Open and probe the board before the software sweep: the sweep can run
   * for a minute or more, and the Zorro II limitation note must be the
   * first thing the user sees, not a post-mortem. A failed open (plain
   * host run) keeps the tool on its software-only path below. */
  status = zz9k_open(&ctx);
  if (status == ZZ9K_STATUS_OK) {
    status = zz9k_query_caps(ctx, &caps);
  }
  if (status == ZZ9K_STATUS_OK &&
      (caps.capability_bits & ZZ9K_CAP_CRYPTO) != 0U &&
      !zz9k_shared_heap_board_visible(ctx)) {
    printf("note: this diagnostic stages its offload buffers in the default\n"
           "shared heap, which is not CPU-visible on Zorro II. The offload\n"
           "sections cannot run here; qualify crypto offload with the\n"
           "accelerated AmiSSL provider instead (docs/zz9k-zorro2-services.md).\n");
  }

  printf("ChaCha20-Poly1305 break-even sweep (%lu iterations/size)\n",
         (unsigned long)iterations);
  printf("%6s  %10s  %10s\n", "bytes", "soft KiB/s", "off KiB/s");

  for (i = 0; i < ZZ9K_CRYPTOBENCH_SWEEP_COUNT; i++) {
    software_kib[i] = zz9k_cryptobench_software_rate(
        &timer, sizes[i], iterations, plaintext, ciphertext);
  }

  if (status == ZZ9K_STATUS_OK &&
      (caps.capability_bits & ZZ9K_CAP_CRYPTO) != 0U) {
    if (zz9k_cryptobench_run_offload_sweep(ctx, &timer, iterations, sizes,
                                           software_kib, offload_kib) != 0) {
      zz9k_close(ctx);
      zz9k_cryptobench_timer_close(&timer);
      return 1;
    }
  } else {
    printf("crypto offload unavailable; software sweep only:\n");
    for (i = 0; i < ZZ9K_CRYPTOBENCH_SWEEP_COUNT; i++) {
      printf("%6lu  %10lu  %10s\n", (unsigned long)sizes[i],
             (unsigned long)software_kib[i], "-");
    }
  }

  breakeven = zz9k_cryptobench_breakeven_size(sizes, software_kib,
                                              offload_kib,
                                              ZZ9K_CRYPTOBENCH_SWEEP_COUNT);
  if (breakeven != 0U) {
    printf("break-even: offload wins from %lu bytes\n",
           (unsigned long)breakeven);
  } else {
    printf("break-even: offload did not beat software in this sweep\n");
  }

  /* X25519 key-exchange benchmark */
  {
    uint32_t soft_ms_x100;
    uint32_t off_ms_x100 = 0U;
    ZZ9KServiceInfo crypto_service;

    printf("\nX25519 key exchange (%lu iterations):\n",
           (unsigned long)iterations);

    soft_ms_x100 = zz9k_cryptobench_x25519_soft_ms_x100(&timer, iterations);
    printf("  Software (m68k):  %lu.%02lu ms/op\n",
           (unsigned long)(soft_ms_x100 / 100U),
           (unsigned long)(soft_ms_x100 % 100U));

    if (ctx != 0 && status == ZZ9K_STATUS_OK &&
        (caps.capability_bits & ZZ9K_CAP_CRYPTO) != 0U) {
      int qs = zz9k_query_service(ctx, ZZ9K_SERVICE_CRYPTO, &crypto_service);
      if (qs == ZZ9K_STATUS_OK &&
          (crypto_service.flags & ZZ9K_SERVICE_FLAG_CRYPTO_X25519) != 0U) {
        off_ms_x100 = zz9k_cryptobench_x25519_offload_ms_x100(
            ctx, &timer, iterations);
        if (off_ms_x100 > 0U && soft_ms_x100 > 0U) {
          uint32_t speedup_x100 = (uint32_t)(
              ((uint64_t)soft_ms_x100 * 100ULL) / off_ms_x100);
          printf("  Offload (ZZ9000): %lu.%02lu ms/op  [%lu.%02lux speedup]\n",
                 (unsigned long)(off_ms_x100 / 100U),
                 (unsigned long)(off_ms_x100 % 100U),
                 (unsigned long)(speedup_x100 / 100U),
                 (unsigned long)(speedup_x100 % 100U));
        } else if (off_ms_x100 > 0U) {
          printf("  Offload (ZZ9000): %lu.%02lu ms/op\n",
                 (unsigned long)(off_ms_x100 / 100U),
                 (unsigned long)(off_ms_x100 % 100U));
        } else {
          printf("  Offload (ZZ9000): measurement failed\n");
        }
      } else {
        printf("  Offload (ZZ9000): X25519 not advertised by crypto service\n");
      }
    }
  }

  /* P-256 ECDH / ECDSA-P256 / RSA-2048 verify handshake benchmarks */
  {
    int crypto_ok = (ctx != 0 && status == ZZ9K_STATUS_OK &&
                     (caps.capability_bits & ZZ9K_CAP_CRYPTO) != 0U);
    ZZ9KServiceInfo svc;
    uint32_t svc_flags = 0U;
    uint32_t soft;
    uint32_t off;
    const char *note;

    if (crypto_ok &&
        zz9k_query_service(ctx, ZZ9K_SERVICE_CRYPTO, &svc) == ZZ9K_STATUS_OK) {
      svc_flags = svc.flags;
    }

    /* P-256 ECDH */
    printf("\nP-256 ECDH (%lu iterations):\n", (unsigned long)iterations);
    soft = zz9k_cryptobench_p256_soft_ms_x100(&timer, iterations);
    off = 0U;
    note = 0;
    if (crypto_ok) {
      if ((svc_flags & ZZ9K_SERVICE_FLAG_CRYPTO_P256) != 0U) {
        off = zz9k_cryptobench_p256_offload_ms_x100(ctx, &timer, iterations);
        if (off == 0U) {
          note = "measurement failed";
        }
      } else {
        note = "P-256 not advertised by crypto service";
      }
    }
    zz9k_cryptobench_report_asym(soft, off, note);

    /* P-256 keygen (ephemeral key share: public = scalar*G) */
    printf("\nP-256 keygen (%lu iterations):\n", (unsigned long)iterations);
    soft = zz9k_cryptobench_p256_keygen_soft_ms_x100(&timer, iterations);
    off = 0U;
    note = 0;
    if (crypto_ok) {
      if ((svc_flags & ZZ9K_SERVICE_FLAG_CRYPTO_P256_KEYGEN) != 0U) {
        off = zz9k_cryptobench_p256_keygen_offload_ms_x100(ctx, &timer,
                                                           iterations);
        if (off == 0U) {
          note = "measurement failed";
        }
      } else {
        note = "P-256 keygen not advertised by crypto service";
      }
    }
    zz9k_cryptobench_report_asym(soft, off, note);

    /* ECDSA-P256 verify */
    printf("\nECDSA-P256 verify (%lu iterations):\n", (unsigned long)iterations);
    soft = zz9k_cryptobench_ecdsa_soft_ms_x100(&timer, iterations);
    off = 0U;
    note = 0;
    if (crypto_ok) {
      if ((svc_flags & ZZ9K_SERVICE_FLAG_CRYPTO_ECDSA_P256) != 0U) {
        off = zz9k_cryptobench_verify_offload_ms_x100(
            ctx, &timer, iterations, ZZ9K_CRYPTO_VERIFY_ECDSA_P256_SHA256,
            zz9k_cryptobench_ecdsa_hash, zz9k_cryptobench_ecdsa_sig,
            sizeof(zz9k_cryptobench_ecdsa_sig), zz9k_cryptobench_ecdsa_pub,
            sizeof(zz9k_cryptobench_ecdsa_pub), "ecdsa");
        if (off == 0U) {
          note = "measurement failed";
        }
      } else {
        note = "ECDSA-P256 not advertised by crypto service";
      }
    }
    zz9k_cryptobench_report_asym(soft, off, note);

    /* RSA-2048 verify */
    {
      uint8_t rsa_key[256 + 4];
      memcpy(rsa_key, zz9k_cryptobench_rsa_mod, 256U);
      memcpy(rsa_key + 256, zz9k_cryptobench_rsa_exp, 4U);

      printf("\nRSA-2048 verify (e=65537, %lu iterations):\n",
             (unsigned long)iterations);
      soft = zz9k_cryptobench_rsa_soft_ms_x100(&timer, iterations);
      off = 0U;
      note = 0;
      if (crypto_ok) {
        if ((svc_flags & ZZ9K_SERVICE_FLAG_CRYPTO_RSA_2048) != 0U) {
          off = zz9k_cryptobench_verify_offload_ms_x100(
              ctx, &timer, iterations, ZZ9K_CRYPTO_VERIFY_RSA_PKCS1_2048_SHA256,
              zz9k_cryptobench_rsa_hash, zz9k_cryptobench_rsa_sig,
              sizeof(zz9k_cryptobench_rsa_sig), rsa_key, sizeof(rsa_key),
              "rsa");
          if (off == 0U) {
            note = "measurement failed";
          }
        } else {
          note = "RSA-2048 not advertised by crypto service";
        }
      }
      zz9k_cryptobench_report_asym(soft, off, note);
    }

    zz9k_cryptobench_run_aes_gcm(
        ctx, &timer, iterations, sizes, plaintext, ciphertext,
        crypto_ok && (svc_flags & ZZ9K_SERVICE_FLAG_CRYPTO_AES_GCM) != 0U);
  }

  if (ctx) {
    zz9k_close(ctx);
  }
  zz9k_cryptobench_timer_close(&timer);
  return 0;
}

#endif /* ZZ9K_CRYPTOBENCH_NO_MAIN */
