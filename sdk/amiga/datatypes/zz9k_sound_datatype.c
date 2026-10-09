/*
 * ZZ9000 MP3, native FLAC and Ogg Vorbis sound DataType class.
 *
 * Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "zz9k_sound_mp3.h"
#include "zz9k_sound_flac.h"
#include "zz9k_sound_vorbis.h"

#define ZZ9K_SOUND_SAMPLE_INITIAL_BYTES (64UL * 1024UL)
#define ZZ9K_SOUND_SAMPLE_MAX_BYTES (256UL * 1024UL * 1024UL)
#define ZZ9K_SOUND_INPUT_RING_BYTES (128UL * 1024UL)
#define ZZ9K_SOUND_HOST_BUFFER_BYTES (64UL * 1024UL)
#define ZZ9K_SOUND_HOST_BUFFER_MIN_BYTES (4UL * 1024UL)
/* Consecutive pump iterations without consumed input or produced PCM before
 * the decode is abandoned as stalled. Progress resets the count, so long
 * files on the 4 KiB compact Zorro II window are not bounded by it. */
#define ZZ9K_SOUND_STALL_LIMIT 64U

/* The conversion and cleanup core deliberately has no Amiga dependency.
 * The actual-source host and m68k/vamos harness include this file with
 * ZZ9K_SOUND_DATATYPE_TEST defined. */

typedef struct ZZ9KSoundOwnedState {
  uint32_t session;
  uint32_t input_handle;
  uint32_t pcm_handle;
  uint32_t staging_handle;
  void *sample;
  /* Right channel plane of a modern stereo sample; sample holds the left. */
  void *right_sample;
  uint8_t session_open;
  uint8_t input_allocated;
  uint8_t pcm_allocated;
  uint8_t staging_allocated;
  uint8_t sample_published;
} ZZ9KSoundOwnedState;

typedef struct ZZ9KSoundCleanupOps {
  void (*close_session)(void *ctx, uint32_t session);
  void (*free_shared)(void *ctx, uint32_t handle);
  void (*free_sample)(void *ctx, void *sample);
} ZZ9KSoundCleanupOps;

static int8_t zz9k_sound_legacy_quantize_s16be(const uint8_t *frame,
                                                uint32_t channels)
{
  int32_t value;
  int32_t quantized;

  if (!frame || (channels != 1U && channels != 2U)) {
    return 0;
  }
  value = (int16_t)(((uint16_t)frame[0] << 8) | frame[1]);
  if (channels == 2U) {
    int32_t right = (int16_t)(((uint16_t)frame[2] << 8) | frame[3]);
    value = (value + right) / 2;
  }
  /* Divide in the signed domain, then saturate. This maps -32768 exactly to
   * -128 and +32767 to +127 without negating INT16_MIN or overflowing the
   * stereo sum. */
  quantized = value / 256;
  if (quantized < -128) {
    quantized = -128;
  } else if (quantized > 127) {
    quantized = 127;
  }
  return (int8_t)quantized;
}

/* Packs the most significant 16 bits of each channel of one big-endian PCM
 * frame (2-byte S16BE or 4-byte MSB-justified S32BE samples) into an S16BE
 * frame for the legacy quantizer. */
static void zz9k_sound_frame_top16(const uint8_t *frame, uint32_t channels,
                                   uint32_t sample_bytes, uint8_t *out)
{
  uint32_t c;
  for (c = 0U; c < channels; c++) {
    out[c * 2U] = frame[c * sample_bytes];
    out[c * 2U + 1U] = frame[c * sample_bytes + 1U];
  }
}

static int zz9k_sound_next_capacity(uint32_t current, uint32_t required,
                                    uint32_t limit, uint32_t *next)
{
  uint32_t candidate;

  if (!next || required > limit) {
    return 0;
  }
  candidate = current ? current : ZZ9K_SOUND_SAMPLE_INITIAL_BYTES;
  if (candidate > limit) {
    candidate = limit;
  }
  while (candidate < required) {
    if (candidate > limit / 2U) {
      candidate = limit;
    } else {
      candidate *= 2U;
    }
    if (candidate < required && candidate == limit) {
      return 0;
    }
  }
  *next = candidate;
  return 1;
}

static void zz9k_sound_cleanup_owned(ZZ9KSoundOwnedState *state,
                                     const ZZ9KSoundCleanupOps *ops,
                                     void *ctx)
{
  if (!state || !ops) {
    return;
  }
  if (state->session_open) {
    if (ops->close_session) {
      ops->close_session(ctx, state->session);
    }
    state->session_open = 0U;
  }
  if (state->staging_allocated) {
    if (ops->free_shared) {
      ops->free_shared(ctx, state->staging_handle);
    }
    state->staging_allocated = 0U;
  }
  if (state->pcm_allocated) {
    if (ops->free_shared) {
      ops->free_shared(ctx, state->pcm_handle);
    }
    state->pcm_allocated = 0U;
  }
  if (state->input_allocated) {
    if (ops->free_shared) {
      ops->free_shared(ctx, state->input_handle);
    }
    state->input_allocated = 0U;
  }
  if (!state->sample_published) {
    if (state->sample && ops->free_sample) {
      ops->free_sample(ctx, state->sample);
    }
    if (state->right_sample && ops->free_sample) {
      ops->free_sample(ctx, state->right_sample);
    }
    state->sample = 0;
    state->right_sample = 0;
  }
}

#ifndef ZZ9K_SOUND_DATATYPE_TEST

#include "zz9k/audio.h"
#include "zz9k/caps.h"
#include "zz9k/shared.h"
#include <SDI_compiler.h>
#include <clib/alib_protos.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/soundclass.h>
#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/resident.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <proto/datatypes.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/zz9k.h>
#include <utility/tagitem.h>

#define ZZ9K_SOUND_DATATYPE_NAME "zz9k-sound.datatype"
#define ZZ9K_SOUND_DATATYPE_VERSION 42
#define ZZ9K_SOUND_DATATYPE_REVISION 1
#define ZZ9K_SOUND_DATATYPE_ID_STRING \
  "$VER: zz9k-sound.datatype 42.1 (9.10.2026) ZZ9000 SDK"
