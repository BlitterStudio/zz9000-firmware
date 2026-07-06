/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Host unit tests for the firmware's buffer-backed LZH I/O shim
 * (ZZ9000_proto.sdk/ZZ9000OS/src/lzh/zz9k_lzh_{bitio,crcio,support}.c).
 *
 * These exercise ONLY the shim's own I/O primitives (getbits/fillbuf via
 * init_getbits, fwrite_crc, make_crctable/calccrc) -- not the vendored
 * decode core (huf.c/slide.c/etc, Task 4) or the sdk_decompress_lzh()
 * backend (Task 6). No archive fixture is needed for that reason.
 *
 * Build + run (gcc:13 Docker image, repo root mounted at /src):
 *
 *   MSYS_NO_PATHCONV=1 docker run --rm -v "/d/Github/zz9000-firmware":/src \
 *     -w /src gcc:13 sh -c 'cd ZZ9000_proto.sdk/ZZ9000OS/src/lzh && \
 *     gcc -DHAVE_CONFIG_H=1 -I. -I/src/ZZ9000_proto.sdk/ZZ9000OS/src \
 *     -std=gnu99 /src/util/test_sdk_lzh_shim.c zz9k_lzh_bitio.c \
 *     zz9k_lzh_crcio.c zz9k_lzh_support.c -o /tmp/shim && /tmp/shim'
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lha.h"
#include "zz9k_lzh.h"

/* Host utility tests have no ARM cache maintenance to perform, but
 * zz9k_lzh_support.c still links against these reclaim visibility hooks. */
void zz9k_lzh_flush_dtext_reclaim(void) {}
void zz9k_lzh_invalidate_dtext_reclaim(void) {}

/* ------------------------------------------------------------------------ */
/* Independent bitwise reference for the LHA/ARC CRC-16 (reflected,         */
/* polynomial 0xA001, init 0x0000, no final xor). Deliberately NOT sharing  */
/* any code with make_crctable()/calccrc()/UPDATE_CRC() -- this is the      */
/* thing the shim's table-driven CRC is checked against.                   */
/* ------------------------------------------------------------------------ */
static unsigned int
reference_crc16(const unsigned char *data, size_t len)
{
    unsigned int crc = 0x0000U;
    size_t i;

    for (i = 0; i < len; i++) {
        int bit;

        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1U)
                crc = (crc >> 1) ^ 0xA001U;
            else
                crc >>= 1;
        }
    }
    return crc & 0xFFFFU;
}

/* ------------------------------------------------------------------------ */

static int g_failures;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
            g_failures++; \
        } \
    } while (0)

/* ------------------------------------------------------------------------ */
/* membuf input: known buffer -> getbits()/fillbuf() after init_getbits().  */
/* Also exercises compsize gating: once compsize correctly reaches 0 in     */
/* lockstep with the real source length, fillbuf() zero-pads instead of     */
/* erroring (upstream behavior -- decode() knows to stop via origsize/      */
/* decode_count, not by relying on a real read failure).                    */
/* ------------------------------------------------------------------------ */
static void
test_bitio_basic_and_compsize_gating(void)
{
    static const uint8_t src[] = { 0xAB, 0xCD, 0xEF, 0x12 };
    uint8_t dst[8];
    unsigned short v;

    if (zz9k_lzh_setjmp_failed()) {
        CHECK(0, "unexpected fatal_error during well-formed bitio read");
        return;
    }

    zz9k_lzh_io_begin(src, (uint32_t)sizeof(src), dst, (uint32_t)sizeof(dst));
    compsize = (off_t)sizeof(src);   /* matches the real source length */
    origsize = 0;
    unpackable = 0;

    init_getbits();     /* pre-loads bitbuf with the first 2 bytes */

    v = getbits(8);
    CHECK(v == 0xAB, "getbits(8) #1 != 0xAB");
    v = getbits(8);
    CHECK(v == 0xCD, "getbits(8) #2 != 0xCD");
    v = getbits(8);
    CHECK(v == 0xEF, "getbits(8) #3 != 0xEF");
    v = getbits(8);
    CHECK(v == 0x12, "getbits(8) #4 != 0x12");

    /* All 4 source bytes consumed and compsize reached 0 exactly in step;
     * a further read must NOT fault -- it zero-pads. */
    CHECK(compsize == 0, "compsize should have reached exactly 0");
    v = getbits(8);
    CHECK(v == 0x00, "post-exhaustion getbits(8) should zero-pad, not fault");
}

