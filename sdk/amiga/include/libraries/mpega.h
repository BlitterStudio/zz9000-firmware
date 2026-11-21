/*
 * Public mpega.library compatibility definitions.
 *
 * Copyright (C) 2024-2026 Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */

#ifndef LIBRARIES_MPEGA_H
#define LIBRARIES_MPEGA_H

#define MPEGA_VERSION 2

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif

#ifndef UTILITY_HOOKS_H
#include <utility/hooks.h>
#endif

#define MPEGA_QUALITY_LOW 0
#define MPEGA_QUALITY_MEDIUM 1
#define MPEGA_QUALITY_HIGH 2

#define MPEGA_BSFUNC_OPEN 0
#define MPEGA_BSFUNC_CLOSE 1
#define MPEGA_BSFUNC_READ 2
#define MPEGA_BSFUNC_SEEK 3

typedef struct MPEGA_ACCESS {
  LONG func;
  union {
    struct {
      char *stream_name;
      LONG buffer_size;
      LONG stream_size;
    } open;
    struct {
      void *buffer;
      LONG num_bytes;
    } read;
    struct {
      LONG abs_byte_seek_pos;
    } seek;
  } data;
} MPEGA_ACCESS;

typedef struct MPEGA_OUTPUT {
  WORD freq_div;
  WORD quality;
  LONG freq_max;
} MPEGA_OUTPUT;

typedef struct MPEGA_LAYER {
  WORD force_mono;
  MPEGA_OUTPUT mono;
  MPEGA_OUTPUT stereo;
} MPEGA_LAYER;

typedef struct MPEGA_CTRL {
  struct Hook *bs_access;
  MPEGA_LAYER layer_1_2;
  MPEGA_LAYER layer_3;
  WORD check_mpeg;
  LONG stream_buffer_size;
} MPEGA_CTRL;

#define MPEGA_MODE_STEREO 0
#define MPEGA_MODE_J_STEREO 1
#define MPEGA_MODE_DUAL 2
#define MPEGA_MODE_MONO 3

typedef struct MPEGA_STREAM {
  WORD norm;
  WORD layer;
  WORD mode;
  WORD bitrate;
  LONG frequency;
  WORD channels;
  ULONG ms_duration;
  WORD private_bit;
  WORD copyright;
  WORD original;
  WORD dec_channels;
  WORD dec_quality;
  LONG dec_frequency;
  void *handle;
} MPEGA_STREAM;

#define MPEGA_MAX_CHANNELS 2
#define MPEGA_PCM_SIZE     1152

#define MPEGA_ERR_NONE     0
#define MPEGA_ERR_BASE     0
#define MPEGA_ERR_EOF      (MPEGA_ERR_BASE - 1)
#define MPEGA_ERR_BADFRAME (MPEGA_ERR_BASE - 2)
#define MPEGA_ERR_MEM      (MPEGA_ERR_BASE - 3)
#define MPEGA_ERR_NO_SYNC  (MPEGA_ERR_BASE - 4)
#define MPEGA_ERR_BADVALUE (MPEGA_ERR_BASE - 5)

/*
 * MPGA_decode_to: the next frame written straight into the caller's output
 * buffers. This vector is not part of Stephane Tavenard's mpega.library. It
 * sits after MPEGA_scale (offset -72) with the same struct and format values
 * as in ambmpega.library, whose interface header (MIT, (c) 2026 Brian King)
 * these declarations follow. A program that calls it needs mpega.library
 * 2.127 or newer from the ZZ9000 SDK (check lib_Revision of the opened
 * library): other mpega.library builds have nothing at this slot.
 *
 * mo_Buffer[] are rings of mo_Mask + 1 samples each (mo_Mask is a power of
 * two minus one; ~0 for a buffer that does not wrap), written from mo_Index
 * on. mo_Index is advanced by the samples written. The samples are those
 * MPEGA_decode_frame would give, then:
 *   MPGA_FMT_S16   WORD, mo_Buffer[0] left (or mono), [1] right
 *   MPGA_FMT_S16I  WORD, left and right interleaved in mo_Buffer[0] (AHI's
 *                  AHIST_S16S; mono: AHIST_M16S), mo_Index in sample frames,
 *                  mo_Mask in WORDs
 *   MPGA_FMT_S8    BYTE (Paula), the high byte: [0] left, [1] right
 *   MPGA_FMT_S8D   BYTE, rounded with triangular dither of one 8-bit step
 *   MPGA_FMT_S14   Paula's 14 bits: [0] and [1] each channel's high byte
 *                  (signed, played at volume 64), [2] and [3] its next 6
 *                  bits (0 to 63, played at volume 1 on a channel of its
 *                  own)
 * mo_Peak[] is each channel's largest size in this frame (0 to 32767, of
 * the 16-bit samples). The samples MPEGA_scale clipped are added to
 * mo_Clipped.
 */
#define MPGA_FMT_S16  0
#define MPGA_FMT_S16I 1
#define MPGA_FMT_S8   2
#define MPGA_FMT_S8D  3
#define MPGA_FMT_S14  4

struct MpgaOut {
  ULONG mo_Format;
  APTR mo_Buffer[4];
  ULONG mo_Mask;
  ULONG mo_Index;
  UWORD mo_Peak[2];
  ULONG mo_Clipped;
};

#endif /* LIBRARIES_MPEGA_H */