/* SDTA_BitsPerSample is a V47 sound.datatype attribute. Earlier V44-V46
 * classes have no proven high-precision contract (KTD4) and take the legacy
 * 8-bit mono path. */
#define ZZ9K_SOUND_MODERN_VERSION 47
#define ZZ9K_SOUND_LIBRARY_NAME "zz9k.library"
#define ZZ9K_SOUND_LIBRARY_VERSION 2

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
/* MakeClass/AddClass/RemoveClass/FreeClass are intuition.library calls; a
 * -nostartfiles resident gets no auto-opened base. */
struct IntuitionBase *IntuitionBase;
struct Library *DataTypesBase;
struct Library *SoundBase;
struct Library *UtilityBase;
struct Library *ZZ9KBase;

typedef struct ZZ9KSoundDatatypeBase {
  struct ClassLibrary class_library;
  BPTR segment;
  UBYTE class_added;
  UBYTE modern;
} ZZ9KSoundDatatypeBase;

/* What the source needs from the audio stream service. */
typedef struct ZZ9KSoundSource {
  uint32_t codec;          /* ZZ9K_AUDIO_CODEC_MP3, _FLAC or _VORBIS */
  uint32_t first_byte;     /* first byte fed to the decoder */
  uint32_t end_byte;       /* one past the last byte fed */
  uint32_t sample_rate;    /* rate the stream must report */
  uint32_t channels;       /* channel count the stream must report */
  uint32_t pcm_format;     /* S16BE, or S32BE for 17..24-bit FLAC */
  /* Largest PCM unit the decoder writes at once (an MP3 frame, a FLAC
   * block, a Vorbis packet); the PCM ring must hold it whole. */
  uint32_t min_pcm_bytes;
} ZZ9KSoundSource;

typedef struct ZZ9KSoundDecode {
  ZZ9KSoundOwnedState owned;
  ZZ9KSharedBuffer input_ring;
  ZZ9KSharedBuffer pcm_ring;
  ZZ9KSharedBuffer staging;
  ZZ9KAudioStreamResult result;
  uint8_t *sample;
  uint32_t sample_bytes;
  uint32_t sample_capacity;
  /* Modern stereo right plane; same byte count as sample. */
  uint8_t *right;
  uint32_t right_capacity;
  uint32_t pcm_seen;
  uint32_t pcm_offset;
  uint32_t pending_ack;
  uint32_t source_channels;
  uint32_t sample_rate;
  /* Geometry the probe promised; a stream that reports anything else is
   * rejected rather than published with mismatched planes. */
  uint32_t expect_rate;
  uint32_t expect_channels;
  /* Requested PCM container and its bytes per channel sample (2 or 4). */
  uint32_t pcm_format;
  uint32_t sample_bytes_in;
  uint8_t modern;
  /* DOS error for a resource failure; 0 means a stream or content failure
   * reported as DTERROR_INVALID_DATA. */
  LONG error;
} ZZ9KSoundDecode;

static ZZ9KSoundDatatypeBase *zz9k_sound_datatype_open(
    REG(a6, ZZ9KSoundDatatypeBase *base));
static BPTR zz9k_sound_datatype_close(REG(a6, ZZ9KSoundDatatypeBase *base));
static BPTR zz9k_sound_datatype_expunge(REG(a6, ZZ9KSoundDatatypeBase *base));
static ULONG zz9k_sound_datatype_null(void);
static Class *zz9k_sound_datatype_get_class(REG(a6, ZZ9KSoundDatatypeBase *base));
static ZZ9KSoundDatatypeBase *zz9k_sound_datatype_init(REG(a0, BPTR segment));
static ULONG zz9k_sound_datatype_dispatch(REG(a0, struct Hook *hook),
                                          REG(a2, Object *object),
                                          REG(a1, Msg msg));

static const APTR zz9k_sound_datatype_vectors[] = {
  (APTR)zz9k_sound_datatype_open,
  (APTR)zz9k_sound_datatype_close,
  (APTR)zz9k_sound_datatype_expunge,
  (APTR)zz9k_sound_datatype_null,
  (APTR)zz9k_sound_datatype_get_class,
  (APTR)-1
};

static const struct Resident zz9k_sound_datatype_romtag
    __attribute__((used)) = {
  RTC_MATCHWORD,
  (struct Resident *)&zz9k_sound_datatype_romtag,
  (APTR)(&zz9k_sound_datatype_romtag + 1),
  0,
  ZZ9K_SOUND_DATATYPE_VERSION,
  NT_LIBRARY,
  0,
  (char *)ZZ9K_SOUND_DATATYPE_NAME,
  (char *)ZZ9K_SOUND_DATATYPE_ID_STRING,
  (APTR)zz9k_sound_datatype_init
};

/* AmigaDOS Seek returns the position before the move: seek to the end, and
 * the seek back to the original position returns the file length. */
static int zz9k_sound_source_size(BPTR file, uint32_t *size)
{
  LONG old;
  LONG end;
  if (!file || !size) {
    return 0;
  }
  old = Seek(file, 0, OFFSET_END);
  if (old < 0) {
    return 0;
  }
  end = Seek(file, old, OFFSET_BEGINNING);
  if (end < 0) {
    return 0;
  }
  *size = (uint32_t)end;
  return 1;
}

static int zz9k_sound_read_at(BPTR file, uint32_t offset, void *dst,
                              uint32_t bytes)
{
  if (!file || !dst || bytes > 0x7fffffffUL || offset > 0x7fffffffUL ||
      Seek(file, (LONG)offset, OFFSET_BEGINNING) < 0) {
    return 0;
  }
  return Read(file, dst, (LONG)bytes) == (LONG)bytes;
}

