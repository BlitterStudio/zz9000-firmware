/*
 * Wavetable voice engine contract for ZZ9000 SDK v2.
 *
 * The firmware mixes up to 32 wavetable voices at 48 kHz into its own audio
 * fabric slot. An Amiga client drives it with timestamped 16-byte records
 * written into a shared-memory ring, uploads samples through a staging
 * buffer, and reads progress from a status block. Everything on the wire is
 * big-endian, as the Amiga writes it.
 *
 * This header carries the mailbox payload layouts (opcodes
 * ZZ9K_OP_AUDIO_VOICE_* in zz9k/abi.h), the ring, record and status layouts,
 * and the pure helpers that build records and read the status block. The
 * session calls that allocate buffers and talk to the card are declared in
 * zz9k/host.h (zz9k_voice_*) and zz9k.library (ZZ9KVoice*).
 *
 * Copyright (C) 2024-2026 Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */

#ifndef ZZ9K_VOICE_H
#define ZZ9K_VOICE_H

#include "zz9k/abi.h"
#include "zz9k/audio.h"

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Limits and defaults ---- */

#define ZZ9K_VOICE_MAX_VOICES       32U
#define ZZ9K_VOICE_MAX_SAMPLES      256U
#define ZZ9K_VOICE_OUTPUT_RATE      48000U
#define ZZ9K_VOICE_BANK_MIN_BYTES   1U
#define ZZ9K_VOICE_BANK_MAX_BYTES   (32U * 1024U * 1024U)

/* Shared buffers are cache-line aligned so firmware cache maintenance on one
 * never touches a neighbour. */
#define ZZ9K_VOICE_BUFFER_ALIGN     64U

/* The client increments the ring heartbeat at least this often. */
#define ZZ9K_VOICE_HEARTBEAT_MS     500U

/* Default buffer sizes. A 2 KiB ring holds 64 records and fits the 16 KiB
 * Zorro II host window next to AHI; the caller may ask for more. */
#define ZZ9K_VOICE_RING_DEFAULT_BYTES    2048U
#define ZZ9K_VOICE_STAGING_DEFAULT_BYTES 4096U

/* ---- OPEN flags ---- */

#define ZZ9K_VOICE_OPEN_FLAG_NTSC   (1U << 0) /* NTSC clock, else PAL */
#define ZZ9K_VOICE_OPEN_FLAG_LINEAR (1U << 1) /* linear interpolation */
/* Low-latency session (low-latency work): the firmware shortens its audio
 * fill-ahead while the session is open, and the lead floor drops from 8
 * periods (7680 frames) to 3 (2880 frames). Firmware without the mode
 * answers ZZ9K_STATUS_BAD_REQUEST; zz9k.library before revision 43 refuses
 * the flag locally with the same status. */
#define ZZ9K_VOICE_OPEN_FLAG_LOW_LATENCY (1U << 2)
#define ZZ9K_VOICE_OPEN_FLAG_KNOWN \
  (ZZ9K_VOICE_OPEN_FLAG_NTSC | ZZ9K_VOICE_OPEN_FLAG_LINEAR | \
   ZZ9K_VOICE_OPEN_FLAG_LOW_LATENCY)

/* ---- UPLOAD flags and sample formats ---- */

#define ZZ9K_VOICE_UPLOAD_FLAG_COPY   (1U << 0) /* copy staged bytes to the bank */
#define ZZ9K_VOICE_UPLOAD_FLAG_DEFINE (1U << 1) /* define a sample */
#define ZZ9K_VOICE_UPLOAD_FLAG_KNOWN \
  (ZZ9K_VOICE_UPLOAD_FLAG_COPY | ZZ9K_VOICE_UPLOAD_FLAG_DEFINE)

#define ZZ9K_VOICE_FORMAT_S8  0U /* signed 8-bit */
#define ZZ9K_VOICE_FORMAT_S16 1U /* signed 16-bit, big-endian */

/* ---- Mailbox payloads (48 bytes, big-endian) ---- */

