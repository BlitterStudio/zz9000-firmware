/*
 * Audio descriptor helpers for ZZ9000 SDK v2.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_AUDIO_H
#define ZZ9K_AUDIO_H

#include "zz9k/abi.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int zz9k_audio_sample_format_known(uint32_t format)
{
  return format == ZZ9K_AUDIO_SAMPLE_FORMAT_S16LE ||
         format == ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE;
}

/* Source-rate vocabulary of rate-bearing leases: exactly the
 * qualified conversion table (and the AHI mix-rate table). 48000 is
 * the bypass rate and is always in vocabulary. */
static inline int zz9k_audio_ring_rate_known(uint32_t rate)
{
  return rate == 8000U || rate == 12000U || rate == 24000U ||
         rate == 32000U || rate == 44100U || rate == 48000U;
}

static inline int zz9k_audio_build_decode_desc(
    ZZ9KAudioDecodeDesc *desc,
    uint32_t src_handle,
    uint32_t src_offset,
    uint32_t src_length,
    uint32_t dst_handle,
    uint32_t dst_offset,
    uint32_t dst_capacity,
    uint32_t output_hz,
    uint32_t output_channels,
    uint32_t output_format,
    uint32_t flags)
{
  if (!desc || src_handle == ZZ9K_INVALID_HANDLE || src_length == 0U ||
      dst_handle == ZZ9K_INVALID_HANDLE || dst_capacity == 0U ||
      !zz9k_audio_sample_format_known(output_format) ||
      (output_channels != 0U && output_channels != 1U &&
       output_channels != 2U) ||
      (flags & ~ZZ9K_AUDIO_DECODE_FLAG_EXPECT_END) != 0U) {
    return 0;
  }

  memset(desc, 0, sizeof(*desc));
  desc->src_handle = src_handle;
  desc->src_offset = src_offset;
  desc->src_length = src_length;
  desc->dst_handle = dst_handle;
  desc->dst_offset = dst_offset;
  desc->dst_capacity = dst_capacity;
  desc->output_hz = output_hz;
  desc->output_channels = output_channels;
  desc->output_format = output_format;
  desc->flags = flags;
  return 1;
}

static inline int zz9k_audio_build_stream_begin_desc(
    ZZ9KAudioStreamBeginDesc *desc,
    uint32_t mp3_ring_handle,
    uint32_t mp3_ring_capacity,
    uint32_t pcm_ring_handle,
    uint32_t pcm_ring_capacity,
    uint32_t output_hz,
    uint32_t output_channels,
    uint32_t output_format,
    uint32_t low_water_bytes,
    uint32_t high_water_bytes,
    uint32_t flags)
{
  if (!desc || mp3_ring_handle == ZZ9K_INVALID_HANDLE ||
      mp3_ring_capacity == 0U || pcm_ring_handle == ZZ9K_INVALID_HANDLE ||
      pcm_ring_capacity == 0U ||
      !zz9k_audio_sample_format_known(output_format) ||
      (output_channels != 0U && output_channels != 1U &&
       output_channels != 2U) ||
      /* Both water marks are PCM-ring thresholds: low_water is the
       * playback pump's refill trigger, high_water caps decode output
       * per pass. Neither relates to the compressed input ring. */
      low_water_bytes >= pcm_ring_capacity ||
      high_water_bytes >= pcm_ring_capacity ||
      flags != 0U) {
    return 0;
  }

  memset(desc, 0, sizeof(*desc));
  desc->mp3_ring_handle = mp3_ring_handle;
  desc->mp3_ring_capacity = mp3_ring_capacity;
  desc->pcm_ring_handle = pcm_ring_handle;
  desc->pcm_ring_capacity = pcm_ring_capacity;
  desc->output_hz = output_hz;
  desc->output_channels = output_channels;
  desc->output_format = output_format;
  desc->low_water_bytes = low_water_bytes;
  desc->high_water_bytes = high_water_bytes;
  desc->flags = flags;
  return 1;
}

