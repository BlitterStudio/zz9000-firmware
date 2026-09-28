/*
 * Direct-ring ABI and request-builder host tests (plan U1).
 *
 * Covers the vocabulary the direct-ring transport rests on: acquire/
 * release/state descriptor validation, big-endian request packing,
 * grant decoding (malformed geometry, alignment, overlap, unknown
 * flags), the shared control-line seqlock helpers (unstable reads,
 * 64-bit cursors across the 32-bit boundary), generation binding and
 * the write-minus-consumed distance rule.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/abi.h"
#include "zz9k/audio.h"
#include "zz9k/reply.h"
#include "zz9k/request.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(ok, name)                                                 \
  do {                                                                  \
    if (!(ok)) {                                                        \
      g_failures++;                                                     \
      printf("FAILED: %s (%s:%d)\n", (name), __FILE__, __LINE__);       \
    }                                                                   \
  } while (0)

/* ---- pure builder/validation layer ---- */

static int test_builders(void)
{
  ZZ9KAudioRingAcquireDesc acquire;
  ZZ9KAudioRingReleaseDesc release;
  ZZ9KAudioFabricStateDesc state;

  if (!zz9k_audio_build_ring_acquire_desc(&acquire, 1U, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM, 128U, 0U, 0U) ||
      acquire.slot != 1U ||
      acquire.identity != ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM ||
      acquire.gain != 128U || acquire.flags != 0U) {
    return 1;
  }
  if (!zz9k_audio_build_ring_acquire_desc(&acquire, ZZ9K_AUDIO_RING_SLOT_MAX, 0U, 255U, 0U, 0U)) {
    return 2;
  }
  /* Slot 0 is the pump; the last leaseable slot is SLOT_MAX. */
  if (zz9k_audio_build_ring_acquire_desc(&acquire, 0U, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM, 128U, 0U, 0U)) {
    return 3;
  }
  if (zz9k_audio_build_ring_acquire_desc(&acquire, ZZ9K_AUDIO_RING_SLOT_MAX + 1U, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM, 128U, 0U, 0U)) {
    return 4;
  }
  if (zz9k_audio_build_ring_acquire_desc(&acquire, 1U, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM + 1U, 128U, 0U, 0U)) {
    return 5;
  }
  if (zz9k_audio_build_ring_acquire_desc(&acquire, 1U, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM, 256U, 0U, 0U)) {
    return 6;
  }
  /* Unknown acquire flags are rejected (bit 0 is the known
   * SOURCE_RATE request flag; bit 1+ are undefined). */
  if (zz9k_audio_build_ring_acquire_desc(&acquire, 1U, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM, 128U, 1U << 1, 0U)) {
    return 7;
  }
  /* Rate-bearing acquires: flag + a vocabulary rate is accepted;
   * off-vocabulary rates and a rate without the flag are rejected. */
  if (!zz9k_audio_build_ring_acquire_desc(&acquire, 1U, ZZ9K_AUDIO_METER_IDENTITY_AHI, 128U, ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE, 44100U) ||
      acquire.flags != ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE ||
      acquire.source_rate_hz != 44100U) {
    return 16;
  }
  if (zz9k_audio_build_ring_acquire_desc(&acquire, 1U, ZZ9K_AUDIO_METER_IDENTITY_AHI, 128U, ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE, 44101U)) {
    return 17;
  }
  if (zz9k_audio_build_ring_acquire_desc(&acquire, 1U, ZZ9K_AUDIO_METER_IDENTITY_AHI, 128U, ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE, 0U)) {
    return 18;
  }
  if (zz9k_audio_build_ring_acquire_desc(&acquire, 1U, ZZ9K_AUDIO_METER_IDENTITY_AHI, 128U, 0U, 44100U)) {
    return 19;
  }
  if (zz9k_audio_build_ring_acquire_desc(0, 1U, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM, 128U, 0U, 0U)) {
    return 8;
  }

  if (!zz9k_audio_build_ring_release_desc(&release, 2U, 0x21U, 0U) ||
      release.slot != 2U || release.generation != 0x21U ||
      release.flags != 0U) {
    return 9;
  }
  if (zz9k_audio_build_ring_release_desc(&release, 0U, 0x21U, 0U)) {
    return 10;
  }
  if (zz9k_audio_build_ring_release_desc(&release, 3U, 0x21U, 0U)) {
    return 11;
  }
  /* Generation 0 is never granted: it is the revoked/free token. */
  if (zz9k_audio_build_ring_release_desc(&release, 1U, 0U, 0U)) {
    return 12;
  }
  if (zz9k_audio_build_ring_release_desc(&release, 1U, 0x21U, 1U)) {
    return 13;
  }

  if (!zz9k_audio_build_fabric_state_desc(&state, 1U,
                                          ZZ9K_AUDIO_FABRIC_STATE_HOLD_RESET) ||
      state.slot != 1U ||
      state.flags != ZZ9K_AUDIO_FABRIC_STATE_HOLD_RESET) {
    return 14;
  }
  if (zz9k_audio_build_fabric_state_desc(&state, 3U, 0U) ||
      zz9k_audio_build_fabric_state_desc(&state, 1U, 2U)) {
    return 15;
  }
  return 0;
}

