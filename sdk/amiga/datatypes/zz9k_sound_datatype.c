/*
 * ZZ9000 MP3 sound DataType class.
 *
 * Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define ZZ9K_SOUND_ID3V2_MAX_BYTES (16UL * 1024UL * 1024UL)
#define ZZ9K_SOUND_SYNC_SCAN_BYTES (16UL * 1024UL)
#define ZZ9K_SOUND_SAMPLE_INITIAL_BYTES (64UL * 1024UL)
#define ZZ9K_SOUND_SAMPLE_MAX_BYTES (256UL * 1024UL * 1024UL)
#define ZZ9K_SOUND_MP3_RING_BYTES (128UL * 1024UL)
#define ZZ9K_SOUND_HOST_BUFFER_BYTES (64UL * 1024UL)
#define ZZ9K_SOUND_HOST_BUFFER_MIN_BYTES (4UL * 1024UL)
#define ZZ9K_SOUND_MAX_PUMP_ATTEMPTS 65536UL

/* The recognition and conversion core deliberately has no Amiga dependency.
 * The actual-source host and m68k/vamos harness include this file with
 * ZZ9K_SOUND_DATATYPE_TEST defined. */
typedef struct ZZ9KSoundMp3Envelope {
  uint32_t first_frame;
  uint32_t frame_bytes;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t samples_per_frame;
} ZZ9KSoundMp3Envelope;

typedef struct ZZ9KSoundOwnedState {
  uint32_t session;
  uint32_t mp3_handle;
  uint32_t pcm_handle;
  uint32_t staging_handle;
  void *sample;
  uint8_t session_open;
  uint8_t mp3_allocated;
  uint8_t pcm_allocated;
  uint8_t staging_allocated;
  uint8_t sample_published;
} ZZ9KSoundOwnedState;

typedef struct ZZ9KSoundCleanupOps {
  void (*close_session)(void *ctx, uint32_t session);
  void (*free_shared)(void *ctx, uint32_t handle);
  void (*free_sample)(void *ctx, void *sample);
} ZZ9KSoundCleanupOps;

static uint32_t zz9k_sound_synchsafe32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 21) | ((uint32_t)p[1] << 14) |
         ((uint32_t)p[2] << 7) | (uint32_t)p[3];
}

static int zz9k_sound_mp3_header(const uint8_t *p, uint32_t available,
                                 ZZ9KSoundMp3Envelope *out)
{
  static const uint16_t rate_table[3][3] = {
    {11025U, 12000U, 8000U},
    {0U, 0U, 0U},
    {22050U, 24000U, 16000U}
  };
  static const uint16_t bitrate_v1[16] = {
    0U, 32U, 40U, 48U, 56U, 64U, 80U, 96U,
    112U, 128U, 160U, 192U, 224U, 256U, 320U, 0U
  };
  static const uint16_t bitrate_v2[16] = {
    0U, 8U, 16U, 24U, 32U, 40U, 48U, 56U,
    64U, 80U, 96U, 112U, 128U, 144U, 160U, 0U
  };
  uint32_t word;
  uint32_t version;
  uint32_t layer;
  uint32_t bitrate_index;
  uint32_t rate_index;
  uint32_t bitrate;
  uint32_t rate;
  uint32_t frame_bytes;

  if (!p || available < 4U) {
    return 0;
  }
  word = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
  if ((word & 0xffe00000UL) != 0xffe00000UL) {
    return 0;
  }
  version = (word >> 19) & 3U;
  layer = (word >> 17) & 3U;
  bitrate_index = (word >> 12) & 15U;
  rate_index = (word >> 10) & 3U;
  /* Layer bits 01 are Layer III. Version bits 01 are reserved. */
  if (version == 1U || layer != 1U || bitrate_index == 0U ||
      bitrate_index == 15U || rate_index == 3U) {
    return 0;
  }
  if (version == 3U) {
    rate = (uint32_t[]){44100U, 48000U, 32000U}[rate_index];
    bitrate = bitrate_v1[bitrate_index];
    frame_bytes = (144000UL * bitrate) / rate + ((word >> 9) & 1U);
  } else {
    rate = rate_table[version][rate_index];
    bitrate = bitrate_v2[bitrate_index];
    frame_bytes = (72000UL * bitrate) / rate + ((word >> 9) & 1U);
  }
  if (rate == 0U || bitrate == 0U || frame_bytes < 4U) {
    return 0;
  }
  if (out) {
    memset(out, 0, sizeof(*out));
    out->frame_bytes = frame_bytes;
    out->sample_rate = rate;
    out->channels = ((word >> 6) & 3U) == 3U ? 1U : 2U;
    out->samples_per_frame = version == 3U ? 1152U : 576U;
  }
  return 1;
}