/* OPEN request. All three handles are host-window shared buffers owned by
 * the caller and distinct from one another; firmware pins them until the
 * session closes. lead_frames is raised to the firmware floor and capped at
 * its maximum. */
typedef struct ZZ9KAudioVoiceOpenPayload {
  uint8_t ring_handle[4];
  uint8_t status_handle[4];
  uint8_t staging_handle[4];
  uint8_t bank_bytes[4];
  uint8_t lead_frames[4];
  uint8_t flags[4];
  uint8_t voices[4];
  uint8_t reserved[20];
} ZZ9KAudioVoiceOpenPayload;

/* OPEN result: what the firmware granted. session is nonzero and changes on
 * every open. */
typedef struct ZZ9KAudioVoiceOpenResultPayload {
  uint8_t session[4];
  uint8_t voices[4];
  uint8_t bank_bytes[4];
  uint8_t lead_frames[4];
  uint8_t lead_floor_frames[4];
  uint8_t lead_max_frames[4];
  uint8_t ring_records[4];
  uint8_t output_rate[4];
  uint8_t max_samples[4];
  uint8_t reserved[12];
} ZZ9KAudioVoiceOpenResultPayload;

/* CLOSE and RESET request. */
typedef struct ZZ9KAudioVoiceSessionPayload {
  uint8_t session[4];
  uint8_t reserved[44];
} ZZ9KAudioVoiceSessionPayload;

/* UPLOAD request. COPY moves length bytes from staging_offset in the staging
 * buffer to bank_offset in the bank; DEFINE then records sample_id as the
 * sample_length bytes at sample_offset in format. COPY runs first. */
typedef struct ZZ9KAudioVoiceUploadPayload {
  uint8_t session[4];
  uint8_t flags[4];
  uint8_t staging_offset[4];
  uint8_t bank_offset[4];
  uint8_t length[4];
  uint8_t sample_id[4];
  uint8_t sample_offset[4];
  uint8_t sample_length[4];
  uint8_t format[4];
  uint8_t reserved[12];
} ZZ9KAudioVoiceUploadPayload;

typedef char ZZ9KAudioVoiceOpenPayload_is_48_bytes[
    sizeof(ZZ9KAudioVoiceOpenPayload) == 48U ? 1 : -1];
typedef char ZZ9KAudioVoiceOpenResultPayload_is_48_bytes[
    sizeof(ZZ9KAudioVoiceOpenResultPayload) == 48U ? 1 : -1];
typedef char ZZ9KAudioVoiceSessionPayload_is_48_bytes[
    sizeof(ZZ9KAudioVoiceSessionPayload) == 48U ? 1 : -1];
typedef char ZZ9KAudioVoiceUploadPayload_is_48_bytes[
    sizeof(ZZ9KAudioVoiceUploadPayload) == 48U ? 1 : -1];

/* ---- Command ring ----
 *
 * A host-window buffer the client owns. A 16-byte header is followed by
 * records. Capacity N is the largest power of two not above
 * (ring_bytes - 16) / 16, at least 16 and at most 4096; record i lives at
 * 16 + (i & (N - 1)) * 16. The client writes records first and then the
 * write cursor in one 16-bit store. It may have at most N records
 * outstanding: free = N - (uint16_t)(write_cursor - read_cursor), with the
 * read cursor taken from the status block. */

#define ZZ9K_VOICE_RING_HEADER_BYTES 16U
#define ZZ9K_VOICE_RECORD_BYTES      16U
#define ZZ9K_VOICE_RING_MIN_RECORDS  16U
#define ZZ9K_VOICE_RING_MAX_RECORDS  4096U
#define ZZ9K_VOICE_RING_MIN_BYTES \
  (ZZ9K_VOICE_RING_HEADER_BYTES + \
   ZZ9K_VOICE_RING_MIN_RECORDS * ZZ9K_VOICE_RECORD_BYTES)
#define ZZ9K_VOICE_RING_MAX_BYTES \
  (ZZ9K_VOICE_RING_HEADER_BYTES + \
   ZZ9K_VOICE_RING_MAX_RECORDS * ZZ9K_VOICE_RECORD_BYTES)