/* ---- request packing ---- */

static int test_request_helpers(void)
{
  ZZ9KAudioRingAcquireDesc acquire;
  ZZ9KRequest request;
  uint32_t i;

  if (!zz9k_audio_build_ring_acquire_desc(&acquire, 2U, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM, 100U, 0U, 0U)) {
    return 1;
  }
  if (zz9k_request_audio_ring_acquire(&request, &acquire) !=
          ZZ9K_STATUS_OK ||
      request.entry.opcode != ZZ9K_OP_AUDIO_RING_ACQUIRE ||
      request.entry.payload_len != 48U ||
      zz9k_get_be32(&request.entry.payload.inline_data[0]) != 2U ||
      zz9k_get_be32(&request.entry.payload.inline_data[4]) !=
          ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM ||
      zz9k_get_be32(&request.entry.payload.inline_data[8]) != 100U ||
      zz9k_get_be32(&request.entry.payload.inline_data[12]) != 0U) {
    return 2;
  }
  for (i = 16U; i < 48U; i += 4U) {
    if (zz9k_get_be32(&request.entry.payload.inline_data[i]) != 0U) {
      return 3;
    }
  }
  /* A rate-bearing acquire packs the flag and the carved rate word;
   * words 20+ stay zero (reserved). */
  if (!zz9k_audio_build_ring_acquire_desc(&acquire, 1U, ZZ9K_AUDIO_METER_IDENTITY_AHI, 128U, ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE, 44100U) ||
      zz9k_request_audio_ring_acquire(&request, &acquire) != ZZ9K_STATUS_OK ||
      zz9k_get_be32(&request.entry.payload.inline_data[12]) !=
          ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE ||
      zz9k_get_be32(&request.entry.payload.inline_data[16]) != 44100U) {
    return 20;
  }
  for (i = 20U; i < 48U; i += 4U) {
    if (zz9k_get_be32(&request.entry.payload.inline_data[i]) != 0U) {
      return 21;
    }
  }
  if (zz9k_request_audio_ring_acquire(0, &acquire) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 4;
  }
  acquire.slot = 0U;
  if (zz9k_request_audio_ring_acquire(&request, &acquire) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 5;
  }

  if (zz9k_request_audio_ring_release(&request, 1U, 7U, 0U) !=
          ZZ9K_STATUS_OK ||
      request.entry.opcode != ZZ9K_OP_AUDIO_RING_RELEASE ||
      request.entry.payload_len != 48U ||
      zz9k_get_be32(&request.entry.payload.inline_data[0]) != 1U ||
      zz9k_get_be32(&request.entry.payload.inline_data[4]) != 7U ||
      zz9k_get_be32(&request.entry.payload.inline_data[8]) != 0U) {
    return 6;
  }
  if (zz9k_request_audio_ring_release(&request, 1U, 0U, 0U) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 7;
  }
  if (zz9k_request_audio_ring_release(&request, 3U, 7U, 0U) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 8;
  }
  if (zz9k_request_audio_ring_release(0, 1U, 7U, 0U) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 9;
  }

  {
    ZZ9KAudioFabricStateDesc state;

    if (!zz9k_audio_build_fabric_state_desc(&state, 1U, 0U)) {
      return 10;
    }
    if (zz9k_request_audio_fabric_state_get(&request, &state) !=
            ZZ9K_STATUS_OK ||
        request.entry.opcode != ZZ9K_OP_AUDIO_FABRIC_STATE_GET ||
        request.entry.payload_len != 48U ||
        zz9k_get_be32(&request.entry.payload.inline_data[0]) != 1U ||
        zz9k_get_be32(&request.entry.payload.inline_data[4]) != 0U) {
      return 11;
    }
  }
  return 0;
}