static inline int zz9k_audio_build_stream_feed_desc(
    ZZ9KAudioStreamFeedDesc *desc,
    uint32_t session,
    uint32_t src_handle,
    uint32_t src_offset,
    uint32_t src_length,
    uint32_t flags)
{
  const uint32_t terminal_flags =
      ZZ9K_AUDIO_STREAM_FEED_EOF | ZZ9K_AUDIO_STREAM_FEED_DRAIN;

  if (!desc || session == 0U || src_handle == ZZ9K_INVALID_HANDLE ||
      (src_length == 0U && (flags & terminal_flags) == 0U) ||
      (flags & ~terminal_flags) != 0U ||
      (flags & terminal_flags) == terminal_flags ||
      ((flags & ZZ9K_AUDIO_STREAM_FEED_DRAIN) != 0U && src_length != 0U)) {
    return 0;
  }

  memset(desc, 0, sizeof(*desc));
  desc->session = session;
  desc->src_handle = src_handle;
  desc->src_offset = src_offset;
  desc->src_length = src_length;
  desc->flags = flags;
  return 1;
}

/* Direct-ring plane (ZZ9K_OP_AUDIO_RING_*). The default acquire is
 * the 48-kHz stereo S16LE bypass lease (flags 0). With
 * ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE the desc also names a
 * source rate from the qualified vocabulary: the lease delivers
 * stereo S16LE at that rate and firmware converts per-slot (requires
 * ZZ9K_SERVICE_FLAG_AUDIO_FABRIC_RATE; older firmware answers
 * BAD_REQUEST, which is the caller's fallback signal). gain is the
 * requested 0..255 producer scale (128 = unity); firmware composes
 * it against the enforced audio ceiling and reports the applied
 * value in the acquire result. */
static inline int zz9k_audio_build_ring_acquire_desc(
    ZZ9KAudioRingAcquireDesc *desc,
    uint32_t slot,
    uint32_t identity,
    uint32_t gain,
    uint32_t flags,
    uint32_t source_rate_hz)
{
  if (!desc || slot == 0U || slot > ZZ9K_AUDIO_RING_SLOT_MAX ||
      identity > ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM ||
      gain > 255U ||
      (flags & ~ZZ9K_AUDIO_RING_ACQUIRE_FLAG_KNOWN) != 0U) {
    return 0;
  }
  if ((flags & ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE) != 0U
          ? !zz9k_audio_ring_rate_known(source_rate_hz)
          : source_rate_hz != 0U) {
    return 0;
  }

  memset(desc, 0, sizeof(*desc));
  desc->slot = slot;
  desc->identity = identity;
  desc->gain = gain;
  desc->flags = flags;
  desc->source_rate_hz = source_rate_hz;
  return 1;
}

/* Release presents the grant's slot and generation exactly as
 * acquired; generation is the revocation token, so a stale
 * (already-revoked) release is rejected here only when the token
 * itself is malformed. */
static inline int zz9k_audio_build_ring_release_desc(
    ZZ9KAudioRingReleaseDesc *desc,
    uint32_t slot,
    uint32_t generation,
    uint32_t flags)
{
  if (!desc || slot == 0U || slot > ZZ9K_AUDIO_RING_SLOT_MAX ||
      generation == 0U || flags != 0U) {
    return 0;
  }

  memset(desc, 0, sizeof(*desc));
  desc->slot = slot;
  desc->generation = generation;
  desc->flags = flags;
  return 1;
}

static inline int zz9k_audio_build_fabric_state_desc(
    ZZ9KAudioFabricStateDesc *desc,
    uint32_t slot,
    uint32_t flags)
{
  if (!desc || slot > ZZ9K_AUDIO_RING_SLOT_MAX ||
      (flags & ~ZZ9K_AUDIO_FABRIC_STATE_HOLD_RESET) != 0U) {
    return 0;
  }

  memset(desc, 0, sizeof(*desc));
  desc->slot = slot;
  desc->flags = flags;
  return 1;
}

