/*
 * mpega.library compatibility resident.
 *
 * This is the ZZ9000-accelerated drop-in for the exact-name mpega.library.
 * It implements the full public MPEGA API: Open, Close, DecodeFrame, Seek,
 * Time, FindSync and Scale, and MPGA_decode_to after them. A stream the card
 * cannot serve goes to the software library LIBS:mpega-software.library
 * (mpega_fallback.c). The PCM handling is in mpega_convert.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDI_compiler.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/resident.h>
#include <libraries/mpega.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "zz9k/audio.h"
#include "zz9k/library_vectors.h"
#include "zz9k/shared.h"
#include <proto/zz9k.h>

#include "mpega_convert.h"
#include "mpega_fallback.h"

#define MPEGA_LIBRARY_NAME "mpega.library"

#define MPEGA_LIBRARY_VERSION 2
#define MPEGA_LIBRARY_REVISION 127
#define MPEGA_LIBRARY_DATE "21.11.2026"

/* Derive the $VER: string from the version macros rather than repeating the
 * numbers. They are the same identity read two ways -- lib_Version and
 * lib_Revision are what OpenLibrary() version checks see, the $VER: string
 * is what a user sees -- and hardcoding the string let them drift during the
 * v2.8 identity freeze: the string moved to 2.124 while lib_Revision stayed
 * at 123. Deriving it makes that impossible. */
#define MPEGA_STRINGIFY_(x) #x
#define MPEGA_STRINGIFY(x) MPEGA_STRINGIFY_(x)
#define MPEGA_LIBRARY_ID_STRING \
  "$VER: " MPEGA_LIBRARY_NAME " " MPEGA_STRINGIFY(MPEGA_LIBRARY_VERSION) \
  "." MPEGA_STRINGIFY(MPEGA_LIBRARY_REVISION) \
  " (" MPEGA_LIBRARY_DATE ") ZZ9000 SDK"

#define MPEGA_MP3_RING_CAPACITY (128UL * 1024UL)
#define MPEGA_PCM_RING_CAPACITY (256UL * 1024UL)
#define MPEGA_PCM_ACK_BATCH_BYTES (192UL * 1024UL)
#define MPEGA_STREAM_CHUNK_BYTES (64UL * 1024UL)
/*
 * Compact host-visible sizes for Zorro 2, where host-mappable allocations
 * come from the firmware's 64 KB board-window heap (shared with the MHI
 * driver's 4 KB staging buffer). Only the PCM ring and the feed staging
 * buffer are host-visible; the decode input ring stays card-only at full
 * size. Smaller rings just mean more feed/ack round trips -- the forced
 * ack at the top of the feed loop keeps a small ring draining (the
 * MPEGA_PCM_ACK_BATCH_BYTES threshold is simply never reached).
 */
#define MPEGA_PCM_RING_COMPACT_CAPACITY (32UL * 1024UL)
#define MPEGA_STREAM_CHUNK_COMPACT_BYTES (16UL * 1024UL)
#define MPEGA_DEFAULT_STREAM_BUFFER_BYTES 16384L
#define MPEGA_MAX_FEED_ATTEMPTS 128U
#define MPEGA_TIME_UPDATE_CHUNK_SAMPLES 64U
#define MPEGA_TIME_UPDATE_CHUNK_MS 64000U
#define MPEGA_XING_TOC_BYTES 100U

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *MPEGABase;
struct Library *ZZ9KBase;
struct Library *UtilityBase;

struct MPEGACompatBase {
  struct Library library;
  BPTR segment;
  UWORD zz9k_open_count;
  /* The audio service's flags, read each time a stream acquires zz9k.library. */
  uint32_t audio_flags;
  MPEGAFallback fallback;
};

typedef struct MPEGACompatStream {
  MPEGA_STREAM stream;
  MPEGA_CTRL ctrl;
  struct MPEGACompatBase *base;
  ZZ9KSharedBuffer mp3_ring;
  ZZ9KSharedBuffer pcm_ring;
  ZZ9KSharedBuffer staging;
  ZZ9KAudioStreamResult result;
  /* The open stream's id. Kept apart from `result`: the host layer zeroes a
   * result struct whenever an operation fails, and the session must still
   * be closed afterwards. */
  uint32_t session;
  uint8_t *input_buffer;
  APTR hook_handle;
  BPTR file;
  LONG input_chunk_bytes;
  LONG stream_size;
  LONG stream_start_byte;
  LONG scale_percent;
  uint32_t xing_stream_bytes;
  ULONG time_ms;
  uint32_t time_ms_accum;
  uint32_t pcm_seen_bytes;
  uint32_t pcm_read_offset;
  uint32_t pcm_pending_ack_bytes;
  uint8_t xing_toc[MPEGA_XING_TOC_BYTES];
  uint8_t xing_has_toc;
  uint8_t session_open;
  uint8_t zz9k_acquired;
  uint8_t mp3_allocated;
  uint8_t pcm_allocated;
  uint8_t staging_allocated;
  MPEGAFeedState feed;
  uint8_t hook_open;
  /* The software library's stream when it serves this one. The fields of
   * `stream` then mirror its public fields. */
  MPEGA_STREAM *fb_stream;
  struct Library *fb_lib;
  /* What the card reduced for this stream (set by mpega_stream_begin). */
  uint32_t reduce_div;
  uint8_t reduce_active;
  uint8_t reduce_mono;
  uint8_t header_probed;
  uint32_t dither;
  /* One frame of PCM ring bytes, and MPGA_decode_to's samples. */
  uint32_t raw[MPEGA_PCM_SIZE];
  int16_t chan[MPEGA_MAX_CHANNELS][MPEGA_PCM_SIZE];
} MPEGACompatStream;

typedef struct MPEGAParsedHeader {
  WORD norm;
  WORD layer;
  WORD mode;
  WORD bitrate;
  LONG frequency;
  WORD channels;
  WORD samples_per_frame;
  WORD private_bit;
  WORD copyright;
  WORD original;
} MPEGAParsedHeader;

static struct MPEGACompatBase *mpega_lib_open(
    REG(a6, struct MPEGACompatBase *base));
static BPTR mpega_lib_close(REG(a6, struct MPEGACompatBase *base));
static BPTR mpega_lib_expunge(REG(a6, struct MPEGACompatBase *base));
static ULONG mpega_lib_null(void);
static struct MPEGACompatBase *mpega_lib_init(REG(a0, BPTR segment));
static void mpega_default_ctrl(MPEGA_CTRL *ctrl);
static LONG mpega_normalize_stream_buffer_size(LONG stream_buffer_size);
static ULONG mpega_call_bs_access(MPEGACompatStream *state, APTR handle,
                                  MPEGA_ACCESS *access);
static int mpega_stream_open_source(MPEGACompatStream *state, char *filename);
static void mpega_stream_close_source(MPEGACompatStream *state);
static LONG mpega_stream_read_source(MPEGACompatStream *state, void *buffer,
                                     LONG length);
static int mpega_stream_seek_source(MPEGACompatStream *state,
                                    LONG abs_byte_pos);
static uint32_t mpega_read_be32(const UBYTE *bytes);
static int mpega_parse_mpeg_header(const UBYTE *bytes,
                                   MPEGAParsedHeader *parsed);
static void mpega_apply_mpeg_header(MPEGACompatStream *state,
                                    const MPEGAParsedHeader *parsed,
                                    LONG header_offset);
static void mpega_apply_xing_info(MPEGACompatStream *state,
                                  const MPEGAParsedHeader *parsed,
                                  const UBYTE *bytes, LONG length,
                                  LONG header_offset);
static int mpega_stream_probe_metadata(MPEGACompatStream *state);
static int mpega_open_probe_metadata(MPEGACompatStream *state);
static void mpega_stream_update_estimated_duration(MPEGACompatStream *state);
static int mpega_output_force_mono(const MPEGACompatStream *state);
static uint32_t mpega_output_channels(const MPEGACompatStream *state,
                                      uint32_t source_channels);
static const MPEGA_OUTPUT *mpega_selected_output(
    const MPEGACompatStream *state, uint32_t output_channels);
static uint32_t mpega_output_freq_div(const MPEGACompatStream *state,
                                      uint32_t output_channels,
                                      uint32_t source_rate);
static WORD mpega_output_quality(const MPEGACompatStream *state,
                                 uint32_t output_channels);
static void mpega_stream_set_defaults(MPEGACompatStream *state);
static void mpega_stream_end_session(MPEGACompatStream *state);
static void mpega_stream_free(MPEGACompatStream *state);
static LONG mpega_xing_toc_seek_byte(const MPEGACompatStream *state,
                                     ULONG ms_time_position);
static LONG mpega_stream_seek_byte_for_ms(MPEGACompatStream *state,
                                          ULONG ms_time_position);
