/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * End-to-end host test: decode real .lzh fixtures
 * (util/lha_real_fixtures.h, 5 real -lh5- archives sliced from Aminet) all
 * the way through the Task-6 sdk_decompress_lzh() backend -- the same
 * vendored LHa for UNIX decode core (Task 4) + buffer-backed I/O shim
 * (Task 5) the firmware links -- and confirm the output is byte-exact with
 * the known-good plaintext, the CRC-16 matches each archive's stored
 * value, and a truncated input fails cleanly without an out-of-bounds
 * write. Also covers the fatal_error()/lha_exit() longjmp recovery path
 * (undersized dst_capacity) to confirm the `dtext` decode-scratch buffer
 * doesn't leak when a longjmp unwinds past slide.c's decode().
 *
 * Build + run (gcc:13 Docker image, repo root mounted at /src):
 *
 *   MSYS_NO_PATHCONV=1 docker run --rm -v "/d/Github/zz9000-firmware":/src \
 *     -w /src gcc:13 sh -c 'cd ZZ9000_proto.sdk/ZZ9000OS/src/lzh && \
 *     gcc -DHAVE_CONFIG_H=1 -I. -I/src/ZZ9000_proto.sdk/ZZ9000OS/src \
 *     -I/src/util -std=gnu99 /src/util/test_sdk_lzh_decode.c extract.c \
 *     huf.c dhuf.c shuf.c slide.c larc.c maketbl.c maketree.c \
 *     zz9k_lzh_bitio.c zz9k_lzh_crcio.c zz9k_lzh_support.c \
 *     sdk_decompress_lzh.c -o /tmp/e2e && /tmp/e2e'
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zz9k_lzh.h"
#include "lha_real_fixtures.h"

/* Not part of zz9k_lzh.h's public API -- lha.h's EXTERN globals mechanism
 * (ZZ9000_proto.sdk/ZZ9000OS/src/lzh/lha.h:374) declares this the same way
 * for every vendored-core translation unit; test_longjmp_recovery_frees_
 * dtext() below needs to inspect it directly rather than pull in the whole
 * of lha.h. Defined in zz9k_lzh_support.c. */
extern unsigned char *dtext;

static int g_failures;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
            g_failures++; \
        } \
    } while (0)

static uint32_t
method_to_algorithm(unsigned method)
{
    switch (method) {
    case 1: return SDK_COMPRESSION_LH1;
    case 5: return SDK_COMPRESSION_LH5;
    case 6: return SDK_COMPRESSION_LH6;
    case 7: return SDK_COMPRESSION_LH7;
    default: return 0xFFFFFFFFU;  /* deliberately unsupported */
    }
}

static void
test_fixture_decodes_correctly(const ZZ9KLhaFixture *fx)
{
    uint32_t algorithm = method_to_algorithm(fx->method);
    uint8_t *dst;
    struct SDKDecompressResult result;
    uint16_t status;
    char msg[256];

    dst = (uint8_t *)malloc(fx->uncompressed_size ? fx->uncompressed_size : 1U);
    if (!dst) {
        printf("FAIL %s: out of memory\n", fx->name);
        g_failures++;
        return;
    }
    /* Poison the buffer so a short/partial decode is obvious. */
    memset(dst, 0xa5, fx->uncompressed_size);

    status = sdk_decompress_lzh(algorithm, fx->compressed,
                                (uint32_t)fx->compressed_size,
                                dst, (uint32_t)fx->uncompressed_size,
                                &result);

    snprintf(msg, sizeof msg,
             "%s: sdk_decompress_lzh returns SDK_STATUS_OK (got %u)",
             fx->name, (unsigned)status);
    CHECK(status == SDK_STATUS_OK, msg);

    snprintf(msg, sizeof msg,
             "%s: bytes_written (%u) == uncompressed_size (%u)",
             fx->name, (unsigned)result.bytes_written, fx->uncompressed_size);
    CHECK(result.bytes_written == fx->uncompressed_size, msg);

    snprintf(msg, sizeof msg,
             "%s: checksum 0x%04x matches expected crc16 0x%04x",
             fx->name, (unsigned)(uint16_t)result.checksum, fx->crc16);
    CHECK((uint16_t)result.checksum == fx->crc16, msg);

    snprintf(msg, sizeof msg,
             "%s: decoded bytes are byte-exact with expected plaintext",
             fx->name);
    CHECK(memcmp(dst, fx->expected, fx->uncompressed_size) == 0, msg);

    snprintf(msg, sizeof msg,
             "%s: result.flags has SDK_DECOMPRESS_RESULT_CHECKSUM_VALID set",
             fx->name);
    CHECK((result.flags & SDK_DECOMPRESS_RESULT_CHECKSUM_VALID) != 0U, msg);

    free(dst);
}