/* ---- Shared control-line access (KTD1) ----
 *
 * The control lines are big-endian byte arrays in shared memory, so
 * these helpers work identically on big- and little-endian hosts.
 * Cache maintenance and store ordering across the Zorro bridge are
 * the grant/cache protocol's concern (plan U2/U3); the seqlock makes
 * consumers robust against observing a torn update. */

typedef struct ZZ9KAudioRingProducerSnapshot {
  uint32_t generation;
  uint64_t write_cursor;
  uint32_t heartbeat;
  uint32_t flags;
} ZZ9KAudioRingProducerSnapshot;

typedef struct ZZ9KAudioRingFirmwareSnapshot {
  uint32_t generation;
  uint64_t consumed_cursor;
  uint32_t status;
} ZZ9KAudioRingFirmwareSnapshot;

/* A seqlock read is stable only when the sequence word did not move
 * and settled even: odd means a write is in flight, and a changed
 * value means one completed between the two reads. */
static inline int zz9k_audio_ring_seqlock_stable(uint32_t before,
                                                 uint32_t after)
{
  return before == after && (before & 1U) == 0U;
}

/* Read the producer line into native words. Returns 1 for a stable
 * snapshot, 0 when the read tore (caller retries; the partial
 * snapshot must be treated as untrusted). */
static inline int zz9k_audio_ring_producer_snapshot(
    const volatile ZZ9KAudioRingProducerLine *line,
    ZZ9KAudioRingProducerSnapshot *out)
{
  uint32_t sequence_before;
  uint32_t sequence_after;

  if (!line || !out) {
    return 0;
  }

  sequence_before = zz9k_get_be32(line->sequence);
  out->generation = zz9k_get_be32(line->generation);
  out->write_cursor =
      zz9k_media_u64_from_be(line->write_cursor_hi, line->write_cursor_lo);
  out->heartbeat = zz9k_get_be32(line->heartbeat);
  out->flags = zz9k_get_be32(line->flags);
  sequence_after = zz9k_get_be32(line->sequence);
  return zz9k_audio_ring_seqlock_stable(sequence_before, sequence_after);
}

/* Commit one producer update: mark the line in flight (odd), publish
 * generation/cursor/heartbeat/flags, then release at the next even
 * sequence. The commit target is computed as the next even word, so
 * a writer recovering from an odd (torn or crashed mid-update)
 * sequence re-establishes the even-resting invariant instead of
 * preserving the broken parity forever -- and never rests the line
 * odd. The heartbeat token is the caller's monotonic refresh value
 * -- any change keeps the lease live (R11). */
static inline void zz9k_audio_ring_producer_publish(
    volatile ZZ9KAudioRingProducerLine *line,
    uint32_t generation,
    uint64_t write_cursor,
    uint32_t heartbeat,
    uint32_t flags)
{
  uint32_t next = (zz9k_get_be32(line->sequence) + 2U) & ~1U;

  zz9k_put_be32(line->sequence, next | 1U);
  zz9k_put_be32(line->generation, generation);
  zz9k_media_u64_to_be(line->write_cursor_hi, line->write_cursor_lo,
                       write_cursor);
  zz9k_put_be32(line->heartbeat, heartbeat);
  zz9k_put_be32(line->flags, flags);
  zz9k_put_be32(line->sequence, next);
}

/* Read the firmware line (ring credit). Returns 1 for a stable
 * snapshot, 0 when the read tore; a snapshot whose generation
 * differs from the active lease describes a revoked lease and must
 * be discarded by the caller (R4). */
static inline int zz9k_audio_ring_firmware_snapshot(
    const volatile ZZ9KAudioRingFirmwareLine *line,
    ZZ9KAudioRingFirmwareSnapshot *out)
{
  uint32_t sequence_before;
  uint32_t sequence_after;

  if (!line || !out) {
    return 0;
  }

  sequence_before = zz9k_get_be32(line->sequence);
  out->generation = zz9k_get_be32(line->generation);
  out->consumed_cursor = zz9k_media_u64_from_be(line->consumed_cursor_hi,
                                                line->consumed_cursor_lo);
  out->status = zz9k_get_be32(line->status);
  sequence_after = zz9k_get_be32(line->sequence);
  return zz9k_audio_ring_seqlock_stable(sequence_before, sequence_after);
}

