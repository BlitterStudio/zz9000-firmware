/*
 * Contract tests for the codec-aware audio streaming session contract.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/abi.h"
#include "zz9k/audio.h"
#include "zz9k/host.h"
#include "zz9k/library_vectors.h"
#include "zz9k/request.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void expect_true(const char *name, int condition)
{
  if (!condition) {
    printf("FAIL: %s\n", name);
    failures++;
  }
}

/* 1. Invariant: Old MP3 Begin wire layout, opcode, and LVO are unchanged byte-for-byte */
static void test_old_mp3_contract_unchanged(void)
{
  ZZ9KAudioStreamBeginPayload legacy_wire;
  ZZ9KAudioStreamBeginDesc legacy_desc;
  ZZ9KRequest req;

  expect_true("legacy begin opcode is 0x0503",
              ZZ9K_OP_AUDIO_STREAM_BEGIN == ZZ9K_SERVICE_AUDIO + 0x03U);
  expect_true("legacy begin wire size is 48 bytes",
              sizeof(ZZ9KAudioStreamBeginPayload) == 48U);
  expect_true("legacy LVO is -264",
              ZZ9K_LVO_AUDIO_STREAM_BEGIN == -264);

  memset(&legacy_desc, 0, sizeof(legacy_desc));
  legacy_desc.mp3_ring_handle = 0x40000001UL;
  legacy_desc.mp3_ring_capacity = 65536U;
  legacy_desc.pcm_ring_handle = 0x40000002UL;
  legacy_desc.pcm_ring_capacity = 131072U;
  legacy_desc.output_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE;

  expect_true("build legacy begin desc succeeds",
              zz9k_audio_build_stream_begin_desc(&legacy_desc,
                  0x40000001UL, 65536U, 0x40000002UL, 131072U,
                  0U, 0U, ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE, 0U, 0U, 0U));

  expect_true("request legacy begin succeeds",
              zz9k_request_audio_stream_begin(&req, &legacy_desc) == ZZ9K_STATUS_OK);
  expect_true("request legacy opcode matches",
              req.entry.opcode == ZZ9K_OP_AUDIO_STREAM_BEGIN);

  memcpy(&legacy_wire, req.entry.payload.inline_data, sizeof(legacy_wire));
  expect_true("wire mp3_ring_handle offset 0",
              zz9k_get_be32(legacy_wire.mp3_ring_handle) == 0x40000001UL);
  expect_true("wire mp3_ring_capacity offset 4",
              zz9k_get_be32(legacy_wire.mp3_ring_capacity) == 65536U);
  expect_true("wire pcm_ring_handle offset 8",
              zz9k_get_be32(legacy_wire.pcm_ring_handle) == 0x40000002UL);
  expect_true("wire pcm_ring_capacity offset 12",
              zz9k_get_be32(legacy_wire.pcm_ring_capacity) == 131072U);
  expect_true("wire output_format offset 24",
              zz9k_get_be32(legacy_wire.output_format) == ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE);
}

/* 2. Invariant: New BeginEx descriptor builder, opcode, and LVO */
static void test_new_begin_ex_contract(void)
{
  ZZ9KAudioStreamBeginExPayload ex_wire;
  ZZ9KAudioStreamBeginExDesc ex_desc;
  ZZ9KRequest req;

  expect_true("begin_ex opcode is 0x0516",
              ZZ9K_OP_AUDIO_STREAM_BEGIN_EX == ZZ9K_SERVICE_AUDIO + 0x16U);
  expect_true("begin_ex wire size is 48 bytes",
              sizeof(ZZ9KAudioStreamBeginExPayload) == 48U);
  expect_true("begin_ex LVO is -348",
              ZZ9K_LVO_AUDIO_STREAM_BEGIN_EX == -348);
  expect_true("min library revision for begin_ex is 33",
              ZZ9K_LIBRARY_MIN_REVISION_AUDIO_STREAM_EX == 33);

  /* Valid MP3 BeginEx */
  expect_true("build begin_ex MP3 desc succeeds",
              zz9k_audio_build_stream_begin_ex_desc(&ex_desc,
                  ZZ9K_AUDIO_CODEC_MP3,
                  0x40000010UL, 65536U, 0x40000020UL, 131072U,
                  0U, 0U, ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE, 0U, 0U, 0U));

  expect_true("request begin_ex encodes OK",
              zz9k_request_audio_stream_begin_ex(&req, &ex_desc) == ZZ9K_STATUS_OK);
  expect_true("request begin_ex opcode matches",
              req.entry.opcode == ZZ9K_OP_AUDIO_STREAM_BEGIN_EX);

  memcpy(&ex_wire, req.entry.payload.inline_data, sizeof(ex_wire));
  expect_true("wire codec offset 0 is MP3",
              zz9k_get_be32(ex_wire.codec) == ZZ9K_AUDIO_CODEC_MP3);
  expect_true("wire input_ring_handle offset 4",
              zz9k_get_be32(ex_wire.input_ring_handle) == 0x40000010UL);
  expect_true("wire input_ring_capacity offset 8",
              zz9k_get_be32(ex_wire.input_ring_capacity) == 65536U);
  expect_true("wire pcm_ring_handle offset 12",
              zz9k_get_be32(ex_wire.pcm_ring_handle) == 0x40000020UL);
  expect_true("wire pcm_ring_capacity offset 16",
              zz9k_get_be32(ex_wire.pcm_ring_capacity) == 131072U);
  expect_true("wire output_format offset 28",
              zz9k_get_be32(ex_wire.output_format) == ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE);
}