#define ZZ9K_VOICE_RING_WRITE_CURSOR_OFFSET 0U /* u16, client */
#define ZZ9K_VOICE_RING_HEARTBEAT_OFFSET    4U /* u32, client */

typedef struct ZZ9KVoiceRingHeader {
  uint8_t write_cursor[2];
  uint8_t reserved0[2];
  uint8_t heartbeat[4];
  uint8_t reserved1[8];
} ZZ9KVoiceRingHeader;

/* Record ops (the engine's decoded commands, see audio_voice.h in the
 * firmware). START: flags = sample id, arg0 = start offset, arg1 =
 * first-pass length (0 = to the sample end). LOOP: arg0/arg1 = loop
 * start/length. SET: flags select rate, volume and rate-in-Hz; arg0 = rate,
 * arg1 = (left << 16) | right. STOP silences the voice. MASTER: arg0 = Q8.8
 * gain. DIRECTION: flags bit 0 = backward. Stamps must not decrease. */
#define ZZ9K_VOICE_OP_START     1U
#define ZZ9K_VOICE_OP_LOOP      2U
#define ZZ9K_VOICE_OP_SET       3U
#define ZZ9K_VOICE_OP_STOP      4U
#define ZZ9K_VOICE_OP_MASTER    5U
#define ZZ9K_VOICE_OP_DIRECTION 6U

#define ZZ9K_VOICE_SET_RATE     0x0001U
#define ZZ9K_VOICE_SET_VOLUME   0x0002U
#define ZZ9K_VOICE_SET_RATE_HZ  0x0004U /* rate is Hz, else a Paula period */
#define ZZ9K_VOICE_DIR_BACKWARD 0x0001U

#define ZZ9K_VOICE_MASTER_UNITY 256U

/* Wire record. */
typedef struct ZZ9KVoiceRecordWire {
  uint8_t stamp[4];
  uint8_t voice;
  uint8_t op;
  uint8_t flags[2];
  uint8_t arg0[4];
  uint8_t arg1[4];
} ZZ9KVoiceRecordWire;

typedef char ZZ9KVoiceRecordWire_is_16_bytes[
    sizeof(ZZ9KVoiceRecordWire) == ZZ9K_VOICE_RECORD_BYTES ? 1 : -1];
typedef char ZZ9KVoiceRingHeader_is_16_bytes[
    sizeof(ZZ9KVoiceRingHeader) == ZZ9K_VOICE_RING_HEADER_BYTES ? 1 : -1];

/* Host-order record, as the builders below fill it. */
typedef struct ZZ9KVoiceRecord {
  uint32_t stamp;  /* engine frame the command applies at */
  uint8_t voice;
  uint8_t op;      /* ZZ9K_VOICE_OP_* */
  uint16_t flags;
  uint32_t arg0;
  uint32_t arg1;
} ZZ9KVoiceRecord;

/* ---- Status block ----
 *
 * A host-window buffer of at least 64 bytes. Only the firmware writes it,
 * from its main-loop poll; the client only reads it. */

#define ZZ9K_VOICE_STATUS_BYTES 64U

#define ZZ9K_VOICE_STATUS_PLAYING_FRAME_OFFSET 0U  /* u32 */
#define ZZ9K_VOICE_STATUS_HORIZON_OFFSET       4U  /* u32 */
#define ZZ9K_VOICE_STATUS_READ_CURSOR_OFFSET   8U  /* u16 */
#define ZZ9K_VOICE_STATUS_STATE_OFFSET         10U /* u16 */
#define ZZ9K_VOICE_STATUS_LATE_OFFSET          12U /* u32 */
#define ZZ9K_VOICE_STATUS_REJECTED_OFFSET      16U /* u32 */
#define ZZ9K_VOICE_STATUS_UNDERRUNS_OFFSET     20U /* u32 */
#define ZZ9K_VOICE_STATUS_DROPPED_OFFSET       24U /* u32 */
#define ZZ9K_VOICE_STATUS_SESSION_OFFSET       28U /* u32 */