/*
 * Truncated input: pass compressed_size - 1 (one real compressed byte
 * withheld). Because compsize (the "how many real compressed bytes remain"
 * gate in bitio.c's fillbuf()) is derived from the very same src_length the
 * caller passes in, a truncated buffer stays self-consistent from the
 * decoder's point of view: fillbuf() zero-pads once compsize/the membuf
 * cursor both run out, rather than always faulting (see
 * test_sdk_lzh_shim.c's compsize-gating test). Empirically, only some
 * fixtures hit the harder fatal_error("cannot read stream")/lha_exit()
 * fault (SDK_STATUS_BAD_REQUEST) on a 1-byte truncation; the rest silently
 * finish with the right LENGTH but wrong trailing bytes -- exactly the
 * class of error the CRC-16 exists to catch (SDK_DECOMPRESS_RESULT_
 * CHECKSUM_VALID is documented -- see zz9k_lha_unix.c's decode_accept() --
 * as the AUTHORITATIVE integrity signal, not the status code alone). So the
 * correctness assertion here is deliberately an OR: either the call already
 * reports non-OK, or -- if it reports OK anyway -- its own CRC-16 must not
 * match the archive's, so a caller checking the checksum (as it always
 * should) never mistakes a truncated decode for a good one. Independent of
 * either outcome, the decode must never write past the declared
 * dst_capacity: the membuf shim's dst cursor is hard-bounded (Task 5), and a
 * canary byte placed just past dst_capacity is checked here as a
 * belt-and-suspenders guard on top of that.
 */
static void
test_truncated_input_fails_cleanly(const ZZ9KLhaFixture *fx)
{
    uint32_t algorithm = method_to_algorithm(fx->method);
    uint8_t *buf;
    uint8_t *dst;
    struct SDKDecompressResult result;
    uint16_t status;
    char msg[256];
    static const uint8_t canary = 0x5aU;

    if (fx->compressed_size == 0U)
        return;

    buf = (uint8_t *)malloc((size_t)fx->uncompressed_size + 1U);
    if (!buf) {
        printf("FAIL %s (truncated): out of memory\n", fx->name);
        g_failures++;
        return;
    }
    dst = buf;
    memset(dst, 0xa5, fx->uncompressed_size);
    dst[fx->uncompressed_size] = canary;

    status = sdk_decompress_lzh(algorithm, fx->compressed,
                                (uint32_t)fx->compressed_size - 1U,
                                dst, (uint32_t)fx->uncompressed_size,
                                &result);

    snprintf(msg, sizeof msg,
             "%s: truncated input (compressed_size-1) does not report "
             "SDK_STATUS_OK with a correct decode (got status=%u, crc=0x%04x "
             "vs expected 0x%04x)",
             fx->name, (unsigned)status, (unsigned)(uint16_t)result.checksum,
             fx->crc16);
    CHECK(status != SDK_STATUS_OK ||
              (uint16_t)result.checksum != fx->crc16,
          msg);

    snprintf(msg, sizeof msg,
             "%s: truncated decode must not write past dst_capacity",
             fx->name);
    CHECK(dst[fx->uncompressed_size] == canary, msg);

    free(buf);
}