static int mpega_stream_restart_at_byte(MPEGACompatStream *state,
                                        LONG abs_byte_pos,
                                        ULONG ms_time_position);
static int mpega_zz9k_acquire(struct MPEGACompatBase *base);
static void mpega_zz9k_release(struct MPEGACompatBase *base);
static int mpega_stream_begin(MPEGACompatStream *state);
static int mpega_stream_feed_until_pcm(MPEGACompatStream *state);
static uint32_t mpega_source_channel_count(const MPEGACompatStream *state);
static uint32_t mpega_output_channel_count(const MPEGACompatStream *state);
static uint32_t mpega_pcm_divisor(const MPEGACompatStream *state,
                                  uint32_t output_channels,
                                  uint32_t source_rate);
static LONG mpega_take_frame(MPEGACompatStream *state, int16_t *dst0,
                             int16_t *dst1, uint32_t *clipped);
static int mpega_ack_pcm(MPEGACompatStream *state, int force);
static void mpega_update_time_ms(MPEGACompatStream *state, uint32_t samples);
static int mpega_is_sync_header(const UBYTE *bytes);
static int mpega_shared_copy_to_bytes(ZZ9KSharedBuffer *buffer,
                                      uint32_t offset, const uint8_t *src,
                                      uint32_t length);

static MPEGA_STREAM *mpega_call_open(REG(a6, struct MPEGACompatBase *base),
                                    REG(a0, char *filename),
                                    REG(a1, MPEGA_CTRL *ctrl));
static void mpega_call_close(REG(a6, struct MPEGACompatBase *base),
                             REG(a0, MPEGA_STREAM *mpds));
static LONG mpega_call_decode_frame(REG(a6, struct MPEGACompatBase *base),
                                    REG(a0, MPEGA_STREAM *mpds),
                                    REG(a1, WORD **pcm));
static LONG mpega_call_seek(REG(a6, struct MPEGACompatBase *base),
                            REG(a0, MPEGA_STREAM *mpds),
                            REG(d0, ULONG ms_time_position));
static LONG mpega_call_time(REG(a6, struct MPEGACompatBase *base),
                            REG(a0, MPEGA_STREAM *mpds),
                            REG(a1, ULONG *ms_time_position));
static LONG mpega_call_find_sync(REG(a6, struct MPEGACompatBase *base),
                                 REG(a0, BYTE *buffer),
                                 REG(d0, LONG buffer_size));
static LONG mpega_call_scale(REG(a6, struct MPEGACompatBase *base),
                             REG(a0, MPEGA_STREAM *mpds),
                             REG(d0, LONG scale_percent));
static LONG mpega_call_decode_to(REG(a6, struct MPEGACompatBase *base),
                                 REG(a0, MPEGA_STREAM *mpds),
                                 REG(a1, struct MpgaOut *out));

static const APTR mpega_lib_vectors[] = {
  (APTR)mpega_lib_open,
  (APTR)mpega_lib_close,
  (APTR)mpega_lib_expunge,
  (APTR)mpega_lib_null,
  (APTR)mpega_call_open,
  (APTR)mpega_call_close,
  (APTR)mpega_call_decode_frame,
  (APTR)mpega_call_seek,
  (APTR)mpega_call_time,
  (APTR)mpega_call_find_sync,
  (APTR)mpega_call_scale,
  (APTR)mpega_call_decode_to,
  (APTR)-1
};

static const struct Resident mpega_romtag __attribute__((used)) = {
  RTC_MATCHWORD,
  (struct Resident *)&mpega_romtag,
  (APTR)(&mpega_romtag + 1),
  0,
  MPEGA_LIBRARY_VERSION,
  NT_LIBRARY,
  0,
  (char *)MPEGA_LIBRARY_NAME,
  (char *)MPEGA_LIBRARY_ID_STRING,
  (APTR)mpega_lib_init
};

static struct MPEGACompatBase *mpega_lib_open(
    REG(a6, struct MPEGACompatBase *base))
{
  if (!base) {
    return 0;
  }

  base->library.lib_OpenCnt++;
  base->library.lib_Flags &= (uint8_t)~LIBF_DELEXP;
  return base;
}

static BPTR mpega_lib_close(REG(a6, struct MPEGACompatBase *base))
{
  if (!base || base->library.lib_OpenCnt == 0) {
    return 0;
  }

  base->library.lib_OpenCnt--;
  if (base->library.lib_OpenCnt == 0 &&
      (base->library.lib_Flags & LIBF_DELEXP)) {
    return mpega_lib_expunge(base);
  }

  return 0;
}

static BPTR mpega_lib_expunge(REG(a6, struct MPEGACompatBase *base))
{
  BPTR segment;

  if (!base) {
    return 0;
  }
  if (base->library.lib_OpenCnt != 0 ||
      mpega_fallback_busy(&base->fallback)) {
    base->library.lib_Flags |= LIBF_DELEXP;
    return 0;
  }

  mpega_fallback_unload(&base->fallback);

  Remove((struct Node *)base);
  if (ZZ9KBase) {
    CloseLibrary(ZZ9KBase);
    ZZ9KBase = 0;
    base->zz9k_open_count = 0;
  }
  if (UtilityBase) {
    CloseLibrary(UtilityBase);
    UtilityBase = 0;
  }
  if (DOSBase) {
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = 0;
  }
  segment = base->segment;
  FreeMem((uint8_t *)base - base->library.lib_NegSize,
          base->library.lib_NegSize + base->library.lib_PosSize);
  return segment;
}

static ULONG mpega_lib_null(void)
{
  return 0;
}

static struct MPEGACompatBase *mpega_lib_init(REG(a0, BPTR segment))
{
  struct MPEGACompatBase *base;

  SysBase = *(struct ExecBase **)4;
  DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 36);
  if (!DOSBase) {
    return 0;
  }
  UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 36);
  if (!UtilityBase) {
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = 0;
    return 0;
  }

  base = (struct MPEGACompatBase *)MakeLibrary(
      (CONST_APTR)mpega_lib_vectors, 0, 0, sizeof(*base), 0);
  if (!base) {
    CloseLibrary(UtilityBase);
    UtilityBase = 0;
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = 0;
    return 0;
  }

  base->library.lib_Node.ln_Type = NT_LIBRARY;
  base->library.lib_Node.ln_Name = (char *)MPEGA_LIBRARY_NAME;
  base->library.lib_Flags = LIBF_CHANGED | LIBF_SUMUSED;
  base->library.lib_Version = MPEGA_LIBRARY_VERSION;
  base->library.lib_Revision = MPEGA_LIBRARY_REVISION;
  base->library.lib_IdString = (APTR)MPEGA_LIBRARY_ID_STRING;
  base->segment = segment;
  mpega_fallback_init(&base->fallback);

  AddLibrary((struct Library *)base);
  return base;
}

static void mpega_default_ctrl(MPEGA_CTRL *ctrl)
{
  memset(ctrl, 0, sizeof(*ctrl));
  ctrl->layer_1_2.mono.quality = MPEGA_QUALITY_HIGH;
  ctrl->layer_1_2.mono.freq_div = 1;
  ctrl->layer_1_2.mono.freq_max = 44100;
  ctrl->layer_1_2.stereo.quality = MPEGA_QUALITY_HIGH;
  ctrl->layer_1_2.stereo.freq_div = 1;
  ctrl->layer_1_2.stereo.freq_max = 44100;
  ctrl->layer_3 = ctrl->layer_1_2;
  ctrl->stream_buffer_size = MPEGA_DEFAULT_STREAM_BUFFER_BYTES;
  ctrl->check_mpeg = 1;
}

static LONG mpega_normalize_stream_buffer_size(LONG stream_buffer_size)
{
  if (stream_buffer_size <= 0) {
    stream_buffer_size = MPEGA_DEFAULT_STREAM_BUFFER_BYTES;
  }
  if (stream_buffer_size < 4) {
    stream_buffer_size = 4;
  }
  if (stream_buffer_size > (LONG)MPEGA_STREAM_CHUNK_BYTES) {
    stream_buffer_size = (LONG)MPEGA_STREAM_CHUNK_BYTES;
  }

  stream_buffer_size &= ~3L;
  if (stream_buffer_size <= 0) {
    stream_buffer_size = 4;
  }
  return stream_buffer_size;
}

static ULONG mpega_call_bs_access(MPEGACompatStream *state, APTR handle,
                                  MPEGA_ACCESS *access)
{
  if (!state || !state->ctrl.bs_access || !access) {
    return 0;
  }

  return CallHookPkt(state->ctrl.bs_access, handle, access);
}