/* Commit one firmware update: consumed cursor (ring credit) and
 * status under the generation the credits belong to. Commits target
 * the next even sequence word (see the producer publish above), so
 * the line always rests even. */
static inline void zz9k_audio_ring_firmware_publish(
    volatile ZZ9KAudioRingFirmwareLine *line,
    uint32_t generation,
    uint64_t consumed_cursor,
    uint32_t status)
{
  uint32_t next = (zz9k_get_be32(line->sequence) + 2U) & ~1U;

  zz9k_put_be32(line->sequence, next | 1U);
  zz9k_put_be32(line->generation, generation);
  zz9k_media_u64_to_be(line->consumed_cursor_hi, line->consumed_cursor_lo,
                       consumed_cursor);
  zz9k_put_be32(line->status, status);
  zz9k_put_be32(line->sequence, next);
}

/* A grant is usable only when it is self-consistent: a leaseable
 * slot inside the advertised count, a nonzero generation, the fixed
 * 3840-byte/20-ms period geometry and a known sample contract with
 * a matching source rate (48000 for the bypass contract, a
 * qualified-vocabulary rate for the source-rate contract), a ring
 * capacity that is a whole number of periods, a cache-line aligned
 * control block that does not overlap the ring, and no unknown
 * result flags. SDK clients validate before dereferencing any
 * granted pointer. The reply decoder normalizes the immediately
 * preceding firmware's zero encoding of the reserved source-rate
 * word to 48000 for bypass grants before calling this, so a legacy
 * grant never reaches here with a zero rate. */
static inline int zz9k_audio_ring_grant_valid(
    const ZZ9KAudioRingAcquireResult *grant)
{
  uint64_t ring_end;
  uint64_t control_end;

  if (!grant || grant->slot == 0U ||
      grant->slot > ZZ9K_AUDIO_RING_SLOT_MAX ||
      grant->slot > grant->slot_count ||
      grant->slot_count == 0U ||
      grant->slot_count > ZZ9K_AUDIO_RING_SLOT_MAX ||
      grant->generation == 0U ||
      grant->period_bytes != ZZ9K_AUDIO_RING_PERIOD_BYTES ||
      grant->period_us != ZZ9K_AUDIO_RING_PERIOD_US ||
      grant->gain_applied > 255U ||
      (grant->flags & ~(ZZ9K_AUDIO_RING_RESULT_GAIN_BOUNDED |
                        ZZ9K_AUDIO_RING_RESULT_BUS_ZORRO2)) != 0U) {
    return 0;
  }

  if (grant->sample_contract ==
      ZZ9K_AUDIO_RING_CONTRACT_48K_STEREO_S16LE) {
    if (grant->source_rate != 48000U) {
      return 0;
    }
  } else if (grant->sample_contract ==
             ZZ9K_AUDIO_RING_CONTRACT_SOURCE_RATE_STEREO_S16LE) {
    if (!zz9k_audio_ring_rate_known(grant->source_rate)) {
      return 0;
    }
  } else {
    return 0;
  }

  if (grant->ring_capacity == 0U ||
      (grant->ring_capacity % ZZ9K_AUDIO_RING_PERIOD_BYTES) != 0U ||
      (grant->ring_offset & 3U) != 0U ||
      (grant->control_offset % ZZ9K_AUDIO_RING_CONTROL_ALIGN) != 0U) {
    return 0;
  }

  ring_end = (uint64_t)grant->ring_offset + grant->ring_capacity;
  control_end = (uint64_t)grant->control_offset +
                ZZ9K_AUDIO_RING_CONTROL_SIZE;
  if ((uint64_t)grant->control_offset < ring_end &&
      (uint64_t)grant->ring_offset < control_end) {
    return 0;
  }

  return 1;
}

/* A grant matches its request only when the sample contract and
 * source rate are exactly what was asked for: a SOURCE_RATE request
 * gets contract 2 at the requested rate; a flags-zero bypass request
 * gets contract 1 at the normalized 48000. The reply decoder's
 * legacy-zero normalization runs before this, so a bypass grant never
 * carries a zero rate here. */