/* ---- reply decoding: the generation-bound grant ---- */

#define Z3_RING_OFFSET    0x00040000UL
#define Z3_RING_CAPACITY  (32U * ZZ9K_AUDIO_RING_PERIOD_BYTES)
#define Z3_CONTROL_OFFSET 0x0003f000UL

static void put_acquire_reply(ZZ9KMailboxEntry *reply,
                              uint32_t slot,
                              uint32_t generation,
                              uint32_t ring_offset,
                              uint32_t ring_capacity,
                              uint32_t control_offset,
                              uint32_t gain_applied,
                              uint32_t slot_count,
                              uint32_t flags)
{
  memset(reply, 0, sizeof(*reply));
  reply->opcode = ZZ9K_OP_AUDIO_RING_ACQUIRE;
  reply->status = ZZ9K_STATUS_OK;
  reply->payload_len = sizeof(ZZ9KAudioRingAcquireResultPayload);
  zz9k_put_be32(&reply->payload.inline_data[0], slot);
  zz9k_put_be32(&reply->payload.inline_data[4], generation);
  zz9k_put_be32(&reply->payload.inline_data[8], ring_offset);
  zz9k_put_be32(&reply->payload.inline_data[12], ring_capacity);
  zz9k_put_be32(&reply->payload.inline_data[16], control_offset);
  zz9k_put_be32(&reply->payload.inline_data[20],
                ZZ9K_AUDIO_RING_PERIOD_BYTES);
  zz9k_put_be32(&reply->payload.inline_data[24], ZZ9K_AUDIO_RING_PERIOD_US);
  zz9k_put_be32(&reply->payload.inline_data[28],
                ZZ9K_AUDIO_RING_CONTRACT_48K_STEREO_S16LE);
  zz9k_put_be32(&reply->payload.inline_data[32], gain_applied);
  zz9k_put_be32(&reply->payload.inline_data[36], slot_count);
  zz9k_put_be32(&reply->payload.inline_data[40], flags);
  /* Both contracts populate the granted source rate (bypass = 48000). */
  zz9k_put_be32(&reply->payload.inline_data[44], 48000U);
}