static int mpega_stream_open_source(MPEGACompatStream *state, char *filename)
{
  MPEGA_ACCESS access;
  LONG end_pos;

  if (!state || (!filename && !state->ctrl.bs_access)) {
    return 0;
  }

  if (state->ctrl.bs_access) {
    memset(&access, 0, sizeof(access));
    access.func = MPEGA_BSFUNC_OPEN;
    access.data.open.stream_name = filename;
    access.data.open.buffer_size = state->input_chunk_bytes;
    access.data.open.stream_size = 0;
    state->hook_handle = (APTR)mpega_call_bs_access(state, 0, &access);
    if (!state->hook_handle) {
      return 0;
    }
    state->hook_open = 1;
    if (access.data.open.stream_size > 0) {
      state->stream_size = access.data.open.stream_size;
    }
    mpega_stream_update_estimated_duration(state);
    return 1;
  }

  state->file = Open((CONST_STRPTR)filename, MODE_OLDFILE);
  if (!state->file) {
    return 0;
  }

  end_pos = Seek(state->file, 0, OFFSET_END);
  if (end_pos >= 0) {
    state->stream_size = end_pos;
  }
  mpega_stream_update_estimated_duration(state);
  return mpega_stream_seek_source(state, 0);
}

static void mpega_stream_close_source(MPEGACompatStream *state)
{
  MPEGA_ACCESS access;

  if (!state) {
    return;
  }

  if (state->hook_open) {
    memset(&access, 0, sizeof(access));
    access.func = MPEGA_BSFUNC_CLOSE;
    (void)mpega_call_bs_access(state, state->hook_handle, &access);
    state->hook_open = 0;
    state->hook_handle = 0;
  }
  if (state->file) {
    Close(state->file);
    state->file = 0;
  }
}

static LONG mpega_stream_read_source(MPEGACompatStream *state, void *buffer,
                                     LONG length)
{
  MPEGA_ACCESS access;
  ULONG got;

  if (!state || !buffer || length <= 0) {
    return 0;
  }

  if (state->ctrl.bs_access) {
    if (!state->hook_open || !state->hook_handle) {
      return -1;
    }
    memset(&access, 0, sizeof(access));
    access.func = MPEGA_BSFUNC_READ;
    access.data.read.buffer = buffer;
    access.data.read.num_bytes = length;
    got = mpega_call_bs_access(state, state->hook_handle, &access);
    if (got > (ULONG)length) {
      return -1;
    }
    return (LONG)got;
  }

  if (!state->file) {
    return -1;
  }
  return Read(state->file, buffer, length);
}

static int mpega_stream_seek_source(MPEGACompatStream *state,
                                    LONG abs_byte_pos)
{
  MPEGA_ACCESS access;

  if (!state || abs_byte_pos < 0) {
    return 0;
  }

  if (state->ctrl.bs_access) {
    if (!state->hook_open || !state->hook_handle) {
      return 0;
    }
    memset(&access, 0, sizeof(access));
    access.func = MPEGA_BSFUNC_SEEK;
    access.data.seek.abs_byte_seek_pos = abs_byte_pos;
    return mpega_call_bs_access(state, state->hook_handle, &access) == 0U;
  }

  return state->file && Seek(state->file, abs_byte_pos, OFFSET_BEGINNING) >= 0;
}

