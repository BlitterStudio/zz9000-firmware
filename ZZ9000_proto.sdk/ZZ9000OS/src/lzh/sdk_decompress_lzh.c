/* ------------------------------------------------------------------------ */
/* ZZ9000 firmware LZH decode offload -- sdk_decompress_lzh() backend       */
/*                                                                          */
/* Bridges the SDK codec service (sdk_compression.c's sdk_decompress_buffer */
/* dispatch) to the vendored LHa for UNIX decode core (extract.c's          */
/* decode_lzhuf()) through the buffer-backed I/O shim (zz9k_lzh_bitio.c/    */
/* zz9k_lzh_crcio.c/zz9k_lzh_support.c, Task 5).                            */
/*                                                                          */
/* Also provides the small set of vendored-core entry points that are      */
/* referenced by the decode path but never vendored:                       */
/*   - lha_exit()   -- lha.h #defines exit(n) to lha_exit(n); maketbl.c's   */
/*                     and slide.c's CVE-hardening `error(...); exit(1);`   */
/*                     aborts on malformed Huffman tables/methods route     */
/*                     here. Must longjmp back to the fatal-error recovery  */
/*                     point (never a real process exit).                  */
/*   - error()/warning() -- diagnostic printf-style calls; safe no-ops (the */
/*                     lha_exit()/fatal_error() longjmp that follows them   */
/*                     is what actually aborts the decode attempt).         */
/*   - start_indicator()/finish_indicator() -- progress UI; safe no-ops.    */
/*   - copyfile()  -- only used by the dicbit==0 (-lh0-/stored) path, which */
/*                     this feature never invokes (LH1/LH5/LH6/LH7 all     */
/*                     have a non-zero dicbit -- see extract.c); safe       */
/*                     no-op stub kept only so the link succeeds.          */
/*                                                                          */
/* Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>              */
/* SPDX-License-Identifier: GPL-3.0-or-later                                */
/* ------------------------------------------------------------------------ */
#include <string.h>

#include "lha.h"
#include "zz9k_lzh.h"

/* ---- vendored-core entry points that are never vendored (see above) ---- */

void
warning(char *fmt, ...)
{
    (void)fmt;
}

void
error(char *fmt, ...)
{
    (void)fmt;
}

/* lha.h: `#define exit(n) lha_exit(n)`. Route every CVE-hardening abort in
 * maketbl.c/maketree.c (malformed Huffman tables) and slide.c (unknown
 * method) to the same fatal-error recovery point fatal_error() uses, so a
 * malformed/hostile compressed buffer always comes back as a clean decode
 * failure -- never a real process exit. */
void
lha_exit(int status)
{
    (void)status;
    zz9k_lzh_error = 1;
    longjmp(zz9k_lzh_fatal_jmp, 1);
}

void
start_indicator(char *name, off_t size, char *msg, long def_indicator_threshold)
{
    (void)name;
    (void)size;
    (void)msg;
    (void)def_indicator_threshold;
}

void
finish_indicator(char *name, char *msg)
{
    (void)name;
    (void)msg;
}

/* Stored (-lh0-)/dicbit==0 path only -- never reached for LH1/LH5/LH6/LH7
 * (see extract.c: all four have a non-zero LZHUFFx_DICBIT). */
off_t
copyfile(FILE *f1, FILE *f2, off_t size, int text_flg, unsigned int *crcp)
{
    (void)f1;
    (void)f2;
    (void)size;
    (void)text_flg;
    (void)crcp;
    return 0;
}

/* ---- Task 6 backend ---- */

uint16_t
sdk_decompress_lzh(uint32_t algorithm,
                   const uint8_t *src, uint32_t src_length,
                   uint8_t *dst, uint32_t dst_capacity,
                   struct SDKDecompressResult *result)
{
    int method;
    off_t read_size = 0;
    unsigned int crc;

    if (!src || !dst || !result || src_length == 0U || dst_capacity == 0U)
        return SDK_STATUS_BAD_REQUEST;

    switch (algorithm) {
    case SDK_COMPRESSION_LH1:
        method = 1;     /* LZHUFF1_METHOD_NUM */
        break;
    case SDK_COMPRESSION_LH5:
        method = 5;     /* LZHUFF5_METHOD_NUM */
        break;
    case SDK_COMPRESSION_LH6:
        method = 6;     /* LZHUFF6_METHOD_NUM */
        break;
    case SDK_COMPRESSION_LH7:
        method = 7;     /* LZHUFF7_METHOD_NUM */
        break;
    default:
        return SDK_STATUS_UNSUPPORTED;
    }

    memset(result, 0, sizeof(*result));
    result->algorithm = algorithm;

    /* fatal_error()/lha_exit() longjmp target. MUST be invoked directly here
     * (it is a macro expanding to setjmp()) -- see zz9k_lzh.h. */
    if (zz9k_lzh_setjmp_failed()) {
        return SDK_STATUS_BAD_REQUEST;
    }

    zz9k_lzh_io_begin(src, src_length, dst, dst_capacity);

    /* Decode-mode globals, mirroring the SDK wrapper
     * (tools/lha-unix/zz9k_lha_unix.c:50-58). */
    zz9k_lzh_error = 0;
    make_crctable();
    quiet = 1;
    quiet_mode = 2;
    verify_mode = 0;
    text_mode = 0;
    output_to_stdout = 0;
    dump_lzss = 0;
    extract_broken_archive = 0;

    /* infile/outfile were just pointed at the membuf sentinels by
     * zz9k_lzh_io_begin(); decode_lzhuf() (and the decode() it calls) thread
     * them through as interface.infile/outfile and back into the globals --
     * the shim's fread_crc()/fwrite_crc() read/write the membuf cursors, not
     * the FILE* itself (see zz9k_lzh.h). */
    crc = (unsigned int)decode_lzhuf(infile, outfile,
                                     (off_t)dst_capacity, (off_t)src_length,
                                     "zz9k", method, &read_size);

    if (zz9k_lzh_io_overflowed())
        return SDK_STATUS_NO_MEMORY;

    result->bytes_written = zz9k_lzh_io_out_count();
    result->bytes_consumed = (uint32_t)read_size;
    result->checksum = (uint16_t)crc;
    result->flags = SDK_DECOMPRESS_RESULT_CHECKSUM_VALID;

    if (result->bytes_written == dst_capacity) {
        result->flags |= SDK_DECOMPRESS_RESULT_STREAM_END;
    } else {
        return SDK_STATUS_BAD_REQUEST;     /* short decode */
    }

    return SDK_STATUS_OK;
}