static int zz9k_sound_probe_mp3(BPTR file, uint32_t file_size,
                                ZZ9KSoundSource *source)
{
  ZZ9KSoundMp3Envelope envelope;
  uint8_t header[10];
  uint8_t *probe;
  uint32_t start;
  uint32_t probe_bytes;
  uint32_t end;
  int ok;

  if (!zz9k_sound_read_at(file, 0U, header, sizeof(header)) ||
      !zz9k_sound_audio_start(header, sizeof(header), &start) ||
      start >= file_size) {
    return 0;
  }
  end = file_size;
  if (end >= 128U && zz9k_sound_read_at(file, end - 128U, header, 3U) &&
      header[0] == 'T' && header[1] == 'A' && header[2] == 'G') {
    end -= 128U;
  }
  if (start >= end) {
    return 0;
  }
  probe_bytes = end - start;
  if (probe_bytes > ZZ9K_SOUND_SYNC_SCAN_BYTES) {
    probe_bytes = ZZ9K_SOUND_SYNC_SCAN_BYTES;
  }
  probe = (uint8_t *)AllocVec(probe_bytes, MEMF_PUBLIC);
  if (!probe) {
    return 0;
  }
  ok = zz9k_sound_read_at(file, start, probe, probe_bytes) &&
       zz9k_sound_recognize_mp3(probe, probe_bytes, &envelope);
  FreeVec(probe);
  if (ok) {
    source->codec = ZZ9K_AUDIO_CODEC_MP3;
    source->first_byte = start + envelope.first_frame;
    source->end_byte = end;
    source->sample_rate = envelope.sample_rate;
    source->channels = envelope.channels;
    source->pcm_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE;
    /* One 1152-sample stereo S16 frame: the firmware's MP3 ring minimum. */
    source->min_pcm_bytes = 1152U * 2U * 2U;
  }
  return ok;
}

/* Native FLAC is fed whole from the marker: the firmware decoder parses its
 * own metadata. Sources wider than 16 bits use the MSB-justified S32BE
 * container so the modern path keeps their full precision. */
static int zz9k_sound_probe_flac(BPTR file, uint32_t file_size,
                                 ZZ9KSoundSource *source)
{
  ZZ9KSoundFlacEnvelope envelope;
  uint8_t header[ZZ9K_SOUND_FLAC_HEADER_BYTES];

  if (file_size < sizeof(header) ||
      !zz9k_sound_read_at(file, 0U, header, sizeof(header)) ||
      !zz9k_sound_recognize_flac(header, sizeof(header), &envelope)) {
    return 0;
  }
  source->codec = ZZ9K_AUDIO_CODEC_FLAC;
  source->first_byte = 0U;
  source->end_byte = file_size;
  source->sample_rate = envelope.sample_rate;
  source->channels = envelope.channels;
  source->pcm_format = envelope.bits_per_sample > 16U ?
      ZZ9K_AUDIO_SAMPLE_FORMAT_S32BE : ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE;
  source->min_pcm_bytes = envelope.max_block_size * envelope.channels *
      (envelope.bits_per_sample > 16U ? 4U : 2U);
  return 1;
}

/* Ogg Vorbis is fed whole from its identification page; the firmware
 * decoder rejects chained and multiplexed streams. Vorbis decodes to S16. */
static int zz9k_sound_probe_vorbis(BPTR file, uint32_t file_size,
                                   ZZ9KSoundSource *source)
{
  ZZ9KSoundVorbisEnvelope envelope;
  uint8_t header[ZZ9K_SOUND_VORBIS_HEADER_BYTES];

  if (file_size < sizeof(header) ||
      !zz9k_sound_read_at(file, 0U, header, sizeof(header)) ||
      !zz9k_sound_recognize_vorbis(header, sizeof(header), &envelope)) {
    return 0;
  }
  source->codec = ZZ9K_AUDIO_CODEC_VORBIS;
  source->first_byte = 0U;
  source->end_byte = file_size;
  source->sample_rate = envelope.sample_rate;
  source->channels = envelope.channels;
  source->pcm_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE;
  /* Overlap-add emits at most half the long block per packet. */
  source->min_pcm_bytes = envelope.max_block_samples / 2U *
      envelope.channels * 2U;
  return 1;
}

static int zz9k_sound_probe_file(BPTR file, uint32_t file_size,
                                 ZZ9KSoundSource *source)
{
  if (!file || !source || file_size < 8U) {
    return 0;
  }
  return zz9k_sound_probe_flac(file, file_size, source) ||
         zz9k_sound_probe_vorbis(file, file_size, source) ||
         zz9k_sound_probe_mp3(file, file_size, source);
}

/* The PCM ring never drops below the decoder's largest output unit; only
 * the staging half keeps shrinking toward the floor when the host-visible
 * heap is compact. */
static int zz9k_sound_alloc_host_pair(ZZ9KSoundDecode *decode,
                                      uint32_t min_pcm)
{
  uint32_t budget;
  if (!decode) {
    return 0;
  }
  min_pcm = (min_pcm + 15UL) & ~15UL;
  for (budget = ZZ9K_SOUND_HOST_BUFFER_BYTES;
       budget >= ZZ9K_SOUND_HOST_BUFFER_MIN_BYTES; budget /= 2U) {
    uint32_t pcm_bytes = budget > min_pcm ? budget : min_pcm;

    if (ZZ9KAllocShared(pcm_bytes, 16U, ZZ9K_ALLOC_HOST_WINDOW,
                        &decode->pcm_ring) == ZZ9K_STATUS_OK) {
      decode->owned.pcm_allocated = 1U;
      decode->owned.pcm_handle = decode->pcm_ring.handle;
      if (ZZ9KAllocShared(budget, 16U, ZZ9K_ALLOC_HOST_WINDOW,
                          &decode->staging) == ZZ9K_STATUS_OK) {
        decode->owned.staging_allocated = 1U;
        decode->owned.staging_handle = decode->staging.handle;
        return 1;
      }
      (void)ZZ9KFreeShared(decode->pcm_ring.handle);
      decode->owned.pcm_allocated = 0U;
    }
  }
  return 0;
}