static int zz9k_sound_recognize_mp3(const uint8_t *bytes, uint32_t length,
                                    ZZ9KSoundMp3Envelope *envelope)
{
  uint32_t start;
  uint32_t scan_end;
  uint32_t offset;

  if (!bytes || length < 8U) {
    return 0;
  }
  start = 0U;
  if (length >= 10U && bytes[0] == 'I' && bytes[1] == 'D' &&
      bytes[2] == '3') {
    uint32_t body;
    uint32_t footer;
    if (bytes[3] == 0xffU || bytes[4] == 0xffU ||
        (bytes[6] | bytes[7] | bytes[8] | bytes[9]) >= 0x80U) {
      return 0;
    }
    body = zz9k_sound_synchsafe32(bytes + 6U);
    footer = (bytes[5] & 0x10U) != 0U ? 10U : 0U;
    if (body > ZZ9K_SOUND_ID3V2_MAX_BYTES ||
        body > 0xffffffffUL - 10U - footer) {
      return 0;
    }
    start = 10U + body + footer;
    if (start > length) {
      return 0;
    }
  }
  scan_end = length;
  if (scan_end - start > ZZ9K_SOUND_SYNC_SCAN_BYTES) {
    scan_end = start + ZZ9K_SOUND_SYNC_SCAN_BYTES;
  }
  for (offset = start; offset + 8U <= scan_end; offset++) {
    ZZ9KSoundMp3Envelope first;
    ZZ9KSoundMp3Envelope second;
    uint32_t second_offset;
    if (!zz9k_sound_mp3_header(bytes + offset, length - offset, &first)) {
      continue;
    }
    if (first.frame_bytes > length - offset) {
      continue;
    }
    second_offset = offset + first.frame_bytes;
    if (second_offset + 4U > length ||
        !zz9k_sound_mp3_header(bytes + second_offset,
                               length - second_offset, &second) ||
        second.sample_rate != first.sample_rate ||
        second.channels != first.channels) {
      continue;
    }
    if (envelope) {
      *envelope = first;
      envelope->first_frame = offset;
    }
    return 1;
  }
  return 0;
}

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
  if (state->mp3_allocated) {
    if (ops->free_shared) {
      ops->free_shared(ctx, state->mp3_handle);
    }
    state->mp3_allocated = 0U;
  }
  if (state->sample && !state->sample_published) {
    if (ops->free_sample) {
      ops->free_sample(ctx, state->sample);
    }
    state->sample = 0;
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

#ifndef SDTA_SampleChannels
#define SDTA_SampleChannels (SDTA_Dummy + 14)
#endif
#ifndef SDTA_BitsPerSample
#define SDTA_BitsPerSample (SDTA_Dummy + 15)
#endif
#ifndef SDTA_FreeSampleData
#define SDTA_FreeSampleData (SDTA_Dummy + 16)
#endif
#ifndef SDTA_SamplesPerSec
#define SDTA_SamplesPerSec (SDTA_Dummy + 17)
#endif

#define ZZ9K_SOUND_DATATYPE_NAME "zz9k-sound.datatype"
#define ZZ9K_SOUND_DATATYPE_VERSION 42
#define ZZ9K_SOUND_DATATYPE_REVISION 1
#define ZZ9K_SOUND_DATATYPE_ID_STRING \
  "$VER: zz9k-sound.datatype 42.1 (9.10.2026) ZZ9000 SDK"
#define ZZ9K_SOUND_MODERN_SUPERCLASS "v41sound.datatype"
#define ZZ9K_SOUND_LIBRARY_NAME "zz9k.library"
#define ZZ9K_SOUND_LIBRARY_VERSION 2

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *DataTypesBase;
struct Library *SoundBase;
struct Library *UtilityBase;
struct Library *ZZ9KBase;

/* These are class-instance state, not sample ownership. A successfully
 * published sample is owned by sound.datatype through FreeSampleData=TRUE. */
typedef struct ZZ9KSoundInstance {
  ZZ9KSoundOwnedState owned;
} ZZ9KSoundInstance;

typedef struct ZZ9KSoundDatatypeBase {
  struct ClassLibrary class_library;
  BPTR segment;
  UBYTE class_added;
  UBYTE modern_superclass;
} ZZ9KSoundDatatypeBase;

typedef struct ZZ9KSoundDecode {
  ZZ9KSoundOwnedState owned;
  ZZ9KSharedBuffer mp3_ring;
  ZZ9KSharedBuffer pcm_ring;
  ZZ9KSharedBuffer staging;
  ZZ9KAudioStreamResult result;
  uint8_t *sample;
  uint32_t sample_bytes;
  uint32_t sample_capacity;
  uint32_t pcm_seen;
  uint32_t pcm_offset;
  uint32_t source_channels;
  uint32_t sample_rate;
  uint8_t modern;
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

static int zz9k_sound_source_size(BPTR file, uint32_t *size)
{
  LONG old;
  LONG end;
  if (!file || !size) {
    return 0;
  }
  old = Seek(file, 0, OFFSET_CURRENT);
  if (old < 0) {
    return 0;
  }
  end = Seek(file, 0, OFFSET_END);
  if (end < 0 || Seek(file, old, OFFSET_BEGINNING) < 0) {
    return 0;
  }
  *size = (uint32_t)end;
  return end >= 0;
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

static int zz9k_sound_probe_file(BPTR file, uint32_t file_size,
                                 ZZ9KSoundMp3Envelope *envelope,
                                 uint32_t *audio_end)
{
  uint8_t header[10];
  uint8_t *probe;
  uint32_t start;
  uint32_t probe_bytes;
  uint32_t end;
  int ok;

  if (!file || !envelope || file_size < 8U ||
      !zz9k_sound_read_at(file, 0U, header, sizeof(header))) {
    return 0;
  }
  start = 0U;
  if (header[0] == 'I' && header[1] == 'D' && header[2] == '3') {
    uint32_t footer;
    uint32_t body;
    if (header[3] == 0xffU || header[4] == 0xffU ||
        (header[6] | header[7] | header[8] | header[9]) >= 0x80U) {
      return 0;
    }
    body = zz9k_sound_synchsafe32(header + 6U);
    footer = (header[5] & 0x10U) != 0U ? 10U : 0U;
    if (body > ZZ9K_SOUND_ID3V2_MAX_BYTES ||
        body > 0xffffffffUL - 10U - footer) {
      return 0;
    }
    start = 10U + body + footer;
    if (start >= file_size) {
      return 0;
    }
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
       zz9k_sound_recognize_mp3(probe, probe_bytes, envelope);
  if (ok) {
    envelope->first_frame += start;
    *audio_end = end;
  }
  FreeVec(probe);
  return ok;
}

static int zz9k_sound_alloc_host_pair(ZZ9KSoundDecode *decode)
{
  uint32_t budget;
  if (!decode) {
    return 0;
  }
  budget = ZZ9K_SOUND_HOST_BUFFER_BYTES;
  while (budget >= ZZ9K_SOUND_HOST_BUFFER_MIN_BYTES) {
    if (ZZ9KAllocShared(budget, 16U, ZZ9K_ALLOC_HOST_WINDOW,
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
    budget /= 2U;
  }
  return 0;
}

static int zz9k_sound_reserve_sample(ZZ9KSoundDecode *decode,
                                     uint32_t additional)
{
  uint32_t required;
  uint32_t capacity;
  uint8_t *replacement;
  if (!decode || additional > ZZ9K_SOUND_SAMPLE_MAX_BYTES - decode->sample_bytes) {
    return 0;
  }
  required = decode->sample_bytes + additional;
  if (required <= decode->sample_capacity) {
    return 1;
  }
  if (!zz9k_sound_next_capacity(decode->sample_capacity, required,
                                ZZ9K_SOUND_SAMPLE_MAX_BYTES, &capacity)) {
    return 0;
  }
  replacement = (uint8_t *)AllocVec(capacity, MEMF_PUBLIC);
  if (!replacement) {
    return 0;
  }
  if (decode->sample_bytes != 0U) {
    CopyMem(decode->sample, replacement, decode->sample_bytes);
  }
  if (decode->sample) {
    FreeVec(decode->sample);
  }
  decode->sample = replacement;
  decode->owned.sample = replacement;
  decode->sample_capacity = capacity;
  return 1;
}

static uint8_t zz9k_sound_ring_byte(const ZZ9KSoundDecode *decode,
                                    uint32_t offset)
{
  return ((volatile const uint8_t *)decode->pcm_ring.data)[offset];
}

static int zz9k_sound_copy_pcm_before_ack(ZZ9KSoundDecode *decode)
{
  uint32_t available;
  uint32_t frame_bytes;
  uint32_t frames;
  uint32_t output_bytes;
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
  if (available > decode->pcm_ring.length ||
      (decode->result.channels != 1U && decode->result.channels != 2U) ||
      decode->result.sample_format != ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE) {
    return 0;
  }
  frame_bytes = decode->result.channels * 2U;
  available -= available % frame_bytes;
  frames = available / frame_bytes;
  if (decode->modern) {
    if (frames > 0xffffffffUL / frame_bytes) {
      return 0;
    }
    output_bytes = frames * frame_bytes;
  } else {
    output_bytes = frames;
  }
  if (!zz9k_sound_reserve_sample(decode, output_bytes)) {
    return 0;
  }
  offset = decode->pcm_offset;
  for (i = 0U; i < frames; i++) {
    uint8_t frame[4];
    uint32_t b;
    for (b = 0U; b < frame_bytes; b++) {
      frame[b] = zz9k_sound_ring_byte(decode, offset);
      if (++offset == decode->pcm_ring.length) {
        offset = 0U;
      }
    }
    if (decode->modern) {
      memcpy(decode->sample + decode->sample_bytes, frame, frame_bytes);
      decode->sample_bytes += frame_bytes;
    } else {
      decode->sample[decode->sample_bytes++] =
          (uint8_t)zz9k_sound_legacy_quantize_s16be(
              frame, decode->result.channels);
    }
  }
  /* The host copy is complete before firmware receives this acknowledgement.
   * It is then free to overwrite the ring range. */
  if (ZZ9KAudioStreamRead(decode->result.session, available, 0U,
                          &decode->result) != ZZ9K_STATUS_OK) {
    return 0;
  }
  decode->pcm_seen += available;
  decode->pcm_offset = decode->result.pcm_read;
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

static int zz9k_sound_decode_file(BPTR file, uint32_t first_frame,
                                  uint32_t audio_end, uint8_t modern,
                                  ZZ9KSoundDecode *decode)
{
  ZZ9KAudioStreamBeginDesc begin;
  uint8_t *input;
  uint32_t position;
  uint32_t pending_offset;
  uint32_t pending_bytes;
  uint32_t consumed_seen;
  uint32_t attempts;
  int ok;

  memset(decode, 0, sizeof(*decode));
  decode->modern = modern;
  decode->owned.mp3_handle = ZZ9K_INVALID_HANDLE;
  decode->owned.pcm_handle = ZZ9K_INVALID_HANDLE;
  decode->owned.staging_handle = ZZ9K_INVALID_HANDLE;
  input = 0;
  ok = 0;

  if (ZZ9KAllocShared(ZZ9K_SOUND_MP3_RING_BYTES, 16U, ZZ9K_ALLOC_CARD_ONLY,
                      &decode->mp3_ring) != ZZ9K_STATUS_OK) {
    goto done;
  }
  decode->owned.mp3_allocated = 1U;
  decode->owned.mp3_handle = decode->mp3_ring.handle;
  if (!zz9k_sound_alloc_host_pair(decode)) {
    goto done;
  }
  if (!zz9k_audio_build_stream_begin_desc(
          &begin, decode->mp3_ring.handle, decode->mp3_ring.length,
          decode->pcm_ring.handle, decode->pcm_ring.length, 0U, 0U,
          ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE, 0U, 0U, 0U) ||
      ZZ9KAudioStreamBegin(&begin, &decode->result) != ZZ9K_STATUS_OK) {
    goto done;
  }
  decode->owned.session_open = 1U;
  decode->owned.session = decode->result.session;
  decode->pcm_offset = decode->result.pcm_read;
  input = (uint8_t *)AllocVec(decode->staging.length, MEMF_PUBLIC);
  if (!input) {
    goto done;
  }

  position = first_frame;
  pending_offset = 0U;
  pending_bytes = 0U;
  consumed_seen = decode->result.bytes_consumed;
  for (attempts = 0U; attempts < ZZ9K_SOUND_MAX_PUMP_ATTEMPTS; attempts++) {
    ZZ9KAudioStreamFeedDesc feed;
    uint32_t chunk;
    uint32_t flags;
    uint32_t consumed;

    if (!zz9k_sound_copy_pcm_before_ack(decode)) {
      goto done;
    }
    if ((decode->result.flags & ZZ9K_AUDIO_STREAM_RESULT_DONE) != 0U) {
      ok = 1;
      break;
    }
    if (pending_bytes == 0U && position < audio_end) {
      chunk = audio_end - position;
      if (chunk > decode->staging.length) {
        chunk = decode->staging.length;
      }
      if (!zz9k_sound_read_at(file, position, input, chunk) ||
          !zz9k_shared_copy_to(&decode->staging, 0U, input, chunk)) {
        goto done;
      }
      position += chunk;
      pending_offset = 0U;
      pending_bytes = chunk;
    }
    if (pending_bytes != 0U) {
      chunk = pending_bytes;
      flags = 0U;
    } else {
      chunk = 0U;
      flags = ZZ9K_AUDIO_STREAM_FEED_EOF;
    }
    if (!zz9k_audio_build_stream_feed_desc(
            &feed, decode->result.session, decode->staging.handle,
            pending_offset, chunk, flags) ||
        ZZ9KAudioStreamFeed(&feed, &decode->result) != ZZ9K_STATUS_OK) {
      goto done;
    }
    if (decode->result.bytes_consumed < consumed_seen) {
      goto done;
    }
    consumed = decode->result.bytes_consumed - consumed_seen;
    if (consumed > pending_bytes) {
      goto done;
    }
    consumed_seen += consumed;
    pending_offset += consumed;
    pending_bytes -= consumed;
    if (decode->result.sample_rate != 0U) {
      decode->sample_rate = decode->result.sample_rate;
      decode->source_channels = decode->result.channels;
    }
  }
  if (ok && !zz9k_sound_copy_pcm_before_ack(decode)) {
    ok = 0;
  }
  if (ok && (decode->sample_rate == 0U ||
             (decode->source_channels != 1U &&
              decode->source_channels != 2U) ||
             decode->sample_bytes == 0U)) {
    ok = 0;
  }

done:
  if (input) {
    FreeVec(input);
  }
  /* Success still closes and frees every firmware resource before the caller
   * may publish the AllocVec sample to sound.datatype. */
  if (ok) {
    /* Retain only the host AllocVec sample. The temporary published marker
     * prevents generic partial-initialization cleanup from freeing it; the
     * caller still has not handed it to the superclass. */
    decode->owned.sample_published = 1U;
    zz9k_sound_cleanup_owned(&decode->owned, &zz9k_sound_real_cleanup_ops, 0);
    decode->owned.sample_published = 0U;
    decode->owned.sample = decode->sample;
  } else {
    zz9k_sound_cleanup_owned(&decode->owned, &zz9k_sound_real_cleanup_ops, 0);
    decode->sample = 0;
  }
  return ok;
}

static int zz9k_sound_publish(Object *object, ZZ9KSoundDecode *decode,
                              uint8_t modern)
{
  uint32_t channels;
  uint32_t bytes_per_frame;
  uint32_t sample_length;
  ULONG changed;

  if (!object || !decode || !decode->sample || decode->sample_bytes == 0U) {
    return 0;
  }
  channels = modern ? decode->source_channels : 1U;
  bytes_per_frame = modern ? channels * 2U : 1U;
  if (bytes_per_frame == 0U || decode->sample_bytes % bytes_per_frame != 0U) {
    return 0;
  }
  sample_length = decode->sample_bytes / bytes_per_frame;
  changed = SetDTAttrs(object, 0, 0,
                       SDTA_Sample, (ULONG)decode->sample,
                       SDTA_SampleLength, sample_length,
                       SDTA_SampleChannels, channels,
                       SDTA_BitsPerSample, modern ? 16U : 8U,
                       SDTA_SamplesPerSec, decode->sample_rate,
                       SDTA_Period, decode->sample_rate ?
                           (ULONG)(3579545UL / decode->sample_rate) : 0U,
                       SDTA_Volume, modern ? 64U : 0x10000UL,
                       SDTA_Cycles, 1U,
                       SDTA_FreeSampleData, TRUE,
                       TAG_END);
  if (changed == 0U) {
    return 0;
  }
  decode->owned.sample_published = 1U;
  decode->owned.sample = 0;
  decode->sample = 0;
  return 1;
}

static int zz9k_sound_load(Object *object, ZZ9KSoundDatatypeBase *base,
                           ZZ9KSoundInstance *instance)
{
  ZZ9KServiceInfo service;
  ZZ9KSoundMp3Envelope envelope;
  ZZ9KSoundDecode decode;
  BPTR file;
  ULONG source_type;
  uint32_t file_size;
  uint32_t audio_end;
  int ok;

  if (!object || !base || !instance) {
    return 0;
  }
  source_type = DTST_FILE;
  file = 0;
  (void)GetDTAttrs(object, DTA_SourceType, (ULONG)&source_type,
                   DTA_Handle, (ULONG)&file, TAG_END);
  /* Memory sources are intentionally refused: both probed superclass stacks
   * reject their relevant NewDTObject memory flow. */
  if (source_type != DTST_FILE || !file ||
      !zz9k_sound_source_size(file, &file_size) ||
      !zz9k_sound_probe_file(file, file_size, &envelope, &audio_end)) {
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }
  if (!ZZ9KBase ||
      ZZ9KBase->lib_Revision < ZZ9K_LIBRARY_MIN_REVISION_ALLOC_FLAGS ||
      ZZ9KQueryService(ZZ9K_SERVICE_AUDIO, &service) != ZZ9K_STATUS_OK ||
      (service.flags & (ZZ9K_SERVICE_FLAG_AUDIO_MP3_DECODE |
                        ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM |
                        ZZ9K_SERVICE_FLAG_AUDIO_PCM16_STEREO)) !=
          (ZZ9K_SERVICE_FLAG_AUDIO_MP3_DECODE |
           ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM |
           ZZ9K_SERVICE_FLAG_AUDIO_PCM16_STEREO)) {
    SetIoErr(ERROR_NOT_IMPLEMENTED);
    return 0;
  }
  ok = zz9k_sound_decode_file(file, envelope.first_frame, audio_end,
                              base->modern_superclass, &decode);
  if (!ok || decode.sample_rate != envelope.sample_rate ||
      decode.source_channels != envelope.channels ||
      !zz9k_sound_publish(object, &decode, base->modern_superclass)) {
    zz9k_sound_cleanup_owned(&decode.owned, &zz9k_sound_real_cleanup_ops, 0);
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }
  instance->owned.sample_published = 1U;
  return 1;
}

static ULONG zz9k_sound_datatype_dispatch(REG(a0, struct Hook *hook),
                                          REG(a2, Object *object),
                                          REG(a1, Msg msg))
{
  Class *cl;
  ULONG result;
  if (!hook || !msg) {
    return 0;
  }
  cl = (Class *)hook;
  switch (msg->MethodID) {
  case OM_NEW:
    result = DoSuperMethodA(cl, object, msg);
    if (result) {
      Object *new_object = (Object *)result;
      ZZ9KSoundInstance *instance =
          (ZZ9KSoundInstance *)INST_DATA(cl, new_object);
      ZZ9KSoundDatatypeBase *base =
          (ZZ9KSoundDatatypeBase *)cl->cl_UserData;
      memset(instance, 0, sizeof(*instance));
      if (!zz9k_sound_load(new_object, base, instance)) {
        CoerceMethod(cl, new_object, OM_DISPOSE);
        result = 0;
      }
    }
    return result;
  case OM_DISPOSE:
  {
    ZZ9KSoundInstance *instance =
        (ZZ9KSoundInstance *)INST_DATA(cl, object);
    if (instance) {
      /* Published sample ownership remains with the superclass. */
      memset(instance, 0, sizeof(*instance));
    }
    return DoSuperMethodA(cl, object, msg);
  }
  case DTM_PROCLAYOUT:
  case DTM_ASYNCLAYOUT:
    return DoSuperMethodA(cl, object, msg);
  default:
    return DoSuperMethodA(cl, object, msg);
  }
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
  CONST_STRPTR superclass;

  SysBase = *(struct ExecBase **)4;
  DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 36);
  UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 39);
  DataTypesBase = OpenLibrary((CONST_STRPTR)"datatypes.library", 39);
  if (!DOSBase || !UtilityBase || !DataTypesBase) {
    zz9k_sound_close_system_bases();
    return 0;
  }
  /* Probe the actual installed superclass, not the OS brand. */
  SoundBase = OpenLibrary((CONST_STRPTR)"datatypes/v41sound.datatype", 0);
  superclass = (CONST_STRPTR)ZZ9K_SOUND_MODERN_SUPERCLASS;
  if (!SoundBase) {
    SoundBase = OpenLibrary((CONST_STRPTR)"datatypes/sound.datatype", 0);
    superclass = (CONST_STRPTR)SOUNDDTCLASS;
  }
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
  base->modern_superclass =
      strcmp((const char *)superclass, ZZ9K_SOUND_MODERN_SUPERCLASS) == 0;
  base->class_library.cl_Class = MakeClass(
      (CONST_STRPTR)ZZ9K_SOUND_DATATYPE_NAME, superclass, 0,
      sizeof(ZZ9KSoundInstance), 0);
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