/* Jitter telemetry (low-latency work), since open. The margin is the
 * smallest time a rendered period waited for the audio interrupt to take
 * it, in microseconds, and ZZ9K_VOICE_JITTER_NONE until the first period is
 * taken. The poll gap is the largest gap between two firmware voice polls,
 * in microseconds. Firmware without them leaves both words zero. */
#define ZZ9K_VOICE_STATUS_MARGIN_OFFSET        32U /* u32 */
#define ZZ9K_VOICE_STATUS_POLL_GAP_OFFSET      36U /* u32 */
#define ZZ9K_VOICE_JITTER_NONE                 0xFFFFFFFFU

#define ZZ9K_VOICE_STATE_CLOSED 0U
#define ZZ9K_VOICE_STATE_OPEN   1U /* open, no record yet */
#define ZZ9K_VOICE_STATE_LIVE   2U

typedef struct ZZ9KVoiceStatusWire {
  uint8_t playing_frame[4];
  uint8_t horizon[4];
  uint8_t read_cursor[2];
  uint8_t state[2];
  uint8_t late[4];
  uint8_t rejected[4];
  uint8_t underruns[4];
  uint8_t dropped[4];
  uint8_t session[4];
  uint8_t margin_min_us[4];
  uint8_t poll_gap_max_us[4];
  uint8_t reserved[24];
} ZZ9KVoiceStatusWire;

typedef char ZZ9KVoiceStatusWire_is_64_bytes[
    sizeof(ZZ9KVoiceStatusWire) == ZZ9K_VOICE_STATUS_BYTES ? 1 : -1];

/* Host-order status snapshot. */
typedef struct ZZ9KVoiceStatus {
  uint32_t playing_frame; /* first frame of the period the DMA is fetching */
  uint32_t horizon;       /* next engine frame the engine will render */
  uint16_t read_cursor;   /* records the firmware has taken */
  uint16_t state;         /* ZZ9K_VOICE_STATE_* */
  uint32_t late;
  uint32_t rejected;
  uint32_t underruns;
  uint32_t dropped;
  uint32_t session;
} ZZ9KVoiceStatus;

/* Host-order jitter telemetry, read by zz9k_voice_jitter_read(). It is not
 * part of ZZ9KVoiceStatus so that a program built against the older, smaller
 * struct keeps working with a library that fills it in. */
typedef struct ZZ9KVoiceJitter {
  uint32_t margin_min_us;   /* ZZ9K_VOICE_JITTER_NONE until the first period */
  uint32_t poll_gap_max_us; /* 0 on firmware without the telemetry */
} ZZ9KVoiceJitter;

/* ---- Host-facing descriptors ---- */

/* What the caller asks for at open. ring_bytes and staging_bytes of 0 select
 * the defaults; larger values buy a deeper ring or fewer upload round trips.
 * voices is 1..32, bank_bytes 1..32 MiB. */
typedef struct ZZ9KVoiceOpenDesc {
  uint32_t voices;
  uint32_t bank_bytes;
  uint32_t lead_frames;
  uint32_t flags;         /* ZZ9K_VOICE_OPEN_FLAG_* */
  uint32_t ring_bytes;
  uint32_t staging_bytes;
} ZZ9KVoiceOpenDesc;

/* What the firmware granted (the decoded OPEN result). */
typedef struct ZZ9KVoiceGrant {
  uint32_t session;
  uint32_t voices;
  uint32_t bank_bytes;
  uint32_t lead_frames;
  uint32_t lead_floor_frames;
  uint32_t lead_max_frames;
  uint32_t ring_records;
  uint32_t output_rate;
  uint32_t max_samples;
} ZZ9KVoiceGrant;

/* A client-side voice session. All members are public so tests and custom
 * mappings can drive one by hand. ring, status and staging are the
 * Amiga-visible pointers to the three shared buffers; the cursor and
 * heartbeat shadow the values the client last stored. */
