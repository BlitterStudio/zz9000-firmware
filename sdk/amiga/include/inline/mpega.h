/*
 * GCC inline callers for mpega.library compatibility.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INLINE_MPEGA_H
#define INLINE_MPEGA_H

#include <exec/libraries.h>
#include <libraries/mpega.h>

#if defined(__GNUC__) && (defined(__amigaos__) || defined(__amiga__) || \
    defined(__AMIGA__))

#define MPEGA_INLINE_CLOBBERS "cc", "memory", "d1", "a0", "a1"

static __inline MPEGA_STREAM *__MPEGAOpenInline(char *stream_name,
                                                MPEGA_CTRL *ctrl)
{
  register MPEGA_STREAM *mpega_d0 __asm("d0");
  register struct Library *mpega_a6 __asm("a6") = MPEGABase;
  register char *mpega_a0 __asm("a0") = stream_name;
  register MPEGA_CTRL *mpega_a1 __asm("a1") = ctrl;
  __asm volatile("jsr -30(a6)"
                 : "=r"(mpega_d0)
                 : "r"(mpega_a6), "r"(mpega_a0), "r"(mpega_a1)
                 : MPEGA_INLINE_CLOBBERS);
  return mpega_d0;
}
#define MPEGA_open(filename, ctrl) __MPEGAOpenInline((filename), (ctrl))

static __inline void __MPEGACloseInline(MPEGA_STREAM *mpds)
{
  register struct Library *mpega_a6 __asm("a6") = MPEGABase;
  register MPEGA_STREAM *mpega_a0 __asm("a0") = mpds;
  __asm volatile("jsr -36(a6)"
                 :
                 : "r"(mpega_a6), "r"(mpega_a0)
                 : MPEGA_INLINE_CLOBBERS);
}
#define MPEGA_close(mpds) __MPEGACloseInline((mpds))

static __inline LONG __MPEGADecodeFrameInline(
    MPEGA_STREAM *mpds, WORD *pcm[MPEGA_MAX_CHANNELS])
{
  register LONG mpega_d0 __asm("d0");
  register struct Library *mpega_a6 __asm("a6") = MPEGABase;
  register MPEGA_STREAM *mpega_a0 __asm("a0") = mpds;
  register WORD **mpega_a1 __asm("a1") = pcm;
  __asm volatile("jsr -42(a6)"
                 : "=r"(mpega_d0)
                 : "r"(mpega_a6), "r"(mpega_a0), "r"(mpega_a1)
                 : MPEGA_INLINE_CLOBBERS);
  return mpega_d0;
}
#define MPEGA_decode_frame(mpds, pcm) \
  __MPEGADecodeFrameInline((mpds), (pcm))

static __inline LONG __MPEGASeekInline(MPEGA_STREAM *mpds,
                                       ULONG ms_time_position)
{
  register ULONG mpega_d0 __asm("d0") = ms_time_position;
  register struct Library *mpega_a6 __asm("a6") = MPEGABase;
  register MPEGA_STREAM *mpega_a0 __asm("a0") = mpds;
  __asm volatile("jsr -48(a6)"
                 : "+r"(mpega_d0)
                 : "r"(mpega_a6), "r"(mpega_a0)
                 : MPEGA_INLINE_CLOBBERS);
  return (LONG)mpega_d0;
}
#define MPEGA_seek(mpds, ms_time_position) \
  __MPEGASeekInline((mpds), (ms_time_position))

static __inline LONG __MPEGATimeInline(MPEGA_STREAM *mpds,
                                       ULONG *ms_time_position)
{
  register LONG mpega_d0 __asm("d0");
  register struct Library *mpega_a6 __asm("a6") = MPEGABase;
  register MPEGA_STREAM *mpega_a0 __asm("a0") = mpds;
  register ULONG *mpega_a1 __asm("a1") = ms_time_position;
  __asm volatile("jsr -54(a6)"
                 : "=r"(mpega_d0)
                 : "r"(mpega_a6), "r"(mpega_a0), "r"(mpega_a1)
                 : MPEGA_INLINE_CLOBBERS);
  return mpega_d0;
}
#define MPEGA_time(mpds, ms_time_position) \
  __MPEGATimeInline((mpds), (ms_time_position))

static __inline LONG __MPEGAFindSyncInline(BYTE *buffer, LONG buffer_size)
{
  register LONG mpega_d0 __asm("d0") = buffer_size;
  register struct Library *mpega_a6 __asm("a6") = MPEGABase;
  register BYTE *mpega_a0 __asm("a0") = buffer;
  __asm volatile("jsr -60(a6)"
                 : "+r"(mpega_d0)
                 : "r"(mpega_a6), "r"(mpega_a0)
                 : MPEGA_INLINE_CLOBBERS);
  return mpega_d0;
}
#define MPEGA_find_sync(buffer, buffer_size) \
  __MPEGAFindSyncInline((buffer), (buffer_size))

static __inline LONG __MPEGAScaleInline(MPEGA_STREAM *mpds,
                                        LONG scale_percent)
{
  register LONG mpega_d0 __asm("d0") = scale_percent;
  register struct Library *mpega_a6 __asm("a6") = MPEGABase;
  register MPEGA_STREAM *mpega_a0 __asm("a0") = mpds;
  __asm volatile("jsr -66(a6)"
                 : "+r"(mpega_d0)
                 : "r"(mpega_a6), "r"(mpega_a0)
                 : MPEGA_INLINE_CLOBBERS);
  return mpega_d0;
}
#define MPEGA_scale(mpds, scale_percent) \
  __MPEGAScaleInline((mpds), (scale_percent))

#else

#define MPEGA_INLINE_CLOBBERS

#endif

#endif /* INLINE_MPEGA_H */
