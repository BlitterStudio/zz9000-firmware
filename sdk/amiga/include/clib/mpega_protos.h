/*
 * C prototypes for mpega.library compatibility calls.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CLIB_MPEGA_PROTOS_H
#define CLIB_MPEGA_PROTOS_H

#include <libraries/mpega.h>

#ifdef __cplusplus
extern "C" {
#endif

MPEGA_STREAM *MPEGA_open(char *stream_name, MPEGA_CTRL *ctrl);
void MPEGA_close(MPEGA_STREAM *mpds);
LONG MPEGA_decode_frame(MPEGA_STREAM *mpds, WORD *pcm[MPEGA_MAX_CHANNELS]);
LONG MPEGA_seek(MPEGA_STREAM *mpds, ULONG ms_time_position);
LONG MPEGA_time(MPEGA_STREAM *mpds, ULONG *ms_time_position);
LONG MPEGA_find_sync(BYTE *buffer, LONG buffer_size);
LONG MPEGA_scale(MPEGA_STREAM *mpds, LONG scale_percent);

#ifdef __cplusplus
}
#endif

#endif /* CLIB_MPEGA_PROTOS_H */
