/* ------------------------------------------------------------------------ */
/* ZZ9000 firmware LZH decode offload -- globals, allocator shims,          */
/* fatal-error recovery, and the membuf I/O plumbing.                      */
/*                                                                          */
/* No vendored translation unit in src/lzh/ defines LHA_MAIN_SRC (that      */
/* belongs to the host archiver driver's lharc.c, which is NOT vendored --  */
/* see lha.h's `#ifdef LHA_MAIN_SRC / #define EXTERN` machinery), so every   */
/* EXTERN global the decode core references is declared extern-only there  */
/* and left UNDEFINED at link. This file is the definition site for all of  */
/* them, except:                                                           */
/*   - infile/outfile/bitbuf  -> zz9k_lzh_bitio.c ("bitio.c" owns them per   */
/*                                lha.h's own section comment)              */
/*   - crctable[]             -> zz9k_lzh_crcio.c (ditto, "crcio.c")        */
/*   - the Huffman arrays (left/right/c_len/pt_len/c_freq/c_table/c_code/   */
/*     p_freq/pt_table/pt_code/t_freq) -> huf.c, guarded by ZZ9K_LHA_HUF_SRC*/
/*     in lha.h; defining them here too would be a duplicate definition.    */
/*                                                                          */
/* "crc" is NOT one of these: a grep across every .c file under src/lzh (and */
/* lha.h's EXTERN list) shows it is always a local variable or a pointer    */
/* parameter threaded through fread_crc()/fwrite_crc() (see slide.c's       */
/* encode()/decode()), never an EXTERN global. Defining a same-named global */
/* here would be spurious dead storage, so it is intentionally omitted.     */
/*                                                                          */
/* Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>              */
/* SPDX-License-Identifier: GPL-3.0-or-later                                */
/* ------------------------------------------------------------------------ */
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lha.h"
#include "zz9k_lzh.h"

#define ZZ9K_LZH_CACHELINE_BYTES 32u

/* ---- EXTERN globals from lha.h (see the ownership map above) ---- */

/* Mode flags the decode core and the Task-6 backend touch (mirrors
 * zz9k_lha_unix.c's decode-mode globals reset). */
boolean quiet;
boolean text_mode;
boolean verify_mode;
boolean output_to_stdout;
boolean dump_lzss;
boolean extract_broken_archive;
int     quiet_mode;

/* slide.c */
int            unpackable;
off_t          origsize, compsize;
unsigned short dicbit;
unsigned short maxmatch;
off_t          decode_count;
unsigned long  loc;
unsigned char *text;    /* encoding buffer (encode path only) */
unsigned char *dtext;   /* decoding buffer */

/* dhuf.c */
unsigned int n_max;

/* ---- membuf state ---- */

static const uint8_t *zz9k_lzh_src_ptr;
static uint32_t       zz9k_lzh_src_len;
static uint32_t       zz9k_lzh_src_pos;

static uint8_t  *zz9k_lzh_dst_ptr;
static uint32_t  zz9k_lzh_dst_cap;
static uint32_t  zz9k_lzh_dst_pos;

static int zz9k_lzh_overflow;

/* infile/outfile (defined in zz9k_lzh_bitio.c) point at these. Never
 * dereferenced -- see the "unused sentinels" note in zz9k_lzh.h. */
static unsigned char zz9k_lzh_infile_sentinel;
static unsigned char zz9k_lzh_outfile_sentinel;

int zz9k_lzh_error;

/* Non-static: zz9k_lzh_setjmp_failed() in zz9k_lzh.h is a MACRO that
 * expands to `setjmp(zz9k_lzh_fatal_jmp)` directly at the caller's call
 * site (see that header for why it must be a macro, not a function), so
 * this needs external linkage. */
jmp_buf zz9k_lzh_fatal_jmp;
static char zz9k_lzh_fatal_msg[128];

/*
 * Reset-time reclaim state for slide.c's dtext window.
 *
 * Core 1 explicitly cleans this single cache line after publishing or clearing
 * the pointer. Core 0 invalidates the same line before reclaiming after a cold
 * restart, so it never relies on a dirty-only core-1 copy of the raw dtext
 * global. The padded line also prevents range cache maintenance from touching
 * unrelated globals.
 */
struct zz9k_lzh_dtext_reclaim_state {
    unsigned char *ptr;
    unsigned char pad[ZZ9K_LZH_CACHELINE_BYTES - sizeof(unsigned char *)];
};

static struct zz9k_lzh_dtext_reclaim_state zz9k_lzh_dtext_reclaim
    __attribute__((aligned(ZZ9K_LZH_CACHELINE_BYTES)));

typedef char zz9k_lzh_dtext_reclaim_is_one_line[
    (sizeof(zz9k_lzh_dtext_reclaim) == ZZ9K_LZH_CACHELINE_BYTES) ? 1 : -1];

void *
zz9k_lzh_dtext_reclaim_base(void)
{
    return &zz9k_lzh_dtext_reclaim;
}

unsigned
zz9k_lzh_dtext_reclaim_bytes(void)
{
    return (unsigned)sizeof(zz9k_lzh_dtext_reclaim);
}

void
zz9k_lzh_track_dtext(unsigned char *ptr)
{
    dtext = ptr;
    zz9k_lzh_dtext_reclaim.ptr = ptr;
    zz9k_lzh_flush_dtext_reclaim();
}