static uint32_t mpega_read_be32(const UBYTE *bytes)
{
  return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
         ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static int mpega_parse_mpeg_header(const UBYTE *bytes,
                                   MPEGAParsedHeader *parsed)
{
  static const WORD bitrate_mpeg1_layer1[16] = {
    0, 32, 64, 96, 128, 160, 192, 224,
    256, 288, 320, 352, 384, 416, 448, 0
  };
  static const WORD bitrate_mpeg1_layer2[16] = {
    0, 32, 48, 56, 64, 80, 96, 112,
    128, 160, 192, 224, 256, 320, 384, 0
  };
  static const WORD bitrate_mpeg1_layer3[16] = {
    0, 32, 40, 48, 56, 64, 80, 96,
    112, 128, 160, 192, 224, 256, 320, 0
  };
  static const WORD bitrate_mpeg2_layer1[16] = {
    0, 32, 48, 56, 64, 80, 96, 112,
    128, 144, 160, 176, 192, 224, 256, 0
  };
  static const WORD bitrate_mpeg2_layer23[16] = {
    0, 8, 16, 24, 32, 40, 48, 56,
    64, 80, 96, 112, 128, 144, 160, 0
  };
  static const LONG sample_rates[3] = {44100, 48000, 32000};
  UBYTE version_bits;
  UBYTE layer_bits;
  UBYTE bitrate_index;
  UBYTE sample_rate_index;
  UBYTE padding;
  const WORD *bitrate_table;
  LONG frequency;

  if (!bytes || !parsed || !mpega_is_sync_header(bytes)) {
    return 0;
  }

  version_bits = (UBYTE)((bytes[1] >> 3) & 0x03U);
  layer_bits = (UBYTE)((bytes[1] >> 1) & 0x03U);
  bitrate_index = (UBYTE)((bytes[2] >> 4) & 0x0fU);
  sample_rate_index = (UBYTE)((bytes[2] >> 2) & 0x03U);
  padding = (UBYTE)((bytes[2] >> 1) & 0x01U);

  parsed->norm = version_bits == 3U ? 1 : 2;
  parsed->layer = (WORD)(4U - layer_bits);
  parsed->mode = (WORD)((bytes[3] >> 6) & 0x03U);
  parsed->channels = parsed->mode == MPEGA_MODE_MONO ? 1 : 2;
  parsed->private_bit = (WORD)(bytes[2] & 0x01U);
  parsed->copyright = (WORD)((bytes[3] >> 3) & 0x01U);
  parsed->original = (WORD)((bytes[3] >> 2) & 0x01U);

  if (version_bits == 3U) {
    if (parsed->layer == 1) {
      bitrate_table = bitrate_mpeg1_layer1;
    } else if (parsed->layer == 2) {
      bitrate_table = bitrate_mpeg1_layer2;
    } else {
      bitrate_table = bitrate_mpeg1_layer3;
    }
    frequency = sample_rates[sample_rate_index];
  } else {
    bitrate_table = parsed->layer == 1 ? bitrate_mpeg2_layer1 :
                    bitrate_mpeg2_layer23;
    frequency = sample_rates[sample_rate_index] >> (version_bits == 2U ? 1 : 2);
  }

  parsed->bitrate = bitrate_table[bitrate_index];
  parsed->frequency = frequency;
  if (parsed->layer == 1) {
    parsed->samples_per_frame = 384;
  } else if (parsed->layer == 3 && version_bits != 3U) {
    parsed->samples_per_frame = 576;
  } else {
    parsed->samples_per_frame = 1152;
  }

  (void)padding;
  return parsed->bitrate > 0 && parsed->frequency > 0;
}

static void mpega_apply_mpeg_header(MPEGACompatStream *state,
                                    const MPEGAParsedHeader *parsed,
                                    LONG header_offset)
{
  uint32_t output_channels;
  uint32_t freq_div;

  if (!state || !parsed) {
    return;
  }

  state->stream.norm = parsed->norm;
  state->stream.layer = parsed->layer;
  state->stream.mode = parsed->mode;
  state->stream.bitrate = parsed->bitrate;
  state->stream.frequency = parsed->frequency;
  state->stream.channels = parsed->channels;
  state->stream.private_bit = parsed->private_bit;
  state->stream.copyright = parsed->copyright;
  state->stream.original = parsed->original;
  state->stream_start_byte = header_offset;
  state->header_probed = 1;
  output_channels = mpega_output_channels(state, parsed->channels);
  freq_div = mpega_output_freq_div(state, output_channels,
                                   (uint32_t)parsed->frequency);
  state->stream.dec_frequency = parsed->frequency / (LONG)freq_div;
  state->stream.dec_channels =
      (WORD)mpega_output_channels(state, parsed->channels);
  state->stream.dec_quality = mpega_output_quality(state, output_channels);
  mpega_stream_update_estimated_duration(state);
}

static void mpega_apply_xing_info(MPEGACompatStream *state,
                                  const MPEGAParsedHeader *parsed,
                                  const UBYTE *bytes, LONG length,
                                  LONG header_offset)
{
  uint32_t flags;
  uint64_t frames;
  uint64_t stream_bytes;
  LONG xing_offset;
  LONG field_offset;

  if (!state || !parsed || !bytes || header_offset < 0 ||
      parsed->frequency <= 0 || parsed->samples_per_frame <= 0) {
    return;
  }

  if (parsed->norm == 1) {
    xing_offset = header_offset + (parsed->channels == 1 ? 21 : 36);
  } else {
    xing_offset = header_offset + (parsed->channels == 1 ? 13 : 21);
  }
  if (xing_offset < 0 || xing_offset > length - 16) {
    return;
  }
  if (memcmp(bytes + xing_offset, "Xing", 4U) != 0 &&
      memcmp(bytes + xing_offset, "Info", 4U) != 0) {
    return;
  }

  flags = mpega_read_be32(bytes + xing_offset + 4);
  field_offset = xing_offset + 8;
  frames = 0;
  stream_bytes = 0;
  if ((flags & 0x01U) != 0U && field_offset <= length - 4) {
    frames = mpega_read_be32(bytes + field_offset);
    field_offset += 4;
  }
  if ((flags & 0x02U) != 0U && field_offset <= length - 4) {
    stream_bytes = mpega_read_be32(bytes + field_offset);
    state->xing_stream_bytes = (uint32_t)stream_bytes;
    field_offset += 4;
  }
  if ((flags & 0x04U) != 0U && field_offset <= length - MPEGA_XING_TOC_BYTES) {
    memcpy(state->xing_toc, bytes + field_offset, MPEGA_XING_TOC_BYTES);
    state->xing_has_toc = 1U;
    field_offset += MPEGA_XING_TOC_BYTES;
  }

  if (frames != 0U) {
    state->stream.ms_duration = (ULONG)((frames *
        (uint32_t)parsed->samples_per_frame * 1000ULL) /
        (uint32_t)parsed->frequency);
  }
  if (frames != 0U && stream_bytes == 0U && state->stream_size > state->stream_start_byte) {
    stream_bytes = (uint64_t)(state->stream_size - state->stream_start_byte);
    if (stream_bytes <= 0xffffffffULL) {
      state->xing_stream_bytes = (uint32_t)stream_bytes;
    }
  }
  if (stream_bytes != 0U && state->stream.ms_duration != 0U) {
    uint64_t average_bitrate = (stream_bytes * 8ULL) /
                               state->stream.ms_duration;

    if (average_bitrate != 0U && average_bitrate <= 32767U) {
      state->stream.bitrate = (WORD)((stream_bytes * 8ULL) /
                                     state->stream.ms_duration);
    }
  }
}

static int mpega_stream_probe_metadata(MPEGACompatStream *state)
{
  LONG total;
  int read_ok;
  int found_header;

  if (!state || !state->input_buffer) {
    return 0;
  }

  total = 0;
  read_ok = 1;
  found_header = 0;
  while (total < (LONG)MPEGA_STREAM_CHUNK_BYTES) {
    LONG request = state->input_chunk_bytes;
    LONG got;
    LONG i;

    if (request <= 0) {
      request = (LONG)MPEGA_STREAM_CHUNK_BYTES;
    }
    if (request > (LONG)MPEGA_STREAM_CHUNK_BYTES - total) {
      request = (LONG)MPEGA_STREAM_CHUNK_BYTES - total;
    }
    got = mpega_stream_read_source(state, state->input_buffer + total,
                                   request);
    if (got < 0) {
      read_ok = 0;
      break;
    }
    if (got == 0) {
      break;
    }
    total += got;

    for (i = 0; i <= total - 4; i++) {
      MPEGAParsedHeader parsed;

      if (!mpega_parse_mpeg_header(state->input_buffer + i, &parsed)) {
        continue;
      }
      mpega_apply_mpeg_header(state, &parsed, i);
      mpega_apply_xing_info(state, &parsed, state->input_buffer, total, i);
      found_header = 1;
      total = (LONG)MPEGA_STREAM_CHUNK_BYTES;
      break;
    }
  }

  if (!mpega_stream_seek_source(state, 0)) {
    return 0;
  }
  return read_ok && found_header;
}

static int mpega_open_probe_metadata(MPEGACompatStream *state)
{
  if (!state) {
    return 0;
  }
  if (state->ctrl.check_mpeg == 0) {
    if (!mpega_stream_seek_source(state, 0)) {
      return 1;
    }
    (void)mpega_stream_probe_metadata(state);
    return 1;
  }

  return mpega_stream_probe_metadata(state);
}

static void mpega_stream_update_estimated_duration(MPEGACompatStream *state)
{
  LONG stream_bytes;

  if (!state || state->stream_size <= 0 || state->stream.bitrate <= 0) {
    return;
  }

  stream_bytes = state->stream_size;
  if (state->stream_start_byte > 0 &&
      state->stream_size > state->stream_start_byte) {
    stream_bytes = state->stream_size - state->stream_start_byte;
  }

  state->stream.ms_duration = (ULONG)(((uint64_t)stream_bytes * 8ULL) /
                                      (uint32_t)state->stream.bitrate);
}

static int mpega_output_force_mono(const MPEGACompatStream *state)
{
  if (!state) {
    return 0;
  }

  if (state->stream.layer == 3) {
    return state->ctrl.layer_3.force_mono != 0;
  }
  return state->ctrl.layer_1_2.force_mono != 0;
}

static uint32_t mpega_output_channels(const MPEGACompatStream *state,
                                      uint32_t source_channels)
{
  return source_channels > 1U && mpega_output_force_mono(state) ? 1U :
         source_channels;
}

static const MPEGA_OUTPUT *mpega_selected_output(
    const MPEGACompatStream *state, uint32_t output_channels)
{
  const MPEGA_LAYER *layer;

  if (!state) {
    return 0;
  }

  layer = state->stream.layer == 3 ? &state->ctrl.layer_3 :
          &state->ctrl.layer_1_2;
  return output_channels == 1U ? &layer->mono : &layer->stereo;
}

static uint32_t mpega_output_freq_div(const MPEGACompatStream *state,
                                      uint32_t output_channels,
                                      uint32_t source_rate)
{
  const MPEGA_OUTPUT *output =
      mpega_selected_output(state, output_channels);

  if (!output || source_rate == 0U) {
    return 1U;
  }

  if (output->freq_div == 1 || output->freq_div == 2 ||
      output->freq_div == 4) {
    return (uint32_t)output->freq_div;
  }

  if (output->freq_div == 0) {
    LONG freq_max = output->freq_max;
    uint32_t freq = source_rate;
    uint32_t freq_div = 1U;

    while ((freq_max < 0 || freq > (uint32_t)freq_max) && freq_div < 4U) {
      freq_div <<= 1;
      freq >>= 1;
    }

    return freq_div == 2U || freq_div == 4U ? freq_div : 1U;
  }

  return 1U;
}

static WORD mpega_output_quality(const MPEGACompatStream *state,
                                 uint32_t output_channels)
{
  const MPEGA_OUTPUT *output =
      mpega_selected_output(state, output_channels);

  if (!output) {
    return MPEGA_QUALITY_HIGH;
  }
  if (output->quality > MPEGA_QUALITY_HIGH) {
    return MPEGA_QUALITY_HIGH;
  }
  if (output->quality < MPEGA_QUALITY_LOW) {
    return MPEGA_QUALITY_LOW;
  }
  return output->quality;
}

static void mpega_stream_set_defaults(MPEGACompatStream *state)
{
  MPEGA_STREAM *stream = &state->stream;

  stream->norm = 1;
  stream->layer = 3;
  stream->mode = MPEGA_MODE_J_STEREO;
  stream->bitrate = 128;
  stream->frequency = 44100;
  stream->channels = 2;
  stream->ms_duration = 0;
  stream->private_bit = 0;
  stream->copyright = 0;
  stream->original = 0;
  stream->dec_channels = 2;
  stream->dec_quality = MPEGA_QUALITY_HIGH;
  stream->dec_frequency = 44100;
  state->stream.handle = state;
  state->scale_percent = 100;
  state->reduce_div = 1U;
  state->dither = 0x2545F491UL;
  state->input_chunk_bytes = MPEGA_STREAM_CHUNK_BYTES;
  state->mp3_ring.handle = ZZ9K_INVALID_HANDLE;
  state->pcm_ring.handle = ZZ9K_INVALID_HANDLE;
  state->staging.handle = ZZ9K_INVALID_HANDLE;
}

static void mpega_stream_end_session(MPEGACompatStream *state)
{
  ZZ9KAudioStreamResult close_result;

  if (!state) {
    return;
  }
  if (state->session_open) {
    (void)mpega_ack_pcm(state, 1);
    (void)ZZ9KAudioStreamClose(state->session, 0, &close_result);
    state->session_open = 0;
    state->session = 0U;
  }
  if (state->mp3_allocated) {
    (void)ZZ9KFreeShared(state->mp3_ring.handle);
    state->mp3_allocated = 0;
    state->mp3_ring.handle = ZZ9K_INVALID_HANDLE;
    state->mp3_ring.data = 0;
    state->mp3_ring.length = 0;
  }
  if (state->pcm_allocated) {
    (void)ZZ9KFreeShared(state->pcm_ring.handle);
    state->pcm_allocated = 0;
    state->pcm_ring.handle = ZZ9K_INVALID_HANDLE;
    state->pcm_ring.data = 0;
    state->pcm_ring.length = 0;
  }
  if (state->staging_allocated) {
    (void)ZZ9KFreeShared(state->staging.handle);
    state->staging_allocated = 0;
    state->staging.handle = ZZ9K_INVALID_HANDLE;
    state->staging.data = 0;
    state->staging.length = 0;
  }
  if (state->zz9k_acquired) {
    mpega_zz9k_release(state->base);
    state->zz9k_acquired = 0;
  }
}

static void mpega_stream_free(MPEGACompatStream *state)
{
  if (!state) {
    return;
  }

  if (state->fb_stream) {
    mpega_fb_close(state->fb_lib, state->fb_stream);
    state->fb_stream = 0;
    mpega_fallback_stream_closed(&state->base->fallback);
  }
  mpega_stream_end_session(state);
  if (state->input_buffer) {
    FreeMem(state->input_buffer, MPEGA_STREAM_CHUNK_BYTES);
  }
  mpega_stream_close_source(state);
  FreeMem(state, sizeof(*state));
}

static LONG mpega_xing_toc_seek_byte(const MPEGACompatStream *state,
                                     ULONG ms_time_position)
{
  uint64_t scaled_percent;
  uint64_t byte_offset;
  uint64_t byte_pos;
  uint32_t stream_bytes;
  uint32_t index;
  uint32_t fraction;
  uint32_t fa;
  uint32_t fb;
  int32_t interpolated;
  LONG stream_start;

  if (!state || !state->xing_has_toc || state->stream.ms_duration == 0U) {
    return -1;
  }
  if (ms_time_position >= state->stream.ms_duration) {
    return -1;
  }

  stream_bytes = state->xing_stream_bytes;
  if (stream_bytes == 0U && state->stream_size > state->stream_start_byte) {
    stream_bytes = (uint32_t)(state->stream_size - state->stream_start_byte);
  }
  if (stream_bytes == 0U) {
    return -1;
  }

  scaled_percent = ((uint64_t)ms_time_position * 10000ULL) /
                   state->stream.ms_duration;
  index = (uint32_t)(scaled_percent / 100ULL);
  if (index > MPEGA_XING_TOC_BYTES - 1U) {
    index = MPEGA_XING_TOC_BYTES - 1U;
  }
  fraction = (uint32_t)(scaled_percent - ((uint64_t)index * 100ULL));
  fa = state->xing_toc[index];
  fb = index < MPEGA_XING_TOC_BYTES - 1U ? state->xing_toc[index + 1U] :
       256U;
  interpolated = ((int32_t)fa * 100) +
                 (((int32_t)fb - (int32_t)fa) * (int32_t)fraction);
  if (interpolated < 0) {
    interpolated = 0;
  }
  if (interpolated > 25600) {
    interpolated = 25600;
  }

  byte_offset = ((uint64_t)stream_bytes * (uint32_t)interpolated) /
                (256ULL * 100ULL);
  stream_start = state->stream_start_byte > 0 ? state->stream_start_byte : 0;
  byte_pos = (uint64_t)stream_start + byte_offset;
  if (byte_pos > 2147483647ULL) {
    return -1;
  }
  if (state->stream_size > 0 && byte_pos >= (uint64_t)state->stream_size) {
    return -1;
  }

  return (LONG)byte_pos;
}

static LONG mpega_stream_seek_byte_for_ms(MPEGACompatStream *state,
                                          ULONG ms_time_position)
{
  uint64_t byte_pos;
  LONG toc_byte_pos;
  LONG stream_start;
  LONG stream_bytes;

  if (!state) {
    return -1;
  }
  if (ms_time_position == 0U) {
    return state->stream_start_byte > 0 ? state->stream_start_byte : 0;
  }

  byte_pos = 0ULL;
  toc_byte_pos = mpega_xing_toc_seek_byte(state, ms_time_position);
  if (toc_byte_pos >= 0) {
    return toc_byte_pos;
  }

  stream_start = state->stream_start_byte > 0 ? state->stream_start_byte : 0;
  if (state->stream_size > 0 && state->stream.ms_duration != 0U) {
    if (ms_time_position >= state->stream.ms_duration) {
      return -1;
    }
    stream_bytes = state->stream_size;
    if (stream_start > 0 && state->stream_size > state->stream_start_byte) {
      stream_bytes = state->stream_size - state->stream_start_byte;
    }
    byte_pos = (uint64_t)stream_start +
               (((uint64_t)stream_bytes * ms_time_position) /
                state->stream.ms_duration);
  } else if (state->stream.bitrate > 0) {
    byte_pos = (uint64_t)stream_start +
               (((uint64_t)ms_time_position *
                 (uint32_t)state->stream.bitrate) / 8ULL);
  } else {
    return -1;
  }

  if (byte_pos > 2147483647ULL) {
    return -1;
  }
  if (state->stream_size > 0 && byte_pos >= (uint64_t)state->stream_size) {
    return -1;
  }

  return (LONG)byte_pos;
}

static int mpega_stream_restart_at_byte(MPEGACompatStream *state,
                                        LONG abs_byte_pos,
                                        ULONG ms_time_position)
{
  if (!state || abs_byte_pos < 0) {
    return 0;
  }

  mpega_stream_end_session(state);
  if (!mpega_stream_seek_source(state, abs_byte_pos)) {
    return 0;
  }

  memset(&state->result, 0, sizeof(state->result));
  state->pcm_seen_bytes = 0U;
  state->pcm_read_offset = 0U;
  state->pcm_pending_ack_bytes = 0U;
  state->time_ms = ms_time_position;
  state->time_ms_accum = 0U;
  memset(&state->feed, 0, sizeof(state->feed));
  return mpega_stream_begin(state);
}

static int mpega_zz9k_acquire(struct MPEGACompatBase *base)
{
  ZZ9KServiceInfo service;
  int opened_now;

  if (!base) {
    return 0;
  }

  opened_now = 0;
  if (!ZZ9KBase) {
    ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                           ZZ9K_LIBRARY_VERSION);
    if (!ZZ9KBase) {
      return 0;
    }
    opened_now = 1;
  }

  /* ALLOC_FLAGS (rev 26) rather than AUDIO_STREAM (rev 22): this library
   * passes ZZ9K_ALLOC_HOST_WINDOW/CARD_ONLY, and it is zz9k.library that
   * strips HOST_WINDOW on Zorro 3. An older library forwards the bit
   * verbatim and new firmware would then place the compact PCM ring
   * inside Z3 P96 VRAM -- refuse the skew instead. */
  if (ZZ9KBase->lib_Revision < ZZ9K_LIBRARY_MIN_REVISION_ALLOC_FLAGS ||
      ZZ9KQueryService(ZZ9K_SERVICE_AUDIO, &service) != ZZ9K_STATUS_OK ||
      (service.flags & ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM) == 0U) {
    if (opened_now) {
      CloseLibrary(ZZ9KBase);
      ZZ9KBase = 0;
    }
    return 0;
  }

  base->audio_flags = service.flags;
  base->zz9k_open_count++;
  return 1;
}