typedef struct ZZ9KVoiceSession {
  ZZ9KVoiceGrant grant;
  uint32_t ring_handle;
  uint32_t status_handle;
  uint32_t staging_handle;
  uint32_t ring_bytes;
  uint32_t staging_bytes;
  volatile uint8_t *ring;
  volatile uint8_t *status;
  volatile uint8_t *staging;
  uint32_t write_cursor;  /* low 16 bits: the stored ring cursor */
  uint32_t heartbeat;
  uint32_t mapped;        /* open succeeded; the helpers are live */
} ZZ9KVoiceSession;

/* A sample upload: length bytes at data go to bank_offset in the bank and
 * are defined as sample_id in format (ZZ9K_VOICE_FORMAT_*). */
typedef struct ZZ9KVoiceUploadDesc {
  uint32_t sample_id;
  uint32_t bank_offset;
  const void *data;
  uint32_t length;
  uint32_t format;
} ZZ9KVoiceUploadDesc;

/* ---- Pure helpers ---- */

/* Record capacity of a ring of ring_bytes, or 0 when it is too small. */
static inline uint32_t zz9k_voice_ring_records_for_bytes(uint32_t ring_bytes)
{
  uint32_t fit;
  uint32_t records;

  if (ring_bytes < ZZ9K_VOICE_RING_MIN_BYTES) {
    return 0U;
  }
  fit = (ring_bytes - ZZ9K_VOICE_RING_HEADER_BYTES) / ZZ9K_VOICE_RECORD_BYTES;
  records = ZZ9K_VOICE_RING_MIN_RECORDS;
  while (records < ZZ9K_VOICE_RING_MAX_RECORDS && (records << 1) <= fit) {
    records <<= 1;
  }
  return records;
}

/* Records the client may still write: N - (uint16_t)(write - read). A
 * cursor distance above N (a corrupt read cursor) leaves no space. */
static inline uint32_t zz9k_voice_ring_free(uint32_t records,
                                            uint16_t write_cursor,
                                            uint16_t read_cursor)
{
  uint32_t outstanding = (uint16_t)(write_cursor - read_cursor);

  return outstanding > records ? 0U : records - outstanding;
}

static inline int zz9k_voice_host_is_little_endian(void)
{
  const uint16_t one = 1U;

  return *(const uint8_t *)(const void *)&one == 1U;
}

/* One 16-bit big-endian load and store. A single bus cycle even on Zorro II;
 * the pointer must be 2-byte aligned. */
static inline uint16_t zz9k_voice_load_be16(const volatile uint8_t *p)
{
  uint16_t value = *(const volatile uint16_t *)(const volatile void *)p;

  if (zz9k_voice_host_is_little_endian()) {
    value = (uint16_t)((value << 8) | (value >> 8));
  }
  return value;
}

static inline void zz9k_voice_store_be16(volatile uint8_t *p, uint16_t value)
{
  if (zz9k_voice_host_is_little_endian()) {
    value = (uint16_t)((value << 8) | (value >> 8));
  }
  *(volatile uint16_t *)(volatile void *)p = value;
}

/* Reads a 32-bit status word that the firmware may be rewriting. Over Zorro
 * II that is two bus cycles, so a read can tear: read the high half, the low
 * half, the high half again and retry while the high half changed. read16
 * returns the big-endian 16-bit word at a byte offset. The firmware writes
 * the block once per poll, so a handful of retries is far more than a real
 * tear needs; after ZZ9K_VOICE_READ_RETRIES the last composed value stands. */
#define ZZ9K_VOICE_READ_RETRIES 8U

typedef uint16_t (*ZZ9KVoiceRead16)(void *user, uint32_t offset);

static inline uint32_t zz9k_voice_read32_with(ZZ9KVoiceRead16 read16,
                                              void *user, uint32_t offset)
{
  uint32_t attempt;
  uint16_t high;
  uint16_t low;
  uint16_t again;

  for (attempt = 0U;; attempt++) {
    high = read16(user, offset);
    low = read16(user, offset + 2U);
    again = read16(user, offset);
    if (high == again || attempt >= ZZ9K_VOICE_READ_RETRIES) {
      return ((uint32_t)again << 16) | low;
    }
  }
}

static inline uint16_t zz9k_voice_mem_read16(void *user, uint32_t offset)
{
  return zz9k_voice_load_be16((const volatile uint8_t *)user + offset);
}