/* ------------------------------------------------------------------------ */
/* membuf input: a stream shorter than what compsize claims (a genuinely    */
/* truncated/corrupt compressed buffer) must hit the decoder's EOF path --  */
/* fillbuf()'s fatal_error("cannot read stream") -- caught here via         */
/* zz9k_lzh_setjmp_failed()'s longjmp recovery.                             */
/* ------------------------------------------------------------------------ */
static void
test_bitio_past_src_len_is_fatal(void)
{
    static const uint8_t src[] = { 0xAA, 0xBB };   /* only 2 real bytes */
    uint8_t dst[8];

    zz9k_lzh_io_begin(src, (uint32_t)sizeof(src), dst, (uint32_t)sizeof(dst));
    /* Claim far more compressed bytes than are actually present. */
    compsize = 8;
    origsize = 0;
    unpackable = 0;

    if (zz9k_lzh_setjmp_failed()) {
        CHECK(zz9k_lzh_error == 1, "zz9k_lzh_error not set after fatal_error");
        CHECK(zz9k_lzh_fatal_message() != NULL &&
              strstr(zz9k_lzh_fatal_message(), "cannot read stream") != NULL,
              "fatal_error message doesn't mention the stream read failure");
        return; /* expected path */
    }

    init_getbits();   /* consumes both real bytes; compsize still > 0 */
    getbits(8);       /* fillbuf() tries another read -> shim EOF -> fatal_error()
                        * longjmps back to the setjmp_failed() check above */
    CHECK(0, "expected a fatal_error()/longjmp on truncated input, got none");
}

/* ------------------------------------------------------------------------ */
/* membuf output: fwrite_crc() lands bytes in dst; writing past dst_cap     */
/* sets zz9k_lzh_io_overflowed() and (via fatal_error()) longjmps.          */
/* ------------------------------------------------------------------------ */
static void
test_crcio_write_and_overflow(void)
{
    static const unsigned char payload[4] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t dst[4];
    unsigned int crc;

    zz9k_lzh_io_begin(NULL, 0, dst, (uint32_t)sizeof(dst));
    verify_mode = 0;
    memset(dst, 0, sizeof(dst));
    crc = 0;

    if (zz9k_lzh_setjmp_failed()) {
        CHECK(0, "unexpected fatal_error while filling dst exactly to capacity");
        return;
    }

    fwrite_crc(&crc, (void *)payload, (int)sizeof(payload), outfile);
    CHECK(memcmp(dst, payload, sizeof(payload)) == 0,
          "fwrite_crc did not land the expected bytes in dst");
    CHECK(zz9k_lzh_io_out_count() == sizeof(payload),
          "zz9k_lzh_io_out_count() wrong after an exact-capacity write");
    CHECK(!zz9k_lzh_io_overflowed(),
          "overflow flag set after a write that exactly fit dst_cap");

    /* Now push one more byte past dst_cap. */
    if (zz9k_lzh_setjmp_failed()) {
        CHECK(zz9k_lzh_io_overflowed(),
              "overflow flag not set after writing past dst_cap");
        CHECK(zz9k_lzh_error == 1,
              "zz9k_lzh_error not set after the overflow fatal_error");
        return; /* expected path */
    } else {
        unsigned char extra = 0x55;

        fwrite_crc(&crc, &extra, 1, outfile);
        CHECK(0, "expected fatal_error()/longjmp on dst_cap overflow, got none");
    }
}

/* ------------------------------------------------------------------------ */
/* CRC correctness: the shim's table-driven CRC-16 (make_crctable() +       */
/* calccrc()/UPDATE_CRC()) vs. the independent bitwise reference above.     */
/* ------------------------------------------------------------------------ */
static void
test_crc16_matches_reference(void)
{
    static const char *cases[] = { "", "hello", "The quick brown fox" };
    size_t i;

    make_crctable();

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *s = cases[i];
        size_t len = strlen(s);
        unsigned int got = calccrc(0, (char *)s, (unsigned int)len);
        unsigned int want = reference_crc16((const unsigned char *)s, len);

        if (got != want) {
            printf("FAIL: CRC-16(\"%s\") = 0x%04x, reference = 0x%04x\n",
                   s, got, want);
            g_failures++;
        }
    }
}

/* ------------------------------------------------------------------------ */

int
main(int argc, char *argv[])
{
    /* lha.h (via prototypes.h) declares main() with the host archiver
     * driver's signature -- match it to avoid a conflicting declaration. */
    (void)argc;
    (void)argv;

    test_bitio_basic_and_compsize_gating();
    test_bitio_past_src_len_is_fatal();
    test_crcio_write_and_overflow();
    test_crc16_matches_reference();

    if (g_failures != 0) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }

    printf("OK\n");
    return 0;
}
