/* ------------------------------------------------------------------------ */
/* ZZ9000 firmware LZH decode offload -- buffer-backed I/O shim public API  */
/*                                                                          */
/* Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>              */
/*                                                                          */
/* The vendored LHa-for-UNIX decode core (dhuf.c/extract.c/huf.c/larc.c/    */
/* maketbl.c/maketree.c/shuf.c/slide.c, vendored verbatim from the SDK's    */
/* tools/lha-unix/) is written against stdio FILE* (bitio.c's fillbuf()     */
/* calls getc(infile); crcio.c's fwrite_crc()/fread_crc() call fwrite()/    */
/* fread() against outfile/infile). The firmware has no stdio FILE, so      */
/* zz9k_lzh_bitio.c/zz9k_lzh_crcio.c replace only the byte-I/O innards of   */
/* the original bitio.c/crcio.c with reads/writes against an in-memory     */
/* source buffer and an in-memory, capacity-bounded destination buffer.    */
/* Every other line of the original algorithm (bit-buffer state machine,   */
/* CRC-16 table + update) is kept byte-for-byte identical to upstream.     */
/*                                                                          */
/* Usage (mirrors the Task-6 sdk_decompress_lzh backend):                  */
/*                                                                          */
/*   if (zz9k_lzh_setjmp_failed()) {                                       */
/*       ... a fatal_error() longjmp landed here: corrupt stream, output   */
/*       overflow, or OOM. zz9k_lzh_error is 1 and zz9k_lzh_fatal_message()*/
/*       has the reason; dst may be partially written. ...                */
/*   } else {                                                              */
/*       zz9k_lzh_io_begin(src, src_len, dst, dst_cap);                    */
/*       make_crctable();                                                 */
/*       ... call decode_lzhuf() / decode() ...                            */
/*       if (zz9k_lzh_io_overflowed()) { ... dst_cap was too small ... }   */
/*       bytes_written = zz9k_lzh_io_out_count();                          */
/*   }                                                                     */
/*                                                                          */
/* SPDX-License-Identifier: GPL-3.0-or-later                                */
/* ------------------------------------------------------------------------ */
#ifndef ZZ9K_LZH_H
#define ZZ9K_LZH_H

#include <setjmp.h>
#include <stdint.h>

/* struct SDKDecompressResult, for the sdk_decompress_lzh() prototype below.
 * Resolved via the src/ include path (see the Makefile's global -I$(SRC_DIR),
 * and the host test's explicit -I.../ZZ9000OS/src). */
#include "sdk_compression.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Arm the membuf cursors for one decode. Must be called before invoking the
 * decode core (decode_lzhuf()/decode()) for each buffer. Also points the
 * core's `infile`/`outfile` FILE* globals at non-NULL sentinel addresses:
 * verbatim core code only ever null-checks or passes these straight through
 * to the shimmed fread_crc()/fwrite_crc() (which read/write the membuf
 * cursors below instead of touching the FILE* itself), so the sentinels are
 * never dereferenced.
 */
void zz9k_lzh_io_begin(const uint8_t *src, uint32_t src_len,
                       uint8_t *dst, uint32_t dst_cap);

/* Number of bytes written to dst so far (== the decoded output length once
 * decoding completes without overflow). */
uint32_t zz9k_lzh_io_out_count(void);

/* Nonzero if a write attempted to go past dst_cap. When this happens, the
 * write that overflowed also invokes fatal_error() (see below), so this
 * flag is meant to be inspected after a fatal_error() recovery. */
int zz9k_lzh_io_overflowed(void);

/*
 * fatal_error() recovery (prototypes.h: `void fatal_error(char *fmt, ...);`,
 * a variadic, printf-style diagnostic the vendored core calls on any
 * unrecoverable condition -- corrupt stream, output-buffer overrun, OOM).
 * The firmware cannot unwind stdio state, so fatal_error() longjmp()s back
 * to a jmp_buf armed by this helper.
 *
 * Use it exactly like setjmp(): it returns 0 the first time (the recovery
 * point is now armed), and nonzero if control arrived via a fatal_error()
 * longjmp (in which case the decode attempt was aborted mid-stream and
 * dst may be partially written).
 *
 *   if (zz9k_lzh_setjmp_failed()) {
 *       return SDK_STATUS_BAD_REQUEST;
 *   }
 *   zz9k_lzh_io_begin(...);
 *   ... decode ...
 *
 * This MUST be a macro, not a function: C99 7.13.2.1 requires setjmp() to
 * be invoked directly within the function that establishes the recovery
 * point (as the entire controlling expression of a selection/iteration
 * statement, optionally compared against an integer constant) -- if it
 * were wrapped in an ordinary helper function, that function's own stack
 * frame would already be gone by the time a later fatal_error() longjmp
 * tried to resume it, which is undefined behavior. The macro instead
 * inlines the setjmp() call at the actual call site (e.g. inside
 * sdk_decompress_lzh() itself), which is exactly the "one true form"
 * `if (setjmp(x) != 0)` the standard blesses.
 */
extern jmp_buf zz9k_lzh_fatal_jmp;
#define zz9k_lzh_setjmp_failed() (setjmp(zz9k_lzh_fatal_jmp) != 0)

/* Set to 1 by fatal_error() when a fatal condition was hit. The backend
 * resets it to 0 before each decode attempt (mirroring zz9k_lha_unix.c's
 * decode-mode globals reset). */
extern int zz9k_lzh_error;

/* The message recorded by the most recent fatal_error() call, or NULL if
 * none has fired yet. Diagnostic only -- not required by the decode path. */
const char *zz9k_lzh_fatal_message(void);

/*
 * Task 6 backend: decode an in-memory LZH/LHA-format compressed buffer
 * (algorithm selects the encoding method -- SDK_COMPRESSION_LH1/LH5/LH6/
 * LH7) into an in-memory output buffer, using the vendored decode core
 * through this shim. Declared here (Task 5); implemented in
 * sdk_decompress_lzh.c (Task 6).
 */
uint16_t sdk_decompress_lzh(uint32_t algorithm,
                            const uint8_t *src, uint32_t src_length,
                            uint8_t *dst, uint32_t dst_capacity,
                            struct SDKDecompressResult *result);

/* Cold-restart reclaim: free the decoder window a core-1 reset abandoned.
 * dtext is the ONLY heap allocation on the LZH decode path (see the
 * longjmp-recovery branch in sdk_decompress_lzh); runs on core 0 after
 * core 1 is halted, so touching the decoder globals is safe. Mirrors the
 * setjmp-recovery free+NULL pairing. */
void zz9k_lzh_reclaim(void);

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_LZH_H */