/* Grows one sample plane to hold additional bytes beyond used. */
static int zz9k_sound_reserve_plane(uint8_t **plane, uint32_t *capacity,
                                    uint32_t used, uint32_t additional,
                                    void **owned)
{
  uint32_t required;
  uint32_t next;
  uint8_t *replacement;
  if (additional > ZZ9K_SOUND_SAMPLE_MAX_BYTES - used) {
    return 0;
  }
  required = used + additional;
  if (required <= *capacity) {
    return 1;
  }
  if (!zz9k_sound_next_capacity(*capacity, required,
                                ZZ9K_SOUND_SAMPLE_MAX_BYTES, &next)) {
    return 0;
  }
  replacement = (uint8_t *)AllocVec(next, MEMF_PUBLIC);
  if (!replacement) {
    return 0;
  }
  if (used != 0U) {
    CopyMem(*plane, replacement, used);
  }
  if (*plane) {
    FreeVec(*plane);
  }
  *plane = replacement;
  *owned = replacement;
  *capacity = next;
  return 1;
}

static uint8_t zz9k_sound_ring_byte(const ZZ9KSoundDecode *decode,
                                    uint32_t offset)
{
  return ((volatile const uint8_t *)decode->pcm_ring.data)[offset];
}

/* Copies newly produced whole PCM frames out of the host ring into the
 * sample. Firmware may reuse a ring range only after the Read that
 * acknowledges it, which zz9k_sound_ack_pcm sends afterwards. */
static int zz9k_sound_copy_pcm(ZZ9KSoundDecode *decode)
{
  uint32_t available;
  uint32_t width;
  uint32_t frame_bytes;
  uint32_t frames;
  uint32_t plane_bytes;
  int stereo;
  uint32_t i;
  uint32_t offset;

  if (!decode || decode->result.bytes_produced < decode->pcm_seen ||
      decode->pcm_offset >= decode->pcm_ring.length) {
    return 0;
  }
  available = decode->result.bytes_produced - decode->pcm_seen;
  if (available == 0U) {
    return 1;
  }
  width = decode->sample_bytes_in;
  if (available > decode->pcm_ring.length ||
      (decode->result.channels != 1U && decode->result.channels != 2U) ||
      decode->result.sample_format != decode->pcm_format ||
      (width != 2U && width != 4U) ||
      (decode->result.sample_rate != 0U &&
       (decode->result.sample_rate != decode->expect_rate ||
        decode->result.channels != decode->expect_channels))) {
    return 0;
  }
  frame_bytes = decode->result.channels * width;
  frames = available / frame_bytes;
  available = frames * frame_bytes;
  /* Modern: big-endian planes of the stream's container width (16-bit, or
   * 32-bit MSB-justified for 17..24-bit sources), the right channel in its
   * own allocation as the V44 SDTA_LeftSample/SDTA_RightSample contract
   * requires. Legacy: one 8-bit mono plane. */
  plane_bytes = decode->modern ? frames * width : frames;
  stereo = decode->modern && decode->result.channels == 2U;
  if (!zz9k_sound_reserve_plane(&decode->sample, &decode->sample_capacity,
                                decode->sample_bytes, plane_bytes,
                                &decode->owned.sample) ||
      (stereo &&
       !zz9k_sound_reserve_plane(&decode->right, &decode->right_capacity,
                                 decode->sample_bytes, plane_bytes,
                                 &decode->owned.right_sample))) {
    decode->error = ERROR_NO_FREE_STORE;
    return 0;
  }
  offset = decode->pcm_offset;
  for (i = 0U; i < frames; i++) {
    uint8_t frame[8];
    uint32_t b;
    for (b = 0U; b < frame_bytes; b++) {
      frame[b] = zz9k_sound_ring_byte(decode, offset);
      if (++offset == decode->pcm_ring.length) {
        offset = 0U;
      }
    }
    if (!decode->modern) {
      uint8_t top[4];
      zz9k_sound_frame_top16(frame, decode->result.channels, width, top);
      decode->sample[decode->sample_bytes++] =
          (uint8_t)zz9k_sound_legacy_quantize_s16be(
              top, decode->result.channels);
      continue;
    }
    memcpy(decode->sample + decode->sample_bytes, frame, width);
    if (stereo) {
      memcpy(decode->right + decode->sample_bytes, frame + width, width);
    }
    decode->sample_bytes += width;
  }
  decode->pcm_offset = offset;
  decode->pcm_seen += available;
  decode->pending_ack += available;
  return 1;
}

/* A resource status from Feed or Read keeps its DOS meaning; anything else
 * is a stream or content failure (DTERROR_INVALID_DATA). */
static LONG zz9k_sound_stream_error(int status)
{
  return status == ZZ9K_STATUS_BUSY ? ERROR_OBJECT_IN_USE :
         status == ZZ9K_STATUS_NO_MEMORY ? ERROR_NO_FREE_STORE : 0;
}

/* Returns copied PCM credit. A forced Read is sent even with no credit: it is
 * the only way a backpressured stream resumes consuming compressed input. */
static int zz9k_sound_ack_pcm(ZZ9KSoundDecode *decode, int force)
{
  int status;

  if (!force && decode->pending_ack < decode->pcm_ring.length / 2U) {
    return 1;
  }
  status = ZZ9KAudioStreamRead(decode->result.session, decode->pending_ack,
                               0U, &decode->result);
  if (status != ZZ9K_STATUS_OK) {
    decode->error = zz9k_sound_stream_error(status);
    return 0;
  }
  decode->pending_ack = 0U;
  decode->pcm_offset = decode->result.pcm_read;
  return 1;
}

/* Copies and acknowledges while each Read reports new PCM. The pass is
 * bounded; callers loop until the stream is drained. */