static int test_acquire_reply(void)
{
  ZZ9KAudioRingAcquireResult grant;
  ZZ9KMailboxEntry reply;

  /* Zorro III grant: two leaseable slots, full-size ring. */
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 128U, 2U, 0U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
          ZZ9K_STATUS_OK ||
      grant.slot != 1U || grant.generation != 5U ||
      grant.ring_offset != Z3_RING_OFFSET ||
      grant.ring_capacity != Z3_RING_CAPACITY ||
      grant.control_offset != Z3_CONTROL_OFFSET ||
      grant.period_bytes != ZZ9K_AUDIO_RING_PERIOD_BYTES ||
      grant.gain_applied != 128U || grant.slot_count != 2U ||
      grant.source_rate != 48000U ||
      grant.flags != 0U) {
    return 1;
  }
  /* Legacy firmware left the reserved source-rate word at zero for
   * bypass grants; the decoder normalizes it to the bypass rate. */
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 128U, 2U, 0U);
  zz9k_put_be32(&reply.payload.inline_data[44], 0U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
          ZZ9K_STATUS_OK ||
      grant.sample_contract !=
          ZZ9K_AUDIO_RING_CONTRACT_48K_STEREO_S16LE ||
      grant.source_rate != 48000U) {
    return 19; /* legacy zero source rate not normalized to 48000 */
  }

  /* The zero encoding is contract-1 only: a contract-2 grant with a
   * zero rate stays off-vocabulary and malformed. */
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 128U, 2U, 0U);
  zz9k_put_be32(&reply.payload.inline_data[28],
                ZZ9K_AUDIO_RING_CONTRACT_SOURCE_RATE_STEREO_S16LE);
  zz9k_put_be32(&reply.payload.inline_data[44], 0U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 20; /* contract-2 zero source rate stays malformed */
  }

  /* Zorro II compact grant: one slot, bounded gain flagged. */
  put_acquire_reply(&reply, 1U, 9U, 0x1000U,
                    12U * ZZ9K_AUDIO_RING_PERIOD_BYTES, 0x800U, 96U, 1U,
                    ZZ9K_AUDIO_RING_RESULT_BUS_ZORRO2 |
                        ZZ9K_AUDIO_RING_RESULT_GAIN_BOUNDED);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
          ZZ9K_STATUS_OK ||
      grant.slot_count != 1U || grant.gain_applied != 96U ||
      (grant.flags & ZZ9K_AUDIO_RING_RESULT_BUS_ZORRO2) == 0U) {
    return 2;
  }

  /* Malformed grants must never reach the client. */
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 128U, 2U, 1U << 2);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 3; /* unknown result flags */
  }
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 128U, 2U, 0U);
  zz9k_put_be32(&reply.payload.inline_data[20], 3844U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 4; /* malformed period size */
  }
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 128U, 2U, 0U);
  zz9k_put_be32(&reply.payload.inline_data[24], 19999U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 5; /* malformed period duration */
  }
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 128U, 2U, 0U);
  zz9k_put_be32(&reply.payload.inline_data[28], 3U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 6; /* unknown sample contract */
  }
  /* A contract-2 grant decodes with its validated source rate. */
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 128U, 2U, 0U);
  zz9k_put_be32(&reply.payload.inline_data[28],
                ZZ9K_AUDIO_RING_CONTRACT_SOURCE_RATE_STEREO_S16LE);
  zz9k_put_be32(&reply.payload.inline_data[44], 44100U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
          ZZ9K_STATUS_OK ||
      grant.sample_contract !=
          ZZ9K_AUDIO_RING_CONTRACT_SOURCE_RATE_STEREO_S16LE ||
      grant.source_rate != 44100U) {
    return 17; /* contract-2 grant with a vocabulary rate */
  }
  /* A contract-2 grant with an off-vocabulary rate is malformed. */
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 128U, 2U, 0U);
  zz9k_put_be32(&reply.payload.inline_data[28], 2U);
  zz9k_put_be32(&reply.payload.inline_data[44], 44101U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 18; /* contract-2 grant with an off-vocabulary rate */
  }
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET,
                    Z3_RING_CAPACITY + 2U, Z3_CONTROL_OFFSET, 128U, 2U, 0U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 7; /* capacity not a whole number of periods */
  }
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, 0U, Z3_CONTROL_OFFSET,
                    128U, 2U, 0U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 8; /* empty ring */
  }
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET + 4U, 128U, 2U, 0U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 9; /* misaligned control block */
  }
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_RING_OFFSET + 16U, 128U, 2U, 0U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 10; /* control block overlaps the ring */
  }
  put_acquire_reply(&reply, 0U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 128U, 2U, 0U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 11; /* the pump slot is never granted */
  }
  put_acquire_reply(&reply, 2U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 128U, 1U, 0U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 12; /* slot above the advertised count */
  }
  put_acquire_reply(&reply, 1U, 0U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 128U, 2U, 0U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 13; /* generation 0 is never granted */
  }
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 256U, 2U, 0U);
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 14; /* gain out of range */
  }
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 128U, 2U, 0U);
  reply.payload_len = 44U;
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 15; /* short payload */
  }
  put_acquire_reply(&reply, 1U, 5U, Z3_RING_OFFSET, Z3_RING_CAPACITY,
                    Z3_CONTROL_OFFSET, 128U, 2U, 0U);
  reply.opcode = ZZ9K_OP_AUDIO_RING_RELEASE;
  if (zz9k_reply_audio_ring_acquire_result(&reply, &grant) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 16; /* wrong opcode */
  }
  if (zz9k_reply_audio_ring_acquire_result(0, &grant) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 17;
  }
  return 0;
}

