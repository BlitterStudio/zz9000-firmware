/* ------------------------------------------------------------------------ */
/* ZZ9000 firmware LZH decode offload -- buffer-backed CRC I/O              */
/*                                                                          */
/* Adapted from LHa for UNIX's crcio.c (tools/lha-unix/crcio.c in the SDK   */
/* repo; itself jca02266/lha 1.14i-ac20220213). make_crctable(), the CRC-16 */
/* table build, and calccrc()'s CRC-16 update (poly 0xA001, via the         */
/* UPDATE_CRC()/CRCPOLY macros in lha_macro.h) are kept byte-for-byte        */
/* identical to upstream. The ONLY change is the byte-I/O innards of        */
/* fread_crc()/fwrite_crc(): instead of fread()/fwrite() against            */
/* infile/outfile, they read/write the membuf SOURCE/DEST cursors armed    */
/* by zz9k_lzh_io_begin(). Binary path only: text_mode is always 0 for      */
/* firmware use (decode of real archives is always binary), so the         */
/* fwrite_txt/fread_txt/putc_euc text-mode and EUC helpers are host-        */
/* archiver-only and are not vendored here (see config.h's comment) --      */
/* nothing in the decode core references them.                             */
/*                                                                          */
/* Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>              */
/* SPDX-License-Identifier: GPL-3.0-or-later                                */
/* ------------------------------------------------------------------------ */
#include <stdint.h>

#include "lha.h"
#include "zz9k_lzh.h"

/* Provided by zz9k_lzh_support.c. Internal to the shim -- not part of the
 * public zz9k_lzh.h API surface. */
extern uint32_t zz9k_lzh_src_read(void *p, uint32_t n);
extern uint32_t zz9k_lzh_dst_write(const void *p, uint32_t n);

/* EXTERN global lha.h assigns to "crcio.c" (see the ownership map in
 * zz9k_lzh_support.c). */
unsigned int crctable[UCHAR_MAX + 1];

/* ------------------------------------------------------------------------ */
void
make_crctable( /* void */ )
{
    unsigned int    i, j, r;

    for (i = 0; i <= UCHAR_MAX; i++) {
        r = i;
        for (j = 0; j < CHAR_BIT; j++)
            if (r & 1)
                r = (r >> 1) ^ CRCPOLY;
            else
                r >>= 1;
        crctable[i] = r;
    }
}

/* ------------------------------------------------------------------------ */
unsigned int
calccrc(unsigned int crc, char *p, unsigned int n)
{
    while (n-- > 0)
        crc = UPDATE_CRC(crc, *p++);
    return crc;
}

/* ------------------------------------------------------------------------ */
int
fread_crc(unsigned int *crcp, void *p, int n, FILE *fp)
{
    unsigned int got;

    (void)fp;   /* unused sentinel under the membuf design; see zz9k_lzh.h */

    got = zz9k_lzh_src_read(p, (uint32_t)n);
    n = (int)got;

    *crcp = calccrc(*crcp, p, n);
    return n;
}

/* ------------------------------------------------------------------------ */
void
fwrite_crc(unsigned int *crcp, void *p, int n, FILE *fp)
{
    *crcp = calccrc(*crcp, p, n);

    if (verify_mode)
        return;

    if (fp) {
        uint32_t written = zz9k_lzh_dst_write(p, (uint32_t)n);

        if (written < (uint32_t)n) {
            /* zz9k_lzh_dst_write() already flagged the overflow (see
             * zz9k_lzh_io_overflowed()); fatal_error() longjmps out. */
            fatal_error("File write error");
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Every decode_start_*() in the vendored core (huf.c/dhuf.c/shuf.c/larc.c)
 * calls init_code_cache() unconditionally, regardless of text_mode. Upstream
 * it just resets the static EUC lookahead caches used by fread_txt/putc_euc
 * (host-archiver, text-mode-only helpers). Those helpers -- and EUC/SJIS
 * filename conversion generally -- are not vendored here (config.h; binary
 * decode only), so there is nothing to reset: a no-op keeps the call sites
 * linking without reintroducing that dead code. */
void
init_code_cache( /* void */ )
{
}
