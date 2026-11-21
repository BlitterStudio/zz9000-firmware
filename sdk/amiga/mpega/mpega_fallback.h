/*
 * The software mpega.library the ZZ9000 library hands a stream to when the
 * card cannot serve it.
 *
 * The previous mpega.library is kept as LIBS:mpega-software.library. Its
 * internal name is also "mpega.library", so OpenLibrary() would find this
 * library instead. It is loaded here with LoadSeg(), started with
 * InitResident(), and moved from exec's library list to a private list, so
 * that no other program can open it by name. Its functions are called
 * through its vector table.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MPEGA_FALLBACK_H
#define MPEGA_FALLBACK_H

#include <exec/lists.h>
#include <exec/semaphores.h>
#include <exec/types.h>
#include <libraries/mpega.h>

#define MPEGA_FALLBACK_PATH "LIBS:mpega-software.library"

typedef struct MPEGAFallback {
  struct SignalSemaphore lock;  /* guards loading */
  struct List libs;             /* where the library node lives while loaded */
  struct Library *lib;          /* the opened software library, or NULL */
  UWORD open_streams;           /* streams opened through it and not closed */
} MPEGAFallback;

void mpega_fallback_init(MPEGAFallback *fb);

/*
 * The software library, loaded on first use. Only a process loads it (DOS
 * needs one); NULL when the caller is a plain task, when the file is missing,
 * is not a library, or is the ZZ9000 library, or when its start fails.
 */
struct Library *mpega_fallback_acquire(MPEGAFallback *fb);

/* Counts of the streams that run on the software library. */
void mpega_fallback_stream_opened(MPEGAFallback *fb);
void mpega_fallback_stream_closed(MPEGAFallback *fb);

/*
 * Whether the library cannot be dropped now: a stream is open on it, or it is
 * loaded and the caller is not a process (UnLoadSeg needs one).
 */
int mpega_fallback_busy(const MPEGAFallback *fb);

/*
 * Closes the software library and expunges it. The segment is unloaded if
 * the library hands it back. If it does not (it keeps itself because
 * something still holds it), the library stays in memory.
 */
void mpega_fallback_unload(MPEGAFallback *fb);

/* The software library's MPEGA_* functions, called through its vectors. */
MPEGA_STREAM *mpega_fb_open(struct Library *lib, char *filename,
                            MPEGA_CTRL *ctrl);
void mpega_fb_close(struct Library *lib, MPEGA_STREAM *stream);
LONG mpega_fb_decode_frame(struct Library *lib, MPEGA_STREAM *stream,
                           WORD **pcm);
LONG mpega_fb_seek(struct Library *lib, MPEGA_STREAM *stream,
                   ULONG ms_time_position);
LONG mpega_fb_time(struct Library *lib, MPEGA_STREAM *stream,
                   ULONG *ms_time_position);
LONG mpega_fb_scale(struct Library *lib, MPEGA_STREAM *stream,
                    LONG scale_percent);

#endif /* MPEGA_FALLBACK_H */