static void mpega_zz9k_release(struct MPEGACompatBase *base)
{
  if (!base || base->zz9k_open_count == 0) {
    return;
  }

  base->zz9k_open_count--;
  if (base->zz9k_open_count == 0 && ZZ9KBase) {
    CloseLibrary(ZZ9KBase);
    ZZ9KBase = 0;
  }
}

static void mpega_update_stream_info(MPEGACompatStream *state)
{
  uint32_t channels;
  uint32_t output_channels;
  uint32_t freq_div;

  if (!state || state->fb_stream || state->result.sample_rate == 0U) {
    return;
  }

  channels = state->result.channels;
  if (channels == 0U || channels > MPEGA_MAX_CHANNELS) {
    channels = MPEGA_MAX_CHANNELS;
  }

  if (state->reduce_active) {
    /* The ring holds what the card reduced the stream to. The stream's own
     * rate and layout are what it was reduced from. */
    state->stream.frequency =
        (LONG)(state->result.sample_rate * state->reduce_div);
    state->stream.channels = (WORD)(state->reduce_mono ? 2U : channels);
    state->stream.dec_frequency = (LONG)state->result.sample_rate;
    state->stream.dec_channels = (WORD)channels;
    state->stream.dec_quality = mpega_output_quality(state, channels);
    return;
  }

  state->stream.frequency = (LONG)state->result.sample_rate;
  state->stream.channels = (WORD)channels;
  output_channels = mpega_output_channels(state, channels);
  freq_div = mpega_output_freq_div(state, output_channels,
                                   state->result.sample_rate);
  state->stream.dec_frequency = (LONG)state->result.sample_rate /
                                (LONG)freq_div;
  state->stream.dec_channels = (WORD)mpega_output_channels(state, channels);
  state->stream.dec_quality = mpega_output_quality(state, output_channels);
}