static int zz9k_sound_drain_pcm(ZZ9KSoundDecode *decode)
{
  uint32_t pass;
  for (pass = 0U; pass < 64U; pass++) {
    uint32_t seen = decode->pcm_seen;
    if (!zz9k_sound_copy_pcm(decode)) {
      return 0;
    }
    if (decode->pcm_seen == seen) {
      return 1;
    }
    if (!zz9k_sound_ack_pcm(decode, 0)) {
      return 0;
    }
  }
  return 1;
}

static void zz9k_sound_real_close(void *ctx, uint32_t session)
{
  ZZ9KAudioStreamResult result;
  (void)ctx;
  memset(&result, 0, sizeof(result));
  (void)ZZ9KAudioStreamClose(session, 0U, &result);
}

static void zz9k_sound_real_free_shared(void *ctx, uint32_t handle)
{
  (void)ctx;
  (void)ZZ9KFreeShared(handle);
}

static void zz9k_sound_real_free_sample(void *ctx, void *sample)
{
  (void)ctx;
  FreeVec(sample);
}

static const ZZ9KSoundCleanupOps zz9k_sound_real_cleanup_ops = {
  zz9k_sound_real_close,
  zz9k_sound_real_free_shared,
  zz9k_sound_real_free_sample
};

/* Opens the stream for the source codec. MP3 keeps the original Begin so it
 * runs on firmware that predates BeginEx; FLAC and Vorbis need the
 * codec-aware BeginEx. */
static int zz9k_sound_begin_stream(const ZZ9KSoundSource *source,
                                   ZZ9KSoundDecode *decode)
{
  if (source->codec == ZZ9K_AUDIO_CODEC_MP3) {
    ZZ9KAudioStreamBeginDesc begin;
    if (!zz9k_audio_build_stream_begin_desc(
            &begin, decode->input_ring.handle, decode->input_ring.length,
            decode->pcm_ring.handle, decode->pcm_ring.length, 0U, 0U,
            source->pcm_format, 0U, 0U, 0U)) {
      return ZZ9K_STATUS_BAD_REQUEST;
    }
    return ZZ9KAudioStreamBegin(&begin, &decode->result);
  } else {
    ZZ9KAudioStreamBeginExDesc begin;
    if (!zz9k_audio_build_stream_begin_ex_desc(
            &begin, source->codec, decode->input_ring.handle,
            decode->input_ring.length, decode->pcm_ring.handle,
            decode->pcm_ring.length, 0U, 0U, source->pcm_format, 0U, 0U,
            0U)) {
      return ZZ9K_STATUS_BAD_REQUEST;
    }
    return ZZ9KAudioStreamBeginEx(&begin, &decode->result);
  }
}