/* ---- reply decoding: framed slot state ---- */

static void put_state_reply(ZZ9KMailboxEntry *reply,
                            uint32_t slot,
                            uint32_t generation,
                            uint32_t state,
                            uint32_t heartbeat_ms,
                            uint64_t cursor_write,
                            uint64_t cursor_read,
                            uint32_t starvation,
                            uint32_t flags)
{
  memset(reply, 0, sizeof(*reply));
  reply->opcode = ZZ9K_OP_AUDIO_FABRIC_STATE_GET;
  reply->status = ZZ9K_STATUS_OK;
  reply->payload_len = sizeof(ZZ9KAudioFabricStateResultPayload);
  zz9k_put_be32(&reply->payload.inline_data[0], slot);
  zz9k_put_be32(&reply->payload.inline_data[4], generation);
  zz9k_put_be32(&reply->payload.inline_data[8], state);
  zz9k_put_be32(&reply->payload.inline_data[12], heartbeat_ms);
  zz9k_put_be32(&reply->payload.inline_data[16],
                (uint32_t)(cursor_write >> 32));
  zz9k_put_be32(&reply->payload.inline_data[20], (uint32_t)cursor_write);
  zz9k_put_be32(&reply->payload.inline_data[24],
                (uint32_t)(cursor_read >> 32));
  zz9k_put_be32(&reply->payload.inline_data[28], (uint32_t)cursor_read);
  zz9k_put_be32(&reply->payload.inline_data[32], starvation);
  zz9k_put_be32(&reply->payload.inline_data[36], flags);
  zz9k_put_be32(&reply->payload.inline_data[40], 0x00018000U);
  zz9k_put_be32(&reply->payload.inline_data[44], 2U);
}