typedef struct MPEGABeginContext {
  MPEGACompatStream *state;
  uint32_t staging_bytes;
} MPEGABeginContext;

static int mpega_begin_call(void *context, uint32_t output_hz,
                            uint32_t output_channels)
{
  MPEGABeginContext *begin_context = (MPEGABeginContext *)context;
  MPEGACompatStream *state = begin_context->state;
  ZZ9KAudioStreamBeginDesc begin;

  /* low_water is the firmware pump's PCM refill threshold; it must stay
   * below the ring capacity, so scale it with the staging quantum. */
  if (!zz9k_audio_build_stream_begin_desc(
          &begin, state->mp3_ring.handle, state->mp3_ring.length,
          state->pcm_ring.handle, state->pcm_ring.length, output_hz,
          output_channels, ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE,
          begin_context->staging_bytes, 0U, 0U)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return ZZ9KAudioStreamBegin(&begin, &state->result);
}

static int mpega_stream_begin(MPEGACompatStream *state)
{
  MPEGABeginContext begin_context;
  MPEGAReducePlan plan;
  uint32_t staging_bytes;
  uint32_t freq_div;

  if (!state || !mpega_zz9k_acquire(state->base)) {
    return 0;
  }
  state->zz9k_acquired = 1;
  state->reduce_active = 0;
  state->reduce_mono = 0;
  state->reduce_div = 1U;

  /* The decode input ring is card-side only (the 68k passes just the
   * handle), so it must not consume board-window mapping space. */
  if (ZZ9KAllocShared(MPEGA_MP3_RING_CAPACITY, 16U, ZZ9K_ALLOC_CARD_ONLY,
                      &state->mp3_ring) != ZZ9K_STATUS_OK) {
    return 0;
  }
  state->mp3_allocated = 1;

  /* The PCM ring and the feed staging buffer ARE host-visible (the planar
   * copy reads the ring, feeds write staging). HOST_WINDOW is stripped by
   * the library on Zorro 3 (normal shared-heap alloc, full sizes); on
   * Zorro 2 it lands them in the firmware's small board-window heap,
   * where the full-size ring cannot fit -- fall back to compact sizes. */
  staging_bytes = MPEGA_STREAM_CHUNK_BYTES;
  if (ZZ9KAllocShared(MPEGA_PCM_RING_CAPACITY, 16U, ZZ9K_ALLOC_HOST_WINDOW,
                      &state->pcm_ring) != ZZ9K_STATUS_OK) {
    if (ZZ9KAllocShared(MPEGA_PCM_RING_COMPACT_CAPACITY, 16U,
                        ZZ9K_ALLOC_HOST_WINDOW,
                        &state->pcm_ring) != ZZ9K_STATUS_OK) {
      return 0;
    }
    staging_bytes = MPEGA_STREAM_CHUNK_COMPACT_BYTES;
  }
  state->pcm_allocated = 1;

  if (ZZ9KAllocShared(staging_bytes, 16U, ZZ9K_ALLOC_HOST_WINDOW,
                      &state->staging) != ZZ9K_STATUS_OK) {
    return 0;
  }
  state->staging_allocated = 1;
  /* Never stage more per feed than the staging buffer holds. */
  if (state->input_chunk_bytes > (LONG)state->staging.length) {
    state->input_chunk_bytes = (LONG)state->staging.length;
  }

  /* A card that reports AUDIO_STREAM_REDUCE mixes down and decimates (with a
   * low-pass filter) for us, so the 68k reads a ring in the layout it hands
   * out. An older card rejects the request and the 68k reduces instead. */
  freq_div = mpega_output_freq_div(
      state, mpega_output_channels(state, (uint32_t)state->stream.channels),
      (uint32_t)state->stream.frequency);
  mpega_reduce_plan(&plan, state->base->audio_flags, state->header_probed,
                    (uint32_t)state->stream.channels,
                    mpega_output_force_mono(state),
                    (uint32_t)state->stream.frequency, freq_div);
  begin_context.state = state;
  begin_context.staging_bytes = staging_bytes;
  if (mpega_reduce_begin(&plan, mpega_begin_call, &begin_context) !=
      ZZ9K_STATUS_OK) {
    return 0;
  }
  if (plan.output_hz != 0U || plan.output_channels != 0U) {
    state->reduce_active = 1;
    state->reduce_mono = plan.output_channels == 1U;
    state->reduce_div = plan.output_hz != 0U ? freq_div : 1U;
  }

  state->session_open = 1;
  state->session = state->result.session;
  state->pcm_read_offset = state->result.pcm_read;
  return 1;
}

static uint32_t mpega_pcm_available(const MPEGACompatStream *state)
{
  if (!state || state->result.bytes_produced < state->pcm_seen_bytes) {
    return 0;
  }

  return state->result.bytes_produced - state->pcm_seen_bytes;
}

static int mpega_stream_feed(MPEGACompatStream *state, uint32_t length,
                             uint32_t flags)
{
  ZZ9KAudioStreamFeedDesc feed;
  ZZ9KAudioStreamResult result;

  if (!zz9k_audio_build_stream_feed_desc(&feed, state->session,
                                         state->staging.handle, 0U, length,
                                         flags) ||
      ZZ9KAudioStreamFeed(&feed, &result) != ZZ9K_STATUS_OK) {
    return 0;
  }
  state->result = result;
  mpega_update_stream_info(state);
  return 1;
}

/* A zero-byte READ runs a decode pass without returning PCM credit (an empty
 * FEED is not a valid request). The loop below calls it with no credit
 * pending. */
static int mpega_stream_kick(MPEGACompatStream *state)
{
  ZZ9KAudioStreamResult result;

  if (ZZ9KAudioStreamRead(state->session, 0U, 0U, &result) != ZZ9K_STATUS_OK) {
    return 0;
  }
  state->result = result;
  state->pcm_read_offset = state->result.pcm_read;
  mpega_update_stream_info(state);
  return 1;
}

static int mpega_feed_op_pcm_available(void *context)
{
  return mpega_pcm_available((MPEGACompatStream *)context) != 0U;
}

static int mpega_feed_op_ack(void *context)
{
  return mpega_ack_pcm((MPEGACompatStream *)context, 1);
}

static int mpega_feed_op_read_stage(void *context, uint32_t *got)
{
  MPEGACompatStream *state = (MPEGACompatStream *)context;
  LONG length = mpega_stream_read_source(state, state->input_buffer,
                                         state->input_chunk_bytes);

  if (length < 0) {
    return 0;
  }
  *got = (uint32_t)length;
  return length == 0 ||
         mpega_shared_copy_to_bytes(&state->staging, 0U, state->input_buffer,
                                    (uint32_t)length);
}

static int mpega_feed_op_feed(void *context, uint32_t length, uint32_t flags)
{
  return mpega_stream_feed((MPEGACompatStream *)context, length, flags);
}

static int mpega_feed_op_kick(void *context)
{
  return mpega_stream_kick((MPEGACompatStream *)context);
}

static uint32_t mpega_feed_op_result_flags(void *context)
{
  return ((MPEGACompatStream *)context)->result.flags;
}

static uint32_t mpega_feed_op_bytes_consumed(void *context)
{
  return ((MPEGACompatStream *)context)->result.bytes_consumed;
}

static uint32_t mpega_feed_op_bytes_produced(void *context)
{
  return ((MPEGACompatStream *)context)->result.bytes_produced;
}

/* Feeds the card until it has PCM for us (mpega_feed_until_pcm). */
static int mpega_stream_feed_until_pcm(MPEGACompatStream *state)
{
  MPEGAFeedOps ops;

  if (!state) {
    return MPEGA_ERR_BADVALUE;
  }
  ops.context = state;
  ops.pcm_available = mpega_feed_op_pcm_available;
  ops.ack = mpega_feed_op_ack;
  ops.read_stage = mpega_feed_op_read_stage;
  ops.feed = mpega_feed_op_feed;
  ops.kick = mpega_feed_op_kick;
  ops.result_flags = mpega_feed_op_result_flags;
  ops.bytes_consumed = mpega_feed_op_bytes_consumed;
  ops.bytes_produced = mpega_feed_op_bytes_produced;
  return mpega_feed_until_pcm(&state->feed, &ops, MPEGA_MAX_FEED_ATTEMPTS);
}

static uint32_t mpega_source_channel_count(const MPEGACompatStream *state)
{
  uint32_t channels = state->result.channels ? state->result.channels :
                      (uint32_t)state->stream.channels;

  if (channels == 0U || channels > MPEGA_MAX_CHANNELS) {
    channels = MPEGA_MAX_CHANNELS;
  }
  return channels;
}

static uint32_t mpega_output_channel_count(const MPEGACompatStream *state)
{
  uint32_t channels =
      mpega_output_channels(state, mpega_source_channel_count(state));

  if (channels == 0U || channels > MPEGA_MAX_CHANNELS) {
    channels = MPEGA_MAX_CHANNELS;
  }
  return channels;
}

static uint32_t mpega_pcm_divisor(const MPEGACompatStream *state,
                                  uint32_t output_channels,
                                  uint32_t source_rate)
{
  /* A card that reduced the stream already sent the rate asked for. */
  return state->reduce_active ? 1U :
         mpega_output_freq_div(state, output_channels, source_rate);
}

/*
 * Takes the next frame from the PCM ring into dst0 (left, or mono) and dst1
 * (right, only for two output channels). Returns the samples per channel, 0
 * when the ring holds nothing, or an error. *clipped is increased by the
 * samples MPEGA_scale clipped.
 */
static LONG mpega_take_frame(MPEGACompatStream *state, int16_t *dst0,
                             int16_t *dst1, uint32_t *clipped)
{
  uint32_t source_channels;
  uint32_t output_channels;
  uint32_t available;
  uint32_t bytes;
  uint32_t frame_bytes;
  uint32_t source_samples;
  uint32_t output_samples;
  uint32_t source_rate;
  uint32_t freq_div;
  uint32_t offset;

  if (!state || !dst0) {
    return MPEGA_ERR_BADVALUE;
  }

  source_channels = mpega_source_channel_count(state);
  output_channels = mpega_output_channel_count(state);
  if (output_channels == 2U && !dst1) {
    return MPEGA_ERR_BADVALUE;
  }

  available = mpega_pcm_available(state);
  if (available == 0U) {
    return 0;
  }
  if (available > state->pcm_ring.length) {
    return MPEGA_ERR_BADFRAME;
  }
  if (state->pcm_read_offset >= state->pcm_ring.length) {
    return MPEGA_ERR_BADFRAME;
  }

  bytes = MPEGA_PCM_SIZE * source_channels * 2U;
  if (bytes > available) {
    bytes = available;
  }
  frame_bytes = source_channels * 2U;
  bytes &= ~(frame_bytes - 1U);
  source_samples = bytes / frame_bytes;
  source_rate = state->result.sample_rate ? state->result.sample_rate :
                (uint32_t)state->stream.frequency;
  freq_div = mpega_pcm_divisor(state, output_channels, source_rate);

  mpega_ring_read((uint8_t *)state->raw,
                  (const volatile uint8_t *)state->pcm_ring.data,
                  state->pcm_ring.length, state->pcm_read_offset, bytes);
  offset = mpega_ring_advance(state->pcm_read_offset, bytes,
                              state->pcm_ring.length);
  output_samples = mpega_pcm_prepare((const uint8_t *)state->raw,
                                     source_samples, source_channels,
                                     output_channels, freq_div,
                                     (int32_t)state->scale_percent, dst0,
                                     dst1, clipped);

  if (state->pcm_pending_ack_bytes > 0xffffffffUL - bytes) {
    return MPEGA_ERR_BADFRAME;
  }
  state->pcm_seen_bytes += bytes;
  state->pcm_pending_ack_bytes += bytes;
  state->pcm_read_offset = offset;
  mpega_update_time_ms(state, source_samples);

  if (!mpega_ack_pcm(state, 0)) {
    return MPEGA_ERR_BADFRAME;
  }

  return (LONG)output_samples;
}

static int mpega_ack_pcm(MPEGACompatStream *state, int force)
{
  ZZ9KAudioStreamResult result;

  if (!state || !state->session_open) {
    return 0;
  }
  if (state->pcm_pending_ack_bytes == 0U) {
    return 1;
  }
  if (!force && state->pcm_pending_ack_bytes < MPEGA_PCM_ACK_BATCH_BYTES) {
    return 1;
  }

  if (ZZ9KAudioStreamRead(state->session, state->pcm_pending_ack_bytes,
                          0U, &result) != ZZ9K_STATUS_OK) {
    return 0;
  }
  state->result = result;

  state->pcm_pending_ack_bytes = 0U;
  state->pcm_read_offset = state->result.pcm_read;
  mpega_update_stream_info(state);
  return 1;
}

static void mpega_update_time_ms(MPEGACompatStream *state, uint32_t samples)
{
  uint32_t sample_rate;
  uint32_t i;

  if (!state || samples == 0U || state->result.sample_rate == 0U) {
    return;
  }

  sample_rate = state->result.sample_rate;
  while (samples >= MPEGA_TIME_UPDATE_CHUNK_SAMPLES) {
    state->time_ms_accum += MPEGA_TIME_UPDATE_CHUNK_MS;
    samples -= MPEGA_TIME_UPDATE_CHUNK_SAMPLES;
    while (state->time_ms_accum >= sample_rate) {
      state->time_ms++;
      state->time_ms_accum -= sample_rate;
    }
  }
  for (i = 0; i < samples; i++) {
    state->time_ms_accum += 1000U;
    while (state->time_ms_accum >= sample_rate) {
      state->time_ms++;
      state->time_ms_accum -= sample_rate;
    }
  }
}

static int mpega_shared_copy_to_bytes(ZZ9KSharedBuffer *buffer,
                                      uint32_t offset, const uint8_t *src,
                                      uint32_t length)
{
  volatile uint8_t *dst;
  uint32_t i;

  if (!buffer || buffer->handle == ZZ9K_INVALID_HANDLE || !buffer->data ||
      offset > buffer->length || length > (buffer->length - offset) ||
      (length != 0U && !src)) {
    return 0;
  }

  dst = (volatile uint8_t *)buffer->data + offset;
  for (i = 0; i < length; i++) {
    dst[i] = src[i];
  }

  return 1;
}

/* The fields of our MPEGA_STREAM follow the software library's stream. */
static void mpega_fallback_mirror(MPEGACompatStream *state)
{
  void *handle = state->stream.handle;

  state->stream = *state->fb_stream;
  state->stream.handle = handle;
}

/*
 * Hands a stream the card cannot serve to the software library. The card
 * resources and the source this library opened are released first, so that
 * the software library opens the stream by itself (a bs_access hook sees a
 * CLOSE for each OPEN).
 */
static int mpega_open_fallback(MPEGACompatStream *state, char *filename,
                               MPEGA_CTRL *ctrl)
{
  struct Library *lib;
  MPEGA_STREAM *stream;

  mpega_stream_end_session(state);
  mpega_stream_close_source(state);
  if (state->input_buffer) {
    FreeMem(state->input_buffer, MPEGA_STREAM_CHUNK_BYTES);
    state->input_buffer = 0;
  }

  lib = mpega_fallback_acquire(&state->base->fallback);
  if (!lib) {
    return 0;
  }
  stream = mpega_fb_open(lib, filename, ctrl);
  if (!stream) {
    return 0;
  }

  state->fb_lib = lib;
  state->fb_stream = stream;
  mpega_fallback_stream_opened(&state->base->fallback);
  mpega_fallback_mirror(state);
  return 1;
}

static MPEGA_STREAM *mpega_call_open(REG(a6, struct MPEGACompatBase *base),
                                    REG(a0, char *filename),
                                    REG(a1, MPEGA_CTRL *ctrl))
{
  MPEGACompatStream *state;

  /* A custom bitstream hook owns stream access and may not need a name.
   * RiVA relies on this original mpega.library behaviour for system streams. */
  if (!filename && (!ctrl || !ctrl->bs_access)) {
    return 0;
  }

  state = (MPEGACompatStream *)AllocMem(sizeof(*state), MEMF_PUBLIC | MEMF_CLEAR);
  if (!state) {
    return 0;
  }

  mpega_stream_set_defaults(state);
  if (ctrl) {
    state->ctrl = *ctrl;
    if (state->ctrl.stream_buffer_size <= 0) {
      state->ctrl.stream_buffer_size = MPEGA_DEFAULT_STREAM_BUFFER_BYTES;
    }
  } else {
    mpega_default_ctrl(&state->ctrl);
  }
  state->input_chunk_bytes = MPEGA_STREAM_CHUNK_BYTES;
  if (state->ctrl.bs_access) {
    state->input_chunk_bytes = mpega_normalize_stream_buffer_size(state->ctrl.stream_buffer_size);
  }

  state->base = base;
  state->input_buffer = (uint8_t *)AllocMem(MPEGA_STREAM_CHUNK_BYTES,
                                            MEMF_PUBLIC);
  if (!state->input_buffer || !mpega_stream_open_source(state, filename) ||
      !mpega_open_probe_metadata(state)) {
    mpega_stream_free(state);
    return 0;
  }

  /* The stream is MPEG audio. If the card cannot serve it (no zz9k.library
   * or MP3 stream service, no shared memory, or the card refuses the
   * stream), the software mpega.library gets it. */
  if (!mpega_stream_begin(state) &&
      !mpega_open_fallback(state, filename, ctrl)) {
    mpega_stream_free(state);
    return 0;
  }

  return &state->stream;
}

static void mpega_call_close(REG(a6, struct MPEGACompatBase *base),
                             REG(a0, MPEGA_STREAM *mpds))
{
  (void)base;
  if (mpds) {
    mpega_stream_free((MPEGACompatStream *)mpds->handle);
  }
}

static LONG mpega_call_decode_frame(REG(a6, struct MPEGACompatBase *base),
                                    REG(a0, MPEGA_STREAM *mpds),
                                    REG(a1, WORD **pcm))
{
  MPEGACompatStream *state;
  uint32_t clipped;
  LONG status;

  (void)base;
  if (!mpds || !mpds->handle) {
    return MPEGA_ERR_EOF;
  }

  state = (MPEGACompatStream *)mpds->handle;
  if (state->fb_stream) {
    status = mpega_fb_decode_frame(state->fb_lib, state->fb_stream, pcm);
    mpega_fallback_mirror(state);
    return status;
  }

  status = mpega_stream_feed_until_pcm(state);
  if (status == MPEGA_FEED_NO_PCM) {
    return 0;
  }
  if (status != MPEGA_ERR_NONE) {
    return status;
  }

  if (!pcm || !pcm[0]) {
    return MPEGA_ERR_BADVALUE;
  }
  clipped = 0U;
  return mpega_take_frame(state, (int16_t *)pcm[0], (int16_t *)pcm[1],
                          &clipped);
}

static LONG mpega_call_seek(REG(a6, struct MPEGACompatBase *base),
                            REG(a0, MPEGA_STREAM *mpds),
                            REG(d0, ULONG ms_time_position))
{
  MPEGACompatStream *state;
  LONG byte_pos;
  LONG status;

  (void)base;
  if (!mpds || !mpds->handle) {
    return MPEGA_ERR_EOF;
  }

  state = (MPEGACompatStream *)mpds->handle;
  if (state->fb_stream) {
    status = mpega_fb_seek(state->fb_lib, state->fb_stream, ms_time_position);
    mpega_fallback_mirror(state);
    return status;
  }

  byte_pos = mpega_stream_seek_byte_for_ms(state, ms_time_position);
  if (byte_pos < 0) {
    return MPEGA_ERR_EOF;
  }
  if (!mpega_stream_restart_at_byte(state, byte_pos, ms_time_position)) {
    return MPEGA_ERR_BADFRAME;
  }

  return MPEGA_ERR_NONE;
}

static LONG mpega_call_time(REG(a6, struct MPEGACompatBase *base),
                            REG(a0, MPEGA_STREAM *mpds),
                            REG(a1, ULONG *ms_time_position))
{
  MPEGACompatStream *state;
  LONG status;

  (void)base;
  if (!mpds) {
    return MPEGA_ERR_EOF;
  }
  if (!ms_time_position) {
    return MPEGA_ERR_BADVALUE;
  }

  state = (MPEGACompatStream *)mpds->handle;
  if (!state) {
    return MPEGA_ERR_EOF;
  }

  if (state->fb_stream) {
    status = mpega_fb_time(state->fb_lib, state->fb_stream, ms_time_position);
    mpega_fallback_mirror(state);
    return status;
  }

  mpega_update_stream_info(state);
  *ms_time_position = state->time_ms;
  return MPEGA_ERR_NONE;
}

static int mpega_is_sync_header(const UBYTE *bytes)
{
  UBYTE layer;
  UBYTE bitrate;
  UBYTE sample_rate;

  if (bytes[0] != 0xffU || (bytes[1] & 0xe0U) != 0xe0U) {
    return 0;
  }

  layer = (UBYTE)((bytes[1] >> 1) & 0x03U);
  bitrate = (UBYTE)((bytes[2] >> 4) & 0x0fU);
  sample_rate = (UBYTE)((bytes[2] >> 2) & 0x03U);

  return layer != 0U && bitrate != 0U && bitrate != 0x0fU &&
         sample_rate != 3U;
}

static LONG mpega_call_find_sync(REG(a6, struct MPEGACompatBase *base),
                                 REG(a0, BYTE *buffer),
                                 REG(d0, LONG buffer_size))
{
  const UBYTE *bytes;
  LONG i;

  (void)base;
  if (!buffer || buffer_size < 4) {
    return MPEGA_ERR_NO_SYNC;
  }

  bytes = (const UBYTE *)buffer;
  for (i = 0; i <= buffer_size - 4; i++) {
    if (mpega_is_sync_header(bytes + i)) {
      return i;
    }
  }

  return MPEGA_ERR_NO_SYNC;
}

static LONG mpega_call_scale(REG(a6, struct MPEGACompatBase *base),
                             REG(a0, MPEGA_STREAM *mpds),
                             REG(d0, LONG scale_percent))
{
  MPEGACompatStream *state;
  LONG status;

  (void)base;
  if (!mpds || !mpds->handle) {
    return MPEGA_ERR_BADVALUE;
  }

  state = (MPEGACompatStream *)mpds->handle;
  if (state->fb_stream) {
    /* The software library applies the scale and has its own range. */
    status = mpega_fb_scale(state->fb_lib, state->fb_stream, scale_percent);
    if (status == MPEGA_ERR_NONE) {
      state->scale_percent = scale_percent;
    }
    return status;
  }
  if (scale_percent <= 0 || scale_percent > 10000) {
    return MPEGA_ERR_BADVALUE;
  }

  state->scale_percent = scale_percent;
  return MPEGA_ERR_NONE;
}

static LONG mpega_call_decode_to(REG(a6, struct MPEGACompatBase *base),
                                 REG(a0, MPEGA_STREAM *mpds),
                                 REG(a1, struct MpgaOut *out))
{
  MPEGACompatStream *state;
  WORD *fb_pcm[MPEGA_MAX_CHANNELS];
  uint32_t channels;
  uint32_t clipped;
  LONG status;

  (void)base;
  if (!mpds || !mpds->handle || !out) {
    return MPEGA_ERR_BADVALUE;
  }

  state = (MPEGACompatStream *)mpds->handle;
  clipped = 0U;
  if (state->fb_stream) {
    /* The software library decodes into our buffers; they are converted
     * below. It has applied the scale already, so no clip is counted. */
    channels = (uint32_t)state->stream.dec_channels;
    if (channels == 0U || channels > MPEGA_MAX_CHANNELS) {
      channels = MPEGA_MAX_CHANNELS;
    }
    if (!mpega_out_valid(out, channels)) {
      return MPEGA_ERR_BADVALUE;
    }
    fb_pcm[0] = (WORD *)state->chan[0];
    fb_pcm[1] = (WORD *)state->chan[1];
    status = mpega_fb_decode_frame(state->fb_lib, state->fb_stream, fb_pcm);
    mpega_fallback_mirror(state);
    if (status <= 0) {
      return status;
    }
  } else {
    status = mpega_stream_feed_until_pcm(state);
    if (status == MPEGA_FEED_NO_PCM) {
      return 0;
    }
    if (status != MPEGA_ERR_NONE) {
      return status;
    }
    /* Checked against the frame's layout before it is taken, so a bad
     * buffer set leaves the frame in the ring. */
    channels = mpega_output_channel_count(state);
    if (!mpega_out_valid(out, channels)) {
      return MPEGA_ERR_BADVALUE;
    }
    status = mpega_take_frame(state, state->chan[0], state->chan[1],
                              &clipped);
    if (status <= 0) {
      return status;
    }
  }

  if (status > MPEGA_PCM_SIZE) {
    status = MPEGA_PCM_SIZE;
  }
  mpega_pcm_write_out(out, state->chan[0], state->chan[1], (uint32_t)status,
                      channels, &state->dither);
  out->mo_Clipped += clipped;
  return status;
}

/* MPGA_decode_to shares ambmpega.library's struct and vector slot, so a
 * change to either breaks programs built for that interface. */
#ifdef __m68k__
_Static_assert(sizeof(struct MpgaOut) == 36U, "struct MpgaOut size");
_Static_assert(offsetof(struct MpgaOut, mo_Buffer) == 4U, "mo_Buffer offset");
_Static_assert(offsetof(struct MpgaOut, mo_Mask) == 20U, "mo_Mask offset");
_Static_assert(offsetof(struct MpgaOut, mo_Index) == 24U, "mo_Index offset");
_Static_assert(offsetof(struct MpgaOut, mo_Peak) == 28U, "mo_Peak offset");
_Static_assert(offsetof(struct MpgaOut, mo_Clipped) == 32U,
               "mo_Clipped offset");
_Static_assert(sizeof(mpega_lib_vectors) / sizeof(mpega_lib_vectors[0]) ==
               13U, "MPGA_decode_to is the twelfth vector, offset -72");
#endif