static int zz9k_sound_decode_file(BPTR file, const ZZ9KSoundSource *source,
                                  uint8_t modern, ZZ9KSoundDecode *decode)
{
  uint8_t *input;
  uint32_t position;
  uint32_t total_fed;
  uint32_t guard;
  int ok;
  int status;

  memset(decode, 0, sizeof(*decode));
  decode->modern = modern;
  decode->pcm_format = source->pcm_format;
  decode->sample_bytes_in =
      source->pcm_format == ZZ9K_AUDIO_SAMPLE_FORMAT_S32BE ? 4U : 2U;
  decode->expect_rate = source->sample_rate;
  decode->expect_channels = source->channels;
  decode->owned.input_handle = ZZ9K_INVALID_HANDLE;
  decode->owned.pcm_handle = ZZ9K_INVALID_HANDLE;
  decode->owned.staging_handle = ZZ9K_INVALID_HANDLE;
  input = 0;
  ok = 0;

  /* Resource failures report a resource error rather than invalid data, so
   * a decode competing with an active player fails as busy/out of memory
   * instead of being mistaken for a corrupt file. */
  decode->error = ERROR_NO_FREE_STORE;
  if (ZZ9KAllocShared(ZZ9K_SOUND_INPUT_RING_BYTES, 16U, ZZ9K_ALLOC_CARD_ONLY,
                      &decode->input_ring) != ZZ9K_STATUS_OK) {
    goto done;
  }
  decode->owned.input_allocated = 1U;
  decode->owned.input_handle = decode->input_ring.handle;
  if (!zz9k_sound_alloc_host_pair(decode, source->min_pcm_bytes)) {
    goto done;
  }
  status = zz9k_sound_begin_stream(source, decode);
  if (status != ZZ9K_STATUS_OK) {
    decode->error = status == ZZ9K_STATUS_BUSY ? ERROR_OBJECT_IN_USE :
        status == ZZ9K_STATUS_NO_MEMORY ? ERROR_NO_FREE_STORE :
        ERROR_NOT_IMPLEMENTED;
    goto done;
  }
  decode->owned.session_open = 1U;
  decode->owned.session = decode->result.session;
  decode->pcm_offset = decode->result.pcm_read;
  input = (uint8_t *)AllocVec(decode->staging.length, MEMF_PUBLIC);
  if (!input) {
    goto done;
  }
  /* From here on a failure is a stream or content problem unless the sample
   * buffer cannot grow (zz9k_sound_copy_pcm sets ERROR_NO_FREE_STORE). */
  decode->error = 0;

  /* Same protocol as zz9k-mp3: every feed hands the firmware the whole
   * staged chunk; bytes_consumed is the decoder's cumulative consumption
   * from the input ring. Before feeding, PCM credit is returned until the
   * ring has room; a BACKPRESSURE result is answered with a forced Read and
   * the same chunk is fed again. */
  position = source->first_byte;
  total_fed = 0U;
  for (;;) {
    ZZ9KAudioStreamFeedDesc feed;
    uint32_t chunk = source->end_byte - position;
    uint32_t flags = 0U;

    if (chunk > decode->staging.length) {
      chunk = decode->staging.length;
    }
    if (chunk == 0U) {
      flags = ZZ9K_AUDIO_STREAM_FEED_EOF;
    } else {
      for (guard = 0U;
           decode->result.bytes_consumed < total_fed &&
           total_fed - decode->result.bytes_consumed >
               decode->input_ring.length - chunk;) {
        uint32_t consumed = decode->result.bytes_consumed;
        if (decode->pending_ack == 0U) {
          break;
        }
        if (!zz9k_sound_ack_pcm(decode, 1) || !zz9k_sound_drain_pcm(decode)) {
          goto done;
        }
        guard = decode->result.bytes_consumed == consumed ? guard + 1U : 0U;
        if (guard >= ZZ9K_SOUND_STALL_LIMIT) {
          goto done;
        }
      }
      if (!zz9k_sound_read_at(file, position, input, chunk) ||
          !zz9k_shared_copy_to(&decode->staging, 0U, input, chunk)) {
        goto done;
      }
    }
    for (guard = 0U;;) {
      uint32_t seen = decode->pcm_seen;
      uint32_t consumed = decode->result.bytes_consumed;
      if (!zz9k_audio_build_stream_feed_desc(
              &feed, decode->result.session, decode->staging.handle, 0U,
              chunk, flags)) {
        goto done;
      }
      status = ZZ9KAudioStreamFeed(&feed, &decode->result);
      if (status != ZZ9K_STATUS_OK) {
        decode->error = zz9k_sound_stream_error(status);
        goto done;
      }
      if (!zz9k_sound_drain_pcm(decode)) {
        goto done;
      }
      if ((decode->result.flags & ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) ==
          0U) {
        break;
      }
      if (!zz9k_sound_ack_pcm(decode, 1)) {
        goto done;
      }
      guard = decode->pcm_seen == seen &&
              decode->result.bytes_consumed == consumed ? guard + 1U : 0U;
      if (guard >= ZZ9K_SOUND_STALL_LIMIT) {
        goto done;
      }
    }
    if (decode->result.sample_rate != 0U) {
      decode->sample_rate = decode->result.sample_rate;
      decode->source_channels = decode->result.channels;
    }
    if (flags != 0U) {
      break;
    }
    position += chunk;
    total_fed += chunk;
  }
  /* Flush: copy and acknowledge until a forced Read yields no new PCM. */
  for (guard = 0U;;) {
    uint32_t seen = decode->pcm_seen;
    if (!zz9k_sound_drain_pcm(decode)) {
      goto done;
    }
    /* A trailing partial frame is never published. */
    if (decode->pending_ack == 0U &&
        decode->result.bytes_produced - decode->pcm_seen <
            decode->result.channels * decode->sample_bytes_in) {
      break;
    }
    if (!zz9k_sound_ack_pcm(decode, 1)) {
      goto done;
    }
    guard = decode->pcm_seen == seen ? guard + 1U : 0U;
    if (guard >= ZZ9K_SOUND_STALL_LIMIT) {
      goto done;
    }
  }
  ok = decode->sample_rate != 0U &&
       (decode->source_channels == 1U || decode->source_channels == 2U) &&
       decode->sample_bytes != 0U;

done:
  if (input) {
    FreeVec(input);
  }
  /* Success still closes and frees every firmware resource before the caller
   * may publish the AllocVec sample to sound.datatype. */
  if (ok) {
    /* Retain only the host AllocVec sample planes. The temporary published
     * marker keeps generic cleanup from freeing them; the caller still has
     * not handed them to the superclass. */
    decode->owned.sample_published = 1U;
    zz9k_sound_cleanup_owned(&decode->owned, &zz9k_sound_real_cleanup_ops, 0);
    decode->owned.sample_published = 0U;
  } else {
    zz9k_sound_cleanup_owned(&decode->owned, &zz9k_sound_real_cleanup_ops, 0);
    decode->sample = 0;
    decode->right = 0;
  }
  return ok;
}

/* Fills the superclass creation tags at OM_NEW. The modern (sound.datatype
 * V47) path publishes planes of the stream's container width (16 bits, or
 * 32 bits for 17..24-bit FLAC) with SDTA_BitsPerSample and
 * SDTA_SamplesPerSec and no period, stereo as V44 SDTA_LeftSample/
 * SDTA_RightSample. SDTA_SampleLength is the byte length of one channel
 * plane, matching the system's own 16-bit WAVE loader; for the legacy 8-bit
 * path bytes equal samples. The legacy path has no rate attribute and gets a
 * period from the system colour clock (5 x the E clock: PAL 3546895, NTSC
 * 3579545). */
static void zz9k_sound_sample_tags(const ZZ9KSoundDecode *decode,
                                   uint8_t modern, struct TagItem *tags,
                                   struct TagItem *more)
{
  int n = 0;

  if (decode->right) {
    tags[n].ti_Tag = SDTA_LeftSample;
    tags[n++].ti_Data = (ULONG)decode->sample;
    tags[n].ti_Tag = SDTA_RightSample;
    tags[n++].ti_Data = (ULONG)decode->right;
  } else {
    tags[n].ti_Tag = SDTA_Sample;
    tags[n++].ti_Data = (ULONG)decode->sample;
  }
  tags[n].ti_Tag = SDTA_SampleLength;
  tags[n++].ti_Data = decode->sample_bytes;
  tags[n].ti_Tag = SDTA_Cycles;
  tags[n++].ti_Data = 1U;
  tags[n].ti_Tag = SDTA_FreeSampleData;
  tags[n++].ti_Data = TRUE;
  if (modern) {
    tags[n].ti_Tag = SDTA_BitsPerSample;
    tags[n++].ti_Data = decode->sample_bytes_in * 8U;
    tags[n].ti_Tag = SDTA_SamplesPerSec;
    tags[n++].ti_Data = decode->sample_rate;
  } else {
    tags[n].ti_Tag = SDTA_Period;
    tags[n++].ti_Data = (SysBase->ex_EClockFrequency * 5UL +
                         decode->sample_rate / 2U) / decode->sample_rate;
  }
  tags[n].ti_Tag = SDTA_Volume;
  tags[n++].ti_Data = 64U;
  tags[n].ti_Tag = TAG_MORE;
  tags[n].ti_Data = (ULONG)more;
}