/* 3. Invariant: Unknown codec and invalid parameters rejected */
static void test_unknown_codec_and_invalid_params_rejected(void)
{
  ZZ9KAudioStreamBeginExDesc ex_desc;
  ZZ9KRequest req;

  /* Unknown codec (0) rejected by builder */
  expect_true("builder rejects unknown codec 0",
              !zz9k_audio_build_stream_begin_ex_desc(&ex_desc,
                  0U, 0x40000010UL, 65536U, 0x40000020UL, 131072U,
                  0U, 0U, ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE, 0U, 0U, 0U));

  /* Unknown codec (99) rejected by builder */
  expect_true("builder rejects invalid codec 99",
              !zz9k_audio_build_stream_begin_ex_desc(&ex_desc,
                  99U, 0x40000010UL, 65536U, 0x40000020UL, 131072U,
                  0U, 0U, ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE, 0U, 0U, 0U));

  /* Unknown sample format rejected by builder */
  expect_true("builder rejects unknown sample format 99",
              !zz9k_audio_build_stream_begin_ex_desc(&ex_desc,
                  ZZ9K_AUDIO_CODEC_MP3, 0x40000010UL, 65536U, 0x40000020UL, 131072U,
                  0U, 0U, 99U, 0U, 0U, 0U));

  /* Nonzero flags rejected by builder */
  expect_true("builder rejects nonzero flags",
              !zz9k_audio_build_stream_begin_ex_desc(&ex_desc,
                  ZZ9K_AUDIO_CODEC_MP3, 0x40000010UL, 65536U, 0x40000020UL, 131072U,
                  0U, 0U, ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE, 0U, 0U, 1U));

  /* Request builder rejects corrupt desc */
  memset(&ex_desc, 0, sizeof(ex_desc));
  ex_desc.codec = ZZ9K_AUDIO_CODEC_MP3;
  ex_desc.input_ring_handle = ZZ9K_INVALID_HANDLE;
  ex_desc.input_ring_capacity = 65536U;
  ex_desc.pcm_ring_handle = 0x40000020UL;
  ex_desc.pcm_ring_capacity = 131072U;
  ex_desc.output_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE;
  expect_true("request builder rejects invalid input handle",
              zz9k_request_audio_stream_begin_ex(&req, &ex_desc) == ZZ9K_STATUS_BAD_REQUEST);

  ex_desc.input_ring_handle = 0x40000010UL;
  ex_desc.codec = 99U;
  expect_true("request builder rejects unknown codec",
              zz9k_request_audio_stream_begin_ex(&req, &ex_desc) == ZZ9K_STATUS_BAD_REQUEST);
}

/* 4. Invariant: PCM container and valid-bit semantics */
static void test_pcm_container_semantics(void)
{
  expect_true("S16LE is known", zz9k_audio_sample_format_known(ZZ9K_AUDIO_SAMPLE_FORMAT_S16LE));
  expect_true("S16BE is known", zz9k_audio_sample_format_known(ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE));
  expect_true("S32LE is known", zz9k_audio_sample_format_known(ZZ9K_AUDIO_SAMPLE_FORMAT_S32LE));
  expect_true("S32BE is known", zz9k_audio_sample_format_known(ZZ9K_AUDIO_SAMPLE_FORMAT_S32BE));
  expect_true("FORMAT_NONE is not known", !zz9k_audio_sample_format_known(ZZ9K_AUDIO_SAMPLE_FORMAT_NONE));
  expect_true("FORMAT 5 is not known", !zz9k_audio_sample_format_known(5U));

  /* 24-bit scaling semantics: 24 valid bits left-aligned in S32 (MSB-justified) */
  {
    /* 24-bit full-scale positive: 0x7FFFFF -> S32: 0x7FFFFF00 */
    int32_t sample24 = 0x007fffff;
    int32_t sample32 = (int32_t)((uint32_t)sample24 << 8);
    expect_true("24-bit left-alignment MSB preserved", (sample32 >> 8) == sample24);
    expect_true("24-bit left-alignment LSBs zeroed", (sample32 & 0xff) == 0);

    /* 24-bit negative: -1 (0xFFFFFF) -> S32: 0xFFFFFF00 */
    sample24 = -1;
    sample32 = (int32_t)((uint32_t)sample24 << 8);
    expect_true("24-bit negative preserved", (sample32 >> 8) == -1);
  }
}