void
zz9k_lzh_disarm_dtext(void)
{
    if (dtext == NULL && zz9k_lzh_dtext_reclaim.ptr == NULL)
        return;
    dtext = NULL;
    zz9k_lzh_dtext_reclaim.ptr = NULL;
    zz9k_lzh_flush_dtext_reclaim();
}

void
zz9k_lzh_free_dtext(void)
{
    unsigned char *ptr = dtext;

    if (ptr == NULL) {
        if (zz9k_lzh_dtext_reclaim.ptr != NULL) {
            zz9k_lzh_dtext_reclaim.ptr = NULL;
            zz9k_lzh_flush_dtext_reclaim();
        }
        return;
    }

    dtext = NULL;
    zz9k_lzh_dtext_reclaim.ptr = NULL;
    zz9k_lzh_flush_dtext_reclaim();  /* DRAM shows non-live before free */
    free(ptr);
}

void
zz9k_lzh_reclaim(void)
{
    unsigned char *ptr;

    zz9k_lzh_invalidate_dtext_reclaim();
    ptr = zz9k_lzh_dtext_reclaim.ptr;
    if (ptr != NULL) {
        zz9k_lzh_dtext_reclaim.ptr = NULL;
        zz9k_lzh_flush_dtext_reclaim();
        free(ptr);
    }
    dtext = NULL;
}

/* ---- public membuf API (zz9k_lzh.h) ---- */

void
zz9k_lzh_io_begin(const uint8_t *src, uint32_t src_len,
                  uint8_t *dst, uint32_t dst_cap)
{
    zz9k_lzh_src_ptr = src;
    zz9k_lzh_src_len = src_len;
    zz9k_lzh_src_pos = 0;

    zz9k_lzh_dst_ptr = dst;
    zz9k_lzh_dst_cap = dst_cap;
    zz9k_lzh_dst_pos = 0;

    zz9k_lzh_overflow = 0;

    infile  = (FILE *)&zz9k_lzh_infile_sentinel;
    outfile = (FILE *)&zz9k_lzh_outfile_sentinel;
}

uint32_t
zz9k_lzh_io_out_count(void)
{
    return zz9k_lzh_dst_pos;
}

int
zz9k_lzh_io_overflowed(void)
{
    return zz9k_lzh_overflow;
}

const char *
zz9k_lzh_fatal_message(void)
{
    return zz9k_lzh_error ? zz9k_lzh_fatal_msg : (const char *)0;
}

/* ---- internal helpers used by zz9k_lzh_bitio.c / zz9k_lzh_crcio.c ---- */

int
zz9k_lzh_src_getc(void)
{
    if (zz9k_lzh_src_pos >= zz9k_lzh_src_len)
        return EOF;
    return zz9k_lzh_src_ptr[zz9k_lzh_src_pos++];
}

uint32_t
zz9k_lzh_src_read(void *p, uint32_t n)
{
    uint32_t avail = zz9k_lzh_src_len - zz9k_lzh_src_pos;
    uint32_t to_copy = (n <= avail) ? n : avail;

    if (to_copy > 0) {
        memcpy(p, zz9k_lzh_src_ptr + zz9k_lzh_src_pos, to_copy);
        zz9k_lzh_src_pos += to_copy;
    }
    return to_copy;
}

uint32_t
zz9k_lzh_dst_write(const void *p, uint32_t n)
{
    uint32_t avail = zz9k_lzh_dst_cap - zz9k_lzh_dst_pos;
    uint32_t to_copy = (n <= avail) ? n : avail;

    if (to_copy > 0) {
        /* dst == NULL is decode-and-discard (batch TEST mode): fwrite_crc()
         * has already folded these bytes into the CRC-16, so only count and
         * bounds-check the produced output without storing it. */
        if (zz9k_lzh_dst_ptr != NULL)
            memcpy(zz9k_lzh_dst_ptr + zz9k_lzh_dst_pos, p, to_copy);
        zz9k_lzh_dst_pos += to_copy;
    }
    if (to_copy < n)
        zz9k_lzh_overflow = 1;
    return to_copy;
}

int
zz9k_lzh_dst_putc(unsigned char c)
{
    return zz9k_lzh_dst_write(&c, 1) == 1 ? 1 : 0;
}

/* ---- fatal_error() + allocator shims (prototypes.h) ---- */

void
fatal_error(char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(zz9k_lzh_fatal_msg, sizeof(zz9k_lzh_fatal_msg), fmt, ap);
    va_end(ap);

    zz9k_lzh_error = 1;
    longjmp(zz9k_lzh_fatal_jmp, 1);
}

void *
xmalloc(size_t size)
{
    void *p = malloc(size ? size : 1);

    if (!p)
        fatal_error("xmalloc: out of memory (%lu bytes)", (unsigned long)size);
    return p;
}

void *
xrealloc(void *old, size_t size)
{
    void *p = realloc(old, size ? size : 1);

    if (!p)
        fatal_error("xrealloc: out of memory (%lu bytes)", (unsigned long)size);
    return p;
}

void *
xcalloc(size_t nmemb, size_t size)
{
    void *p = calloc(nmemb ? nmemb : 1, size ? size : 1);

    if (!p)
        fatal_error("xcalloc: out of memory (%lu * %lu bytes)",
                    (unsigned long)nmemb, (unsigned long)size);
    return p;
}
