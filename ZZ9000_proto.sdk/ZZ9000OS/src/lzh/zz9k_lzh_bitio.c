/* ------------------------------------------------------------------------ */
/* ZZ9000 firmware LZH decode offload -- buffer-backed bit I/O              */
/*                                                                          */
/* Adapted from LHa for UNIX's bitio.c (tools/lha-unix/bitio.c in the SDK   */
/* repo; itself jca02266/lha 1.14i-ac20220213). The bit-buffer algorithm    */
/* (fillbuf/getbits/putcode/putbits/init_getbits/init_putbits, and the      */
/* bitbuf/bitcount/subbitbuf state) is kept byte-for-byte identical to      */
/* upstream. The ONLY change is the byte-I/O innards:                      */
/*   - fillbuf() no longer calls getc(infile) -- it pulls the next byte     */
/*     from the membuf SOURCE cursor armed by zz9k_lzh_io_begin().          */
/*   - putcode() no longer calls fwrite(&subbitbuf,1,1,outfile) -- it       */
/*     writes to the membuf DEST cursor (bounds-checked against dst_cap).   */
/* putcode/putbits/init_putbits are encode-only (never invoked by the       */
/* decode path this feature uses) but are kept: huf.c/dhuf.c/shuf.c contain */
/* encode paths too, and reference these symbols, so they must still link. */
/*                                                                          */
/* Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>              */
/* SPDX-License-Identifier: GPL-3.0-or-later                                */
/* ------------------------------------------------------------------------ */
#include "lha.h"
#include "zz9k_lzh.h"

/* Provided by zz9k_lzh_support.c. Internal to the shim -- not part of the
 * public zz9k_lzh.h API surface. */
extern int zz9k_lzh_src_getc(void);
extern int zz9k_lzh_dst_putc(unsigned char c);

/* EXTERN globals lha.h assigns to "bitio.c" (see the ownership map in
 * zz9k_lzh_support.c). infile/outfile are unused sentinels under the
 * membuf design -- see zz9k_lzh.h -- pointed at real storage by
 * zz9k_lzh_io_begin(). */
FILE           *infile, *outfile;
unsigned short  bitbuf;

static unsigned char subbitbuf, bitcount;

void
fillbuf(unsigned char n)  /* Shift bitbuf n bits left, read n bits */
{
    while (n > bitcount) {
        n -= bitcount;
        bitbuf = (bitbuf << bitcount) + (subbitbuf >> (CHAR_BIT - bitcount));
        if (compsize != 0) {
            int c = zz9k_lzh_src_getc();
            compsize--;
            if (c == EOF) {
                fatal_error("cannot read stream");
            }
            subbitbuf = (unsigned char)c;
        }
        else
            subbitbuf = 0;
        bitcount = CHAR_BIT;
    }
    bitcount -= n;
    bitbuf = (bitbuf << n) + (subbitbuf >> (CHAR_BIT - n));
    subbitbuf <<= n;
}

unsigned short
getbits(unsigned char n)
{
    unsigned short  x;

    x = bitbuf >> (2 * CHAR_BIT - n);
    fillbuf(n);
    return x;
}

void
putcode(unsigned char n, unsigned short x)  /* Write leftmost n bits of x */
{
    while (n >= bitcount) {
        n -= bitcount;
        subbitbuf += x >> (USHRT_BIT - bitcount);
        x <<= bitcount;
        if (compsize < origsize) {
            if (zz9k_lzh_dst_putc(subbitbuf) == 0) {
                fatal_error("Write error in bitio.c(putcode)");
            }
            compsize++;
        }
        else
            unpackable = 1;
        subbitbuf = 0;
        bitcount = CHAR_BIT;
    }
    subbitbuf += x >> (USHRT_BIT - bitcount);
    bitcount -= n;
}

void
putbits(unsigned char n, unsigned short x)  /* Write rightmost n bits of x */
{
    x <<= USHRT_BIT - n;
    putcode(n, x);
}

void
init_getbits( /* void */ )
{
    bitbuf = 0;
    subbitbuf = 0;
    bitcount = 0;
    fillbuf(2 * CHAR_BIT);
}

void
init_putbits( /* void */ )
{
    bitcount = CHAR_BIT;
    subbitbuf = 0;
}