static inline uint32_t zz9k_voice_read32(const volatile uint8_t *base,
                                         uint32_t offset)
{
  return zz9k_voice_read32_with(zz9k_voice_mem_read16, (void *)(uintptr_t)base,
                                offset);
}

/* Reads the whole status block. Each word is consistent in itself; the
 * words are not a single atomic snapshot. The caller invalidates the CPU
 * cache over the block first when the aperture is cacheable. */
static inline void zz9k_voice_status_read(const volatile uint8_t *block,
                                          ZZ9KVoiceStatus *status)
{
  status->playing_frame =
      zz9k_voice_read32(block, ZZ9K_VOICE_STATUS_PLAYING_FRAME_OFFSET);
  status->horizon = zz9k_voice_read32(block, ZZ9K_VOICE_STATUS_HORIZON_OFFSET);
  status->read_cursor =
      zz9k_voice_load_be16(block + ZZ9K_VOICE_STATUS_READ_CURSOR_OFFSET);
  status->state = zz9k_voice_load_be16(block + ZZ9K_VOICE_STATUS_STATE_OFFSET);
  status->late = zz9k_voice_read32(block, ZZ9K_VOICE_STATUS_LATE_OFFSET);
  status->rejected = zz9k_voice_read32(block, ZZ9K_VOICE_STATUS_REJECTED_OFFSET);
  status->underruns =
      zz9k_voice_read32(block, ZZ9K_VOICE_STATUS_UNDERRUNS_OFFSET);
  status->dropped = zz9k_voice_read32(block, ZZ9K_VOICE_STATUS_DROPPED_OFFSET);
  status->session = zz9k_voice_read32(block, ZZ9K_VOICE_STATUS_SESSION_OFFSET);
}

/* Reads the two jitter words of the status block, with the same tear
 * protection and cache rule as zz9k_voice_status_read. */
static inline void zz9k_voice_jitter_read(const volatile uint8_t *block,
                                          ZZ9KVoiceJitter *jitter)
{
  jitter->margin_min_us =
      zz9k_voice_read32(block, ZZ9K_VOICE_STATUS_MARGIN_OFFSET);
  jitter->poll_gap_max_us =
      zz9k_voice_read32(block, ZZ9K_VOICE_STATUS_POLL_GAP_OFFSET);
}

/* Encode and decode one record (big-endian wire form). */
static inline void zz9k_voice_record_encode(ZZ9KVoiceRecordWire *wire,
                                            const ZZ9KVoiceRecord *record)
{
  zz9k_put_be32(wire->stamp, record->stamp);
  wire->voice = record->voice;
  wire->op = record->op;
  zz9k_put_be16(wire->flags, record->flags);
  zz9k_put_be32(wire->arg0, record->arg0);
  zz9k_put_be32(wire->arg1, record->arg1);
}

static inline void zz9k_voice_record_decode(ZZ9KVoiceRecord *record,
                                            const ZZ9KVoiceRecordWire *wire)
{
  record->stamp = zz9k_get_be32(wire->stamp);
  record->voice = wire->voice;
  record->op = wire->op;
  record->flags = zz9k_get_be16(wire->flags);
  record->arg0 = zz9k_get_be32(wire->arg0);
  record->arg1 = zz9k_get_be32(wire->arg1);
}

/* Record builders. Each fills every field of *record. */
static inline void zz9k_voice_build_record(ZZ9KVoiceRecord *record,
                                           uint32_t stamp, uint8_t voice,
                                           uint8_t op, uint16_t flags,
                                           uint32_t arg0, uint32_t arg1)
{
  record->stamp = stamp;
  record->voice = voice;
  record->op = op;
  record->flags = flags;
  record->arg0 = arg0;
  record->arg1 = arg1;
}

/* length 0 plays the first pass to the sample end. */
static inline void zz9k_voice_build_start(ZZ9KVoiceRecord *record,
                                          uint32_t stamp, uint8_t voice,
                                          uint16_t sample_id, uint32_t offset,
                                          uint32_t length)
{
  zz9k_voice_build_record(record, stamp, voice, ZZ9K_VOICE_OP_START,
                          sample_id, offset, length);
}