static int test_state_reply(void)
{
  ZZ9KAudioFabricStateResult state;
  ZZ9KMailboxEntry reply;
  const uint64_t write_past_32_bits =
      (UINT64_C(1) << 32) + UINT64_C(0x80000010);

  put_state_reply(&reply, 1U, 5U, ZZ9K_AUDIO_FABRIC_SLOT_ACTIVE, 120U,
                  write_past_32_bits, write_past_32_bits - 7680U, 0U, 0U);
  if (zz9k_reply_audio_fabric_state_result(&reply, &state) !=
          ZZ9K_STATUS_OK ||
      state.slot != 1U || state.generation != 5U ||
      state.state != ZZ9K_AUDIO_FABRIC_SLOT_ACTIVE ||
      state.heartbeat_ms != 120U ||
      state.cursor_write != write_past_32_bits ||
      state.cursor_read != write_past_32_bits - 7680U ||
      state.starvation_count != 0U || state.flags != 0U ||
      state.peak != 0x00018000U || state.clip != 2U) {
    return 1;
  }
  if (!zz9k_audio_ring_distance_valid(state.cursor_write,
                                      state.cursor_read, Z3_RING_CAPACITY)) {
    return 2;
  }

  /* REVOKED is reportable: the generation was invalidated. */
  put_state_reply(&reply, 1U, 5U, ZZ9K_AUDIO_FABRIC_SLOT_REVOKED,
                  ZZ9K_AUDIO_RING_HEARTBEAT_UNKNOWN, 0U, 0U, 0U,
                  ZZ9K_AUDIO_FABRIC_STATE_HOLD_RESET);
  if (zz9k_reply_audio_fabric_state_result(&reply, &state) !=
          ZZ9K_STATUS_OK ||
      state.state != ZZ9K_AUDIO_FABRIC_SLOT_REVOKED ||
      state.flags != ZZ9K_AUDIO_FABRIC_STATE_HOLD_RESET) {
    return 3;
  }

  put_state_reply(&reply, 1U, 5U, ZZ9K_AUDIO_FABRIC_SLOT_ACTIVE, 120U, 0U,
                  0U, 0U, 1U << 1);
  if (zz9k_reply_audio_fabric_state_result(&reply, &state) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 4; /* unknown state flags */
  }
  put_state_reply(&reply, 3U, 5U, ZZ9K_AUDIO_FABRIC_SLOT_ACTIVE, 120U, 0U,
                  0U, 0U, 0U);
  if (zz9k_reply_audio_fabric_state_result(&reply, &state) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 5; /* slot out of range */
  }
  put_state_reply(&reply, 1U, 5U, 4U, 120U, 0U, 0U, 0U, 0U);
  if (zz9k_reply_audio_fabric_state_result(&reply, &state) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 6; /* unknown state value */
  }
  return 0;
}

/* ---- shared control lines: seqlock vocabulary ---- */