static inline int zz9k_audio_ring_grant_matches_desc(
    const ZZ9KAudioRingAcquireDesc *desc,
    const ZZ9KAudioRingAcquireResult *grant)
{
  if (!desc || !grant) {
    return 0;
  }
  if ((desc->flags & ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE) != 0U) {
    return grant->sample_contract ==
               ZZ9K_AUDIO_RING_CONTRACT_SOURCE_RATE_STEREO_S16LE &&
           grant->source_rate == desc->source_rate_hz;
  }
  return grant->sample_contract ==
             ZZ9K_AUDIO_RING_CONTRACT_48K_STEREO_S16LE &&
         grant->source_rate == 48000U;
}

/* The outstanding distance is write-minus-consumed: a backward
 * cursor or a distance above the ring capacity is a fault on that
 * generation only (KTD4). */
static inline int zz9k_audio_ring_distance_valid(
    uint64_t write_cursor,
    uint64_t consumed_cursor,
    uint32_t capacity)
{
  return write_cursor >= consumed_cursor &&
         (write_cursor - consumed_cursor) <= (uint64_t)capacity;
}


/* ---- Client session helpers (plan U4) ----
 *
 * One ZZ9KAudioRingSession (zz9k/abi.h) carries everything an
 * independent producer task needs after acquisition: the validated
 * grant, the mapped ring and control lines, and the producer's own
 * cursor state. All data-path helpers below are pure shared-memory
 * work -- no function here enters the synchronous mailbox path
 * (R5, KTD3), so two sessions in two tasks never serialize.
 * zz9k_open'd contexts acquire and release sessions through
 * zz9k_audio_ring_session_begin/_end in the host layer. */
/* Platform cache maintenance for host-visible board memory. AmigaOS may map
 * the Zorro III aperture cacheable; producer PCM/control writes must reach the
 * card before cursor publication, and firmware credits must be invalidated
 * before polling. Non-Amiga host builds implement these as no-ops. */
void zz9k_audio_ring_cache_flush(const volatile void *address, uint32_t length);
void zz9k_audio_ring_cache_invalidate(const volatile void *address,
                                      uint32_t length);


/* PCM bytes the producer has staged but firmware has not yet taken
 * credit for: write minus consumed (R7). */
static inline uint32_t zz9k_audio_ring_outstanding(
    const ZZ9KAudioRingSession *session)
{
  if (!session) {
    return 0U;
  }
  return (uint32_t)(session->write_cursor - session->consumed_cursor);
}

/* Ring bytes the producer may stage right now (R7: the consumed
 * cursor is the credit). */
static inline uint32_t zz9k_audio_ring_free_bytes(
    const ZZ9KAudioRingSession *session)
{
  if (!session) {
    return 0U;
  }
  return session->grant.ring_capacity - zz9k_audio_ring_outstanding(session);
}
/* Copy into board memory with naturally aligned longword stores. Byte-at-a-
 * time volatile stores become individual Zorro transactions on m68k and
 * cannot sustain two 192-KiB/s producers. Loading through memcpy keeps the
 * source alignment/aliasing contract unrestricted while each 32-bit store
 * preserves the four source bytes on both big- and little-endian hosts. */
static inline void zz9k_audio_ring_copy_to_board(volatile uint8_t *dst,
                                                 const uint8_t *src,
                                                 uint32_t length)
{
  uint32_t offset = 0U;

  while (offset < length &&
         (((uintptr_t)(dst + offset) & (uintptr_t)3U) != 0U)) {
    dst[offset] = src[offset];
    offset++;
  }
  while (length - offset >= 4U) {
    uint32_t word;

    memcpy(&word, src + offset, sizeof(word));
    *(volatile uint32_t *)(void *)(dst + offset) = word;
    offset += 4U;
  }
  while (offset < length) {
    dst[offset] = src[offset];
    offset++;
  }
}