/* Returns 0 when the card can decode the source, else the DOS error. */
static LONG zz9k_sound_service_error(const ZZ9KSoundSource *source)
{
  ZZ9KServiceInfo service;
  ULONG required;
  UWORD revision;

  if (source->codec == ZZ9K_AUDIO_CODEC_MP3) {
    revision = ZZ9K_LIBRARY_MIN_REVISION_ALLOC_FLAGS;
    required = ZZ9K_SERVICE_FLAG_AUDIO_MP3_DECODE |
               ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM |
               ZZ9K_SERVICE_FLAG_AUDIO_PCM16_STEREO;
  } else {
    revision = ZZ9K_LIBRARY_MIN_REVISION_AUDIO_STREAM_EX;
    required = source->codec == ZZ9K_AUDIO_CODEC_FLAC ?
        ZZ9K_SERVICE_FLAG_AUDIO_FLAC_STREAM :
        ZZ9K_SERVICE_FLAG_AUDIO_VORBIS_STREAM;
  }
  if (!ZZ9KBase || ZZ9KBase->lib_Revision < revision ||
      ZZ9KQueryService(ZZ9K_SERVICE_AUDIO, &service) != ZZ9K_STATUS_OK ||
      (service.flags & required) != required) {
    return ERROR_NOT_IMPLEMENTED;
  }
  return 0;
}

/* Decodes the whole file named by OM_NEW before the superclass constructs the
 * object. On success decode->sample is an unpublished AllocVec buffer. */
static int zz9k_sound_decode_source(ZZ9KSoundDatatypeBase *base,
                                    struct TagItem *attrs,
                                    ZZ9KSoundDecode *decode)
{
  ZZ9KSoundSource source;
  CONST_STRPTR name;
  BPTR file;
  uint32_t file_size;
  LONG error;
  int ok;

  /* Memory sources are refused: both probed superclass stacks reject their
   * NewDTObject memory flow. Before the superclass constructs the object,
   * DTA_Handle carries datatypes.library's lock, not a file handle, so the
   * class opens its own handle by name. */
  name = (CONST_STRPTR)GetTagData(DTA_Name, 0, attrs);
  file = GetTagData(DTA_SourceType, DTST_FILE, attrs) == DTST_FILE && name ?
      Open(name, MODE_OLDFILE) : 0;
  ok = file && zz9k_sound_source_size(file, &file_size) &&
       zz9k_sound_probe_file(file, file_size, &source);
  if (!ok) {
    if (file) {
      Close(file);
    }
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }
  error = zz9k_sound_service_error(&source);
  if (error != 0) {
    Close(file);
    SetIoErr(error);
    return 0;
  }
  ok = zz9k_sound_decode_file(file, &source, base->modern, decode);
  Close(file);
  if (!ok || decode->sample_rate != source.sample_rate ||
      decode->source_channels != source.channels) {
    zz9k_sound_cleanup_owned(&decode->owned, &zz9k_sound_real_cleanup_ops, 0);
    SetIoErr(decode->error ? decode->error : DTERROR_INVALID_DATA);
    return 0;
  }
  return 1;
}

/* datatypes.library hands its DTST_FILE lock to the class chain as
 * DTA_Handle and forgets it once OM_NEW is called, whether or not OM_NEW
 * succeeds; datatypesclass takes it over only when the superclass runs.
 * Refusing a file before that point must therefore release the lock, or
 * every refusal leaks a FileLock. The class's IoErr is preserved. */
static void zz9k_sound_release_source_lock(struct TagItem *attrs)
{
  BPTR lock;
  LONG error;

  if (GetTagData(DTA_SourceType, DTST_FILE, attrs) != DTST_FILE) {
    return;
  }
  lock = (BPTR)GetTagData(DTA_Handle, 0, attrs);
  if (lock) {
    error = IoErr();
    UnLock(lock);
    SetIoErr(error);
  }
}

static ULONG zz9k_sound_datatype_dispatch(REG(a0, struct Hook *hook),
                                          REG(a2, Object *object),
                                          REG(a1, Msg msg))
{
  Class *cl;
  if (!hook || !msg) {
    return 0;
  }
  cl = (Class *)hook;
  if (msg->MethodID == OM_NEW) {
    struct opSet *ops = (struct opSet *)msg;
    struct opSet super_msg;
    struct TagItem tags[9];
    ZZ9KSoundDecode decode;
    ZZ9KSoundDatatypeBase *base = (ZZ9KSoundDatatypeBase *)cl->cl_UserData;
    ULONG result;

    if (!zz9k_sound_decode_source(base, ops->ops_AttrList, &decode)) {
      zz9k_sound_release_source_lock(ops->ops_AttrList);
      return 0;
    }
    zz9k_sound_sample_tags(&decode, base->modern, tags,
                           ops->ops_AttrList);
    super_msg = *ops;
    super_msg.ops_AttrList = tags;
    result = DoSuperMethodA(cl, object, (Msg)&super_msg);
    if (!result) {
      /* Not published: the superclass never took ownership. */
      zz9k_sound_cleanup_owned(&decode.owned, &zz9k_sound_real_cleanup_ops, 0);
    }
    return result;
  }
  /* After OM_NEW the published sample belongs to the superclass, which frees
   * it exactly once on OM_DISPOSE (SDTA_FreeSampleData = TRUE). */
  return DoSuperMethodA(cl, object, msg);
}

static ZZ9KSoundDatatypeBase *zz9k_sound_datatype_open(
    REG(a6, ZZ9KSoundDatatypeBase *base))
{
  if (!base) {
    return 0;
  }
  base->class_library.cl_Lib.lib_OpenCnt++;
  base->class_library.cl_Lib.lib_Flags &= (uint8_t)~LIBF_DELEXP;
  return base;
}