static int test_control_lines(void)
{
  ZZ9KAudioRingProducerLine producer;
  ZZ9KAudioRingFirmwareLine firmware;
  ZZ9KAudioRingProducerSnapshot produced;
  ZZ9KAudioRingFirmwareSnapshot credited;
  const uint64_t cursor_past_32_bits =
      ((uint64_t)0x9U << 32) | UINT64_C(0x80000010);

  /* Seqlock stability vocabulary: even and unchanged. */
  if (!zz9k_audio_ring_seqlock_stable(2U, 2U)) {
    return 1;
  }
  if (zz9k_audio_ring_seqlock_stable(3U, 3U)) {
    return 2; /* odd: an update is in flight */
  }
  if (zz9k_audio_ring_seqlock_stable(2U, 4U)) {
    return 3; /* changed: a commit happened between the reads */
  }

  memset(&producer, 0, sizeof(producer));
  zz9k_audio_ring_producer_publish(&producer, 6U, cursor_past_32_bits, 77U,
                                   0U);
  if (zz9k_get_be32(producer.sequence) != 2U) {
    return 4; /* even after commit */
  }
  if (!zz9k_audio_ring_producer_snapshot(&producer, &produced)) {
    return 5;
  }
  if (produced.generation != 6U || produced.write_cursor != cursor_past_32_bits ||
      produced.heartbeat != 77U || produced.flags != 0U) {
    return 6;
  }

  /* A second publish advances the cursor, heartbeat and sequence. */
  zz9k_audio_ring_producer_publish(&producer, 6U,
                                   cursor_past_32_bits +
                                       ZZ9K_AUDIO_RING_PERIOD_BYTES,
                                   78U,
                                   ZZ9K_AUDIO_RING_PRODUCER_FLAG_PAUSED);
  if (zz9k_get_be32(producer.sequence) != 4U) {
    return 7;
  }
  if (!zz9k_audio_ring_producer_snapshot(&producer, &produced)) {
    return 8;
  }
  if (produced.write_cursor !=
          cursor_past_32_bits + ZZ9K_AUDIO_RING_PERIOD_BYTES ||
      produced.heartbeat != 78U ||
      produced.flags != ZZ9K_AUDIO_RING_PRODUCER_FLAG_PAUSED) {
    return 9;
  }

  /* An odd sequence word means the snapshot tore: retry. */
  zz9k_put_be32(producer.sequence, 5U);
  if (zz9k_audio_ring_producer_snapshot(&producer, &produced)) {
    return 10;
  }

  /* Firmware line: consumed cursor as ring credit plus status. */
  memset(&firmware, 0, sizeof(firmware));
  zz9k_audio_ring_firmware_publish(&firmware, 6U, cursor_past_32_bits,
                                   ZZ9K_AUDIO_RING_STATUS_OK);
  if (zz9k_get_be32(firmware.sequence) != 2U) {
    return 11;
  }
  if (!zz9k_audio_ring_firmware_snapshot(&firmware, &credited)) {
    return 12;
  }
  if (credited.generation != 6U ||
      credited.consumed_cursor != cursor_past_32_bits ||
      credited.status != ZZ9K_AUDIO_RING_STATUS_OK) {
    return 13;
  }
  zz9k_audio_ring_firmware_publish(&firmware, 7U, cursor_past_32_bits + 3840U,
                                   ZZ9K_AUDIO_RING_STATUS_REVOKED_HEARTBEAT);
  if (!zz9k_audio_ring_firmware_snapshot(&firmware, &credited)) {
    return 14;
  }
  if (credited.generation != 7U || /* stale generations are caller-rejected */
      credited.status != ZZ9K_AUDIO_RING_STATUS_REVOKED_HEARTBEAT) {
    return 15;
  }
  zz9k_put_be32(firmware.sequence, 7U);
  if (zz9k_audio_ring_firmware_snapshot(&firmware, &credited)) {
    return 16;
  }
  return 0;
}

/* ---- KTD4 distance rule ---- */

static int test_distance(void)
{
  const uint64_t base = (UINT64_C(3) << 32) + UINT64_C(1234);

  if (!zz9k_audio_ring_distance_valid(base + 3840U, base, Z3_RING_CAPACITY)) {
    return 1;
  }
  if (!zz9k_audio_ring_distance_valid(base + Z3_RING_CAPACITY, base,
                                      Z3_RING_CAPACITY)) {
    return 2; /* exactly full is still valid */
  }
  if (zz9k_audio_ring_distance_valid(base - 1U, base, Z3_RING_CAPACITY)) {
    return 3; /* backward movement faults the generation */
  }
  if (zz9k_audio_ring_distance_valid(base + Z3_RING_CAPACITY + 4U, base,
                                     Z3_RING_CAPACITY)) {
    return 4; /* distance above capacity faults the generation */
  }
  if (!zz9k_audio_ring_distance_valid(base, base, Z3_RING_CAPACITY)) {
    return 5; /* empty ring is valid */
  }
  return 0;
}

int main(void)
{
  int r;

  printf("audio_ring_contract_test: builder/request/reply/control-line checks\n");
  r = test_builders();
  CHECK(r == 0, "descriptor builders reject malformed input");
  r = test_request_helpers();
  CHECK(r == 0, "request builders pack big-endian payloads");
  r = test_acquire_reply();
  CHECK(r == 0, "acquire replies decode only valid grants");
  r = test_state_reply();
  CHECK(r == 0, "state replies decode framed slot snapshots");
  r = test_control_lines();
  CHECK(r == 0, "control-line seqlocks publish and snapshot stably");
  r = test_distance();
  CHECK(r == 0, "write-minus-consumed distance validation");

  if (g_failures != 0) {
    printf("audio_ring_contract_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("audio_ring_contract_test: all checks passed\n");
  return 0;
}