/* Stage PCM into the ring at the write cursor, wrapping at the ring
 * end, bounded by the currently credited free space. The bytes are
 * copied and the session-local cursor advances, but nothing shared
 * is published: the matching zz9k_audio_ring_publish() call is what
 * makes the bytes visible to firmware, so ordering (R6) is
 * structural -- a cursor never moves in shared memory before its
 * PCM lands. Returns the number of bytes staged (0 when the session
 * is dead or the ring has no credited space). */
static inline uint32_t zz9k_audio_ring_write(ZZ9KAudioRingSession *session,
                                             const void *pcm,
                                             uint32_t length)
{
  uint32_t staged;
  uint32_t offset;
  uint32_t first;
  const uint8_t *src;

  if (!session || !session->mapped || !pcm) {
    return 0U;
  }
  if (length > zz9k_audio_ring_free_bytes(session)) {
    length = zz9k_audio_ring_free_bytes(session);
  }
  offset = (uint32_t)(session->write_cursor % session->grant.ring_capacity);
  first = session->grant.ring_capacity - offset;
  if (first > length) {
    first = length;
  }

  src = (const uint8_t *)pcm;
  zz9k_audio_ring_copy_to_board(session->ring + offset, src, first);
  zz9k_audio_ring_copy_to_board(session->ring, src + first, length - first);
  staged = length;
  zz9k_audio_ring_cache_flush(session->ring + offset, first);
  if (staged > first) {
    zz9k_audio_ring_cache_flush(session->ring, staged - first);
  }


  session->write_cursor += staged;
  return staged;
}

/* Commit the producer control line under the seqlock: the current
 * write cursor, the refreshed heartbeat token, and the session's
 * producer flags. Called after zz9k_audio_ring_write() (R6); with no
 * new PCM staged it is a pure heartbeat refresh, which is exactly
 * how a paused producer stays live (R11-R12). */
static inline void zz9k_audio_ring_publish(ZZ9KAudioRingSession *session)
{
  if (!session || !session->mapped) {
    return;
  }
  session->heartbeat++;
  zz9k_audio_ring_producer_publish(session->producer_line,
                                   session->grant.generation,
                                   session->write_cursor,
                                   session->heartbeat,
                                   session->flags);
  zz9k_audio_ring_cache_flush(session->producer_line,
                              ZZ9K_AUDIO_RING_CONTROL_LINE_SIZE);

}

/* Adopt firmware's consumed cursor as ring credit: one bounded,
 * tearing-safe snapshot of the firmware line (R7). Only a stable
 * snapshot carrying this session's generation, an OK status, and a
 * cursor distance inside KTD4 is adopted; anything else is REVOKED
 * and the session stops writing. This read -- not STATE_GET -- is
 * the producer's backpressure. */
static inline int zz9k_audio_ring_take_credits(
    ZZ9KAudioRingSession *session,
    uint32_t retries)
{
  ZZ9KAudioRingFirmwareSnapshot snapshot;
  uint32_t attempt;

  if (!session || !session->mapped) {
    return ZZ9K_AUDIO_RING_CREDIT_REVOKED;
  }
  for (attempt = 0U;; attempt++) {
    zz9k_audio_ring_cache_invalidate(session->firmware_line,
                                     ZZ9K_AUDIO_RING_CONTROL_LINE_SIZE);
    if (zz9k_audio_ring_firmware_snapshot(session->firmware_line,
                                          &snapshot)) {
      if (snapshot.generation != session->grant.generation ||
          snapshot.status != ZZ9K_AUDIO_RING_STATUS_OK ||
          !zz9k_audio_ring_distance_valid(session->write_cursor,
                                          snapshot.consumed_cursor,
                                          session->grant.ring_capacity)) {
        return ZZ9K_AUDIO_RING_CREDIT_REVOKED;
      }
      session->consumed_cursor = snapshot.consumed_cursor;
      return ZZ9K_AUDIO_RING_CREDIT_OK;
    }
    if (attempt >= retries) {
      return ZZ9K_AUDIO_RING_CREDIT_RETRY;
    }
  }
}

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_AUDIO_H */