static BPTR zz9k_sound_datatype_close(REG(a6, ZZ9KSoundDatatypeBase *base))
{
  if (!base || base->class_library.cl_Lib.lib_OpenCnt == 0U) {
    return 0;
  }
  base->class_library.cl_Lib.lib_OpenCnt--;
  if (base->class_library.cl_Lib.lib_OpenCnt == 0U &&
      (base->class_library.cl_Lib.lib_Flags & LIBF_DELEXP) != 0U) {
    return zz9k_sound_datatype_expunge(base);
  }
  return 0;
}

static void zz9k_sound_close_system_bases(void)
{
  if (ZZ9KBase) {
    CloseLibrary(ZZ9KBase);
    ZZ9KBase = 0;
  }
  if (UtilityBase) {
    CloseLibrary(UtilityBase);
    UtilityBase = 0;
  }
  if (IntuitionBase) {
    CloseLibrary((struct Library *)IntuitionBase);
    IntuitionBase = 0;
  }
  if (DataTypesBase) {
    CloseLibrary(DataTypesBase);
    DataTypesBase = 0;
  }
  if (SoundBase) {
    CloseLibrary(SoundBase);
    SoundBase = 0;
  }
  if (DOSBase) {
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = 0;
  }
}

static BPTR zz9k_sound_datatype_expunge(REG(a6, ZZ9KSoundDatatypeBase *base))
{
  BPTR segment;
  if (!base) {
    return 0;
  }
  if (base->class_library.cl_Lib.lib_OpenCnt != 0U) {
    base->class_library.cl_Lib.lib_Flags |= LIBF_DELEXP;
    return 0;
  }
  Remove((struct Node *)base);
  if (base->class_added && base->class_library.cl_Class) {
    RemoveClass(base->class_library.cl_Class);
    base->class_added = 0U;
  }
  if (base->class_library.cl_Class) {
    FreeClass(base->class_library.cl_Class);
    base->class_library.cl_Class = 0;
  }
  zz9k_sound_close_system_bases();
  segment = base->segment;
  FreeMem((uint8_t *)base - base->class_library.cl_Lib.lib_NegSize,
          base->class_library.cl_Lib.lib_NegSize +
          base->class_library.cl_Lib.lib_PosSize);
  return segment;
}

static ULONG zz9k_sound_datatype_null(void)
{
  return 0;
}

static Class *zz9k_sound_datatype_get_class(REG(a6, ZZ9KSoundDatatypeBase *base))
{
  return base ? base->class_library.cl_Class : 0;
}

static void zz9k_sound_free_unpublished(ZZ9KSoundDatatypeBase *base)
{
  if (!base) {
    return;
  }
  if (base->class_added && base->class_library.cl_Class) {
    RemoveClass(base->class_library.cl_Class);
  }
  if (base->class_library.cl_Class) {
    FreeClass(base->class_library.cl_Class);
  }
  FreeMem((uint8_t *)base - base->class_library.cl_Lib.lib_NegSize,
          base->class_library.cl_Lib.lib_NegSize +
          base->class_library.cl_Lib.lib_PosSize);
}

static ZZ9KSoundDatatypeBase *zz9k_sound_datatype_init(REG(a0, BPTR segment))
{
  ZZ9KSoundDatatypeBase *base;

  SysBase = *(struct ExecBase **)4;
  DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 36);
  UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 39);
  IntuitionBase = (struct IntuitionBase *)OpenLibrary(
      (CONST_STRPTR)"intuition.library", 39);
  DataTypesBase = OpenLibrary((CONST_STRPTR)"datatypes.library", 39);
  if (!DOSBase || !UtilityBase || !IntuitionBase || !DataTypesBase) {
    zz9k_sound_close_system_bases();
    return 0;
  }
  /* Subclass the real sound.datatype and probe its version, not the OS
   * brand. On v47 the v41sound.datatype compatibility class ignores
   * SDTA_BitsPerSample; the system's own 16-bit loaders subclass
   * sound.datatype. */
  SoundBase = OpenLibrary((CONST_STRPTR)"datatypes/sound.datatype", 0);
  if (!SoundBase) {
    zz9k_sound_close_system_bases();
    return 0;
  }
  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_SOUND_LIBRARY_NAME,
                         ZZ9K_SOUND_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    zz9k_sound_close_system_bases();
    return 0;
  }
  base = (ZZ9KSoundDatatypeBase *)MakeLibrary(
      (CONST_APTR)zz9k_sound_datatype_vectors, 0, 0, sizeof(*base), 0);
  if (!base) {
    zz9k_sound_close_system_bases();
    return 0;
  }
  base->class_library.cl_Lib.lib_Node.ln_Type = NT_LIBRARY;
  base->class_library.cl_Lib.lib_Node.ln_Name =
      (char *)ZZ9K_SOUND_DATATYPE_NAME;
  base->class_library.cl_Lib.lib_Flags = LIBF_CHANGED | LIBF_SUMUSED;
  base->class_library.cl_Lib.lib_Version = ZZ9K_SOUND_DATATYPE_VERSION;
  base->class_library.cl_Lib.lib_Revision = ZZ9K_SOUND_DATATYPE_REVISION;
  base->class_library.cl_Lib.lib_IdString =
      (APTR)ZZ9K_SOUND_DATATYPE_ID_STRING;
  base->segment = segment;
  base->modern = SoundBase->lib_Version >= ZZ9K_SOUND_MODERN_VERSION;
  base->class_library.cl_Class = MakeClass(
      (CONST_STRPTR)ZZ9K_SOUND_DATATYPE_NAME, (CONST_STRPTR)SOUNDDTCLASS, 0,
      0, 0);
  if (!base->class_library.cl_Class) {
    zz9k_sound_free_unpublished(base);
    zz9k_sound_close_system_bases();
    return 0;
  }
  base->class_library.cl_Class->cl_Dispatcher.h_Entry =
      (ULONG (*)())zz9k_sound_datatype_dispatch;
  base->class_library.cl_Class->cl_UserData = (ULONG)base;
  AddClass(base->class_library.cl_Class);
  base->class_added = 1U;
  AddLibrary((struct Library *)base);
  return base;
}

#endif /* !ZZ9K_SOUND_DATATYPE_TEST */