/* loop_length 0 means no loop. */
static inline void zz9k_voice_build_loop(ZZ9KVoiceRecord *record,
                                         uint32_t stamp, uint8_t voice,
                                         uint32_t loop_start,
                                         uint32_t loop_length)
{
  zz9k_voice_build_record(record, stamp, voice, ZZ9K_VOICE_OP_LOOP, 0U,
                          loop_start, loop_length);
}

/* flags is a mask of ZZ9K_VOICE_SET_*; rate is a Paula period, or a
 * frequency in Hz with ZZ9K_VOICE_SET_RATE_HZ. */
static inline void zz9k_voice_build_set(ZZ9KVoiceRecord *record,
                                        uint32_t stamp, uint8_t voice,
                                        uint16_t flags, uint32_t rate,
                                        uint16_t left, uint16_t right)
{
  zz9k_voice_build_record(record, stamp, voice, ZZ9K_VOICE_OP_SET, flags,
                          rate, ((uint32_t)left << 16) | right);
}

static inline void zz9k_voice_build_stop(ZZ9KVoiceRecord *record,
                                         uint32_t stamp, uint8_t voice)
{
  zz9k_voice_build_record(record, stamp, voice, ZZ9K_VOICE_OP_STOP, 0U, 0U,
                          0U);
}

/* gain is Q8.8, 256 = unity. */
static inline void zz9k_voice_build_master(ZZ9KVoiceRecord *record,
                                           uint32_t stamp, uint32_t gain)
{
  zz9k_voice_build_record(record, stamp, 0U, ZZ9K_VOICE_OP_MASTER, 0U, gain,
                          0U);
}

static inline void zz9k_voice_build_direction(ZZ9KVoiceRecord *record,
                                              uint32_t stamp, uint8_t voice,
                                              int backward)
{
  zz9k_voice_build_record(record, stamp, voice, ZZ9K_VOICE_OP_DIRECTION,
                          backward ? ZZ9K_VOICE_DIR_BACKWARD : 0U, 0U, 0U);
}

/* Copies up to count records into the ring behind the session's write
 * cursor, bounded by the free space given the firmware's read_cursor, and
 * advances the session's shadow cursor. Nothing shared is published: the
 * stored ring cursor stays where it was, so the firmware cannot see a record
 * before it has landed. zz9k_voice_ring_publish makes them visible. Returns
 * the number of records staged. */
static inline uint32_t zz9k_voice_ring_stage(ZZ9KVoiceSession *session,
                                             uint16_t read_cursor,
                                             const ZZ9KVoiceRecord *records,
                                             uint32_t count)
{
  uint32_t capacity;
  uint32_t space;
  uint32_t i;

  if (!session || !session->mapped || !records) {
    return 0U;
  }
  capacity = session->grant.ring_records;
  space = zz9k_voice_ring_free(capacity, (uint16_t)session->write_cursor,
                               read_cursor);
  if (count > space) {
    count = space;
  }
  for (i = 0U; i < count; i++) {
    ZZ9KVoiceRecordWire wire;
    uint32_t slot = (session->write_cursor + i) & (capacity - 1U);

    zz9k_voice_record_encode(&wire, &records[i]);
    zz9k_audio_ring_copy_to_board(
        session->ring + ZZ9K_VOICE_RING_HEADER_BYTES +
            slot * ZZ9K_VOICE_RECORD_BYTES,
        (const uint8_t *)&wire, ZZ9K_VOICE_RECORD_BYTES);
  }
  session->write_cursor = (uint16_t)(session->write_cursor + count);
  return count;
}

/* Stores the session's write cursor in the ring header in one 16-bit store. */
static inline void zz9k_voice_ring_publish(ZZ9KVoiceSession *session)
{
  if (!session || !session->mapped) {
    return;
  }
  zz9k_voice_store_be16(session->ring + ZZ9K_VOICE_RING_WRITE_CURSOR_OFFSET,
                        (uint16_t)session->write_cursor);
}

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_VOICE_H */