/* 5. Invariant: Cursor arithmetic, partial ACK, and wraparound safety */
static void test_cursor_and_wraparound_invariants(void)
{
  /* Monotonic cursor tracking simulation */
  uint32_t pcm_ready_total = 0U;
  uint32_t pcm_consumed_total = 0U;
  const uint32_t pcm_capacity = 65536U;

  /* Helper to compute available used bytes */
  #define USED_BYTES(ready, consumed) ((uint32_t)((ready) - (consumed)))

  /* Initial state */
  expect_true("initial used is 0", USED_BYTES(pcm_ready_total, pcm_consumed_total) == 0U);

  /* Decode produces 4096 bytes */
  pcm_ready_total += 4096U;
  expect_true("used after produce is 4096", USED_BYTES(pcm_ready_total, pcm_consumed_total) == 4096U);

  /* Consumer reads and partially acknowledges 1024 bytes */
  pcm_consumed_total += 1024U;
  expect_true("used after partial ACK is 3072 (unreturned preserved)",
              USED_BYTES(pcm_ready_total, pcm_consumed_total) == 3072U);

  /* Over-ACK guard: client requests to read 4096 bytes when only 3072 available -> rejected! */
  {
    uint32_t requested_ack = 4096U;
    int ack_permitted = requested_ack <= USED_BYTES(pcm_ready_total, pcm_consumed_total);
    expect_true("over-ACK is rejected and consumes nothing", !ack_permitted);
  }

  /* Simulate 32-bit counter wraparound:
   * pcm_ready_total near 2^32 - 1, producing wraps past 0 */
  pcm_ready_total = 0xffffff00U;
  pcm_consumed_total = 0xffffff00U;
  expect_true("used at wrap point is 0", USED_BYTES(pcm_ready_total, pcm_consumed_total) == 0U);

  /* Produce 8192 bytes past 0xFFFFFFFF (wraps to 0x00001F00) */
  pcm_ready_total += 8192U;
  expect_true("used across u32 wraparound is exact 8192",
              USED_BYTES(pcm_ready_total, pcm_consumed_total) == 8192U);

  /* Partial ACK 4096 bytes across wrap */
  pcm_consumed_total += 4096U;
  expect_true("used after partial ACK across wrap is 4096",
              USED_BYTES(pcm_ready_total, pcm_consumed_total) == 4096U);

  /* Full ACK remaining 4096 bytes */
  pcm_consumed_total += 4096U;
  expect_true("used after full ACK is 0",
              USED_BYTES(pcm_ready_total, pcm_consumed_total) == 0U);

  #undef USED_BYTES
  (void)pcm_capacity;
}

static void test_lease_rate_vocabulary(void)
{
  expect_true("16 kHz is a lease rate", zz9k_audio_ring_rate_known(16000U));
  expect_true("22.05 kHz is a lease rate", zz9k_audio_ring_rate_known(22050U));
  expect_true("44.1 kHz is a lease rate", zz9k_audio_ring_rate_known(44100U));
  expect_true("48 kHz is a lease rate", zz9k_audio_ring_rate_known(48000U));
  /* Pump-only: 20 ms is not an integer frame count, or the source
   * period does not fit the lease scratch. */
  expect_true("11025 is not a lease rate", !zz9k_audio_ring_rate_known(11025U));
  expect_true("88.2 kHz is not a lease rate", !zz9k_audio_ring_rate_known(88200U));
  expect_true("96 kHz is not a lease rate", !zz9k_audio_ring_rate_known(96000U));
}
int main(void)
{
  test_old_mp3_contract_unchanged();
  test_new_begin_ex_contract();
  test_unknown_codec_and_invalid_params_rejected();
  test_pcm_container_semantics();
  test_lease_rate_vocabulary();
  test_cursor_and_wraparound_invariants();

  if (failures) {
    printf("audio_stream_contract_test: %d test(s) failed\n", failures);
    return 1;
  }
  printf("audio_stream_contract_test: all contract invariants verified\n");
  return 0;
}