/*
 * Longjmp recovery path: prove the `dtext` decode-scratch buffer (slide.c's
 * `dtext = xmalloc(dicsiz)`, freed only on decode()'s NORMAL return at
 * slide.c:484) does not leak when a fatal_error()/lha_exit() longjmp
 * unwinds out of decode() -- see sdk_decompress_lzh.c's setjmp recovery
 * branch.
 *
 * Trigger: a dst_capacity deliberately smaller than the fixture's real
 * uncompressed_size. decode() xmalloc(dicsiz)s `dtext` unconditionally,
 * before it ever looks at dst_capacity (dicsiz depends only on the LHA
 * method's dictionary bits), so a small enough dst_capacity makes the
 * decode loop's final unflushed run of bytes overflow the membuf: the
 * shim's fwrite_crc() -> zz9k_lzh_dst_write() reports a short write ->
 * fatal_error() -> longjmp, firing *after* `dtext` was allocated but
 * *before* slide.c reaches its own free(dtext). This is exactly the leak
 * window the fix closes.
 *
 * dst_capacity=8 against fixture 0 (off-popp.lha:.readme, real
 * uncompressed_size=2109) was empirically pinned by driving
 * sdk_decompress_lzh() over a range of reduced capacities and checking
 * zz9k_lzh_error: it reproducibly lands mid-match (as opposed to some
 * other reduced capacities, which land exactly on a token boundary and
 * decode a clean, merely-truncated result with no longjmp at all -- see
 * test_truncated_input_fails_cleanly() above for that other case).
 * zz9k_lzh_error is the discriminator that PROVES the longjmp path was
 * actually taken: a plain CRC-mismatch/short-decode failure never sets it.
 */
static void
test_longjmp_recovery_frees_dtext(void)
{
    const ZZ9KLhaFixture *fx = &zz9k_lha_fixtures[0]; /* off-popp.lha:.readme */
    uint32_t algorithm = method_to_algorithm(fx->method);
    static const uint32_t undersized_capacity = 8U;
    uint8_t dst[16];
    struct SDKDecompressResult result;
    uint16_t status;
    char msg[256];

    memset(dst, 0xa5, sizeof dst);
    zz9k_lzh_error = 0;

    status = sdk_decompress_lzh(algorithm, fx->compressed,
                                (uint32_t)fx->compressed_size,
                                dst, undersized_capacity, &result);

    snprintf(msg, sizeof msg,
             "%s: undersized dst_capacity (%u) must not report "
             "SDK_STATUS_OK (got %u)",
             fx->name, (unsigned)undersized_capacity, (unsigned)status);
    CHECK(status != SDK_STATUS_OK, msg);

    snprintf(msg, sizeof msg,
             "%s: undersized dst_capacity must take the fatal_error()/"
             "lha_exit() longjmp path (zz9k_lzh_error == 1 proves it; "
             "got %d)",
             fx->name, zz9k_lzh_error);
    CHECK(zz9k_lzh_error == 1, msg);

    snprintf(msg, sizeof msg,
             "%s: dtext scratch buffer must be freed (NULL) after longjmp "
             "recovery, not left dangling for the next decode call to leak "
             "(got %p)",
             fx->name, (void *)dtext);
    CHECK(dtext == NULL, msg);
}

int
main(void)
{
    unsigned i;

    for (i = 0; i < ZZ9K_LHA_FIXTURE_COUNT; i++) {
        test_fixture_decodes_correctly(&zz9k_lha_fixtures[i]);
    }

    for (i = 0; i < ZZ9K_LHA_FIXTURE_COUNT; i++) {
        test_truncated_input_fails_cleanly(&zz9k_lha_fixtures[i]);
    }

    test_longjmp_recovery_frees_dtext();

    if (g_failures != 0) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }

    printf("OK\n");
    return 0;
}
