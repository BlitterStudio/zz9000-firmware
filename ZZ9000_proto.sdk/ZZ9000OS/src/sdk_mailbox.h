/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * ZZ9000 SDK v2 mailbox dispatcher.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_MAILBOX_H
#define SDK_MAILBOX_H

#include <stdint.h>
#include "audio_scene.h"
#include "scheduler.h"

#define SDK_MAILBOX_MAGIC              0x5a5a394bUL
#define SDK_MAILBOX_REG_MAGIC_VALUE    0x5a39U
#define SDK_MAILBOX_ABI_MAJOR          2U
#define SDK_MAILBOX_ABI_MINOR          0U
#define SDK_MAILBOX_ENTRY_SIZE         64U
#define SDK_MAILBOX_DESCRIPTOR_SIZE    128U
#define SDK_MAILBOX_RING_ENTRIES       64U

#define SDK_STATUS_OK                  0U
#define SDK_STATUS_QUEUED              1U
#define SDK_STATUS_BUSY                2U
#define SDK_STATUS_UNSUPPORTED         3U
#define SDK_STATUS_BAD_REQUEST         4U
#define SDK_STATUS_BAD_HANDLE          5U
#define SDK_STATUS_NO_MEMORY           6U
#define SDK_STATUS_IO_ERROR            9U
#define SDK_STATUS_NOT_FOUND           10U
#define SDK_STATUS_INTERNAL_ERROR      0xffffU

#define SDK_CAP_MAILBOX                (1U << 0)
#define SDK_CAP_IRQ_COMPLETION         (1U << 1)
#define SDK_CAP_SHARED_ALLOC           (1U << 2)
#define SDK_CAP_SURFACES               (1U << 3)
#define SDK_CAP_FRAMEBUFFER_SURFACE    (1U << 4)
#define SDK_CAP_IMAGE_DECODE           (1U << 5)
#define SDK_CAP_IMAGE_SCALE            (1U << 6)
#define SDK_CAP_AUDIO_DECODE           (1U << 7)
#define SDK_CAP_CRYPTO                 (1U << 8)
#define SDK_CAP_AUDIO_PLAYBACK         (1U << 19)
#define SDK_CAP_MEMORY_OPS             (1U << 10)
#define SDK_CAP_DIAGNOSTICS            (1U << 11)
#define SDK_CAP_DOORBELL               (1U << 12)
#define SDK_CAP_POLLING_COMPLETION     (1U << 13)
#define SDK_CAP_SERVICE_DISCOVERY      (1U << 14)
#define SDK_CAP_SURFACE_OPS            (1U << 15)
#define SDK_CAP_COMPRESSION            (1U << 16)
// Firmware serves SDK_ALLOC_HOST_WINDOW allocations from the small
// board-window-reachable heap (Zorro 2 support; see memorymap.h).
#define SDK_CAP_HOST_WINDOW_HEAP       (1U << 20)
#define SDK_CAP_VIDEO_DECODE           (1U << 21)
#define SDK_CAP_MEDIA_SESSION          (1U << 22)
#define SDK_CAP_AUDIO_STREAM_DRAIN     (1U << 23)
/* QUERY_APERTURE_LAYOUT is available. HOST_WINDOW_HEAP is advertised on
 * generation-1 Z2 only after the driver acknowledges the exact contract. */
#define SDK_CAP_APERTURE_LAYOUT        (1U << 24)
/* Audio control plane / metering. Append-only but deliberately NOT
 * advertised in any capability word until the on-hardware verification
 * session qualifies them (R12). */
#define SDK_CAP_AUDIO_CONTROL          (1U << 25)
#define SDK_CAP_AUDIO_METERING         (1U << 26)

// SDK_OP_ALLOC_SHARED flags. HOST_WINDOW places the buffer in the
// host-window heap so a Zorro 2 host can map it; CARD_ONLY is a
// host-side declaration ("the 68k never touches this buffer") that the
// firmware merely stores and echoes.
// Generation-1 Zorro 2 bitstreams require an exact layout acknowledgement
// before firmware advertises or honors HOST_WINDOW. Zorro 3 and bitstreams
// without the generation-tagged aperture register retain the legacy fixed
// heap; zz9k.library strips HOST_WINDOW on Z3 and rejects that legacy address
// on sub-4 MB Z2 boards.
#define SDK_ALLOC_HOST_WINDOW     (1U << 0)
#define SDK_ALLOC_CARD_ONLY       (1U << 1)

#ifndef SDK_ENABLE_HDL_TRANSPORT
#define SDK_ENABLE_HDL_TRANSPORT       0
#endif

#ifndef SDK_ENABLE_HDL_DOORBELL
#define SDK_ENABLE_HDL_DOORBELL        0
#endif

#ifndef SDK_ENABLE_COMPLETION_IRQ
#define SDK_ENABLE_COMPLETION_IRQ      1
#endif

#if SDK_ENABLE_COMPLETION_IRQ
#define SDK_IRQ_CAPABILITY_BITS        SDK_CAP_IRQ_COMPLETION
#else
#define SDK_IRQ_CAPABILITY_BITS        0
#endif

#if SDK_ENABLE_HDL_DOORBELL
#define SDK_DOORBELL_CAPABILITY_BITS   SDK_CAP_DOORBELL
#else
#define SDK_DOORBELL_CAPABILITY_BITS   0
#endif

#define SDK_TRANSPORT_CAPABILITY_BITS \
	(SDK_CAP_POLLING_COMPLETION | SDK_IRQ_CAPABILITY_BITS | \
	 SDK_DOORBELL_CAPABILITY_BITS)

#define SDK_SERVICE_CORE               0x0000U
#define SDK_SERVICE_MEMORY             0x0100U
#define SDK_SERVICE_SURFACE            0x0200U
#define SDK_SERVICE_IMAGE              0x0400U
#define SDK_SERVICE_AUDIO              0x0500U
#define SDK_SERVICE_CODEC              0x0600U
#define SDK_SERVICE_CRYPTO             0x0800U
#define SDK_SERVICE_DIAG               0x0900U
#define SDK_SERVICE_VIDEO              0x0b00U

#define SDK_SERVICE_FLAG_FIRMWARE      (1U << 0)
#define SDK_SERVICE_FLAG_MODULE        (1U << 1)
#define SDK_SERVICE_FLAG_ASYNC         (1U << 2)
#define SDK_SERVICE_FLAG_ZERO_COPY     (1U << 3)
#define SDK_SERVICE_FLAG_SURFACE_PALETTE_QUERY  (1U << 16)
#define SDK_SERVICE_FLAG_IMAGE_JPEG_BASELINE    (1U << 16)
#define SDK_SERVICE_FLAG_IMAGE_JPEG_PROGRESSIVE (1U << 17)
#define SDK_SERVICE_FLAG_IMAGE_JPEG_DIRECT_BGRA (1U << 18)
#define SDK_SERVICE_FLAG_IMAGE_JPEG_SCALING     (1U << 19)
#define SDK_SERVICE_FLAG_IMAGE_STREAMING_INPUT  (1U << 20)
#define SDK_SERVICE_FLAG_IMAGE_TILE_OUTPUT      (1U << 21)
#define SDK_SERVICE_FLAG_IMAGE_FRAMEBUFFER_OUTPUT (1U << 22)
#define SDK_SERVICE_FLAG_IMAGE_SCALE_BILINEAR   (1U << 23)
#define SDK_SERVICE_FLAG_IMAGE_SCALE_CLIPPED    (1U << 24)
#define SDK_SERVICE_FLAG_IMAGE_PNG_DIRECT_BGRA  (1U << 25)
#define SDK_SERVICE_FLAG_IMAGE_RGB888_OUTPUT    (1U << 26)
#define SDK_SERVICE_FLAG_IMAGE_SCALE_BGRA_TO_RGB555_RGB565 (1U << 27)
#define SDK_SERVICE_FLAG_AUDIO_MP3_DECODE       (1U << 16)
#define SDK_SERVICE_FLAG_AUDIO_MP3_STREAM       (1U << 20)
/* Control-plane audio opcodes (0x0509+) are dispatchable. Follows the
 * SDK_CAP_AUDIO_CONTROL gated-advertising discipline: not reported in
 * the audio service flags until qualified. */
#define SDK_SERVICE_FLAG_AUDIO_CONTROL (1U << 21)
#define SDK_SERVICE_FLAG_CODEC_DEFLATE_RAW      (1U << 16)
#define SDK_SERVICE_FLAG_CODEC_ZLIB             (1U << 17)
#define SDK_SERVICE_FLAG_CODEC_GZIP             (1U << 18)
#define SDK_SERVICE_FLAG_CODEC_LZ4_BLOCK        (1U << 19)
#define SDK_SERVICE_FLAG_CODEC_LZMA_ALONE       (1U << 20)
#define SDK_SERVICE_FLAG_CODEC_CHECKSUM         (1U << 21)
#define SDK_SERVICE_FLAG_CODEC_DECOMPRESS_TEST  (1U << 22)
#define SDK_SERVICE_FLAG_CODEC_DECOMPRESS_STREAM (1U << 23)
#define SDK_SERVICE_FLAG_CODEC_DECOMPRESS_FEED  (1U << 24)
#define SDK_SERVICE_FLAG_CODEC_DEFLATE_FEED     (1U << 25)
#define SDK_SERVICE_FLAG_CODEC_ZLIB_FEED        (1U << 26)
#define SDK_SERVICE_FLAG_CODEC_GZIP_FEED        (1U << 27)
#define SDK_SERVICE_FLAG_CODEC_LZMA2            (1U << 28)
#define SDK_SERVICE_FLAG_CODEC_LZH              (1U << 29)
#define SDK_SERVICE_FLAG_CODEC_DECOMPRESS_BATCH (1U << 30)
#define SDK_SERVICE_FLAG_CRYPTO_X25519          (1U << 16)
#define SDK_SERVICE_FLAG_VIDEO_MPEG1            (1U << 16)
#define SDK_SERVICE_FLAG_VIDEO_MPEG_PS          (1U << 17)
#define SDK_SERVICE_FLAG_VIDEO_DIRECT_OVERLAY   (1U << 18)
#define SDK_SERVICE_FLAG_VIDEO_STREAMING_INPUT  (1U << 19)
#define SDK_SERVICE_FLAG_VIDEO_CORE1            (1U << 20)
#define SDK_SERVICE_FLAG_VIDEO_MEDIA_SESSION    (1U << 21)
#define SDK_SERVICE_FLAG_VIDEO_MEDIA_MP2        (1U << 22)
#define SDK_SERVICE_FLAG_VIDEO_EXPLICIT_PRESENT (1U << 23)
#define SDK_SERVICE_FLAG_VIDEO_TIMELINE_90KHZ   (1U << 24)
#define SDK_SERVICE_FLAG_VIDEO_PCM_RING_STATUS  (1U << 25)
#define SDK_SERVICE_FLAG_VIDEO_AUDIO_BIND       (1U << 26)

#define SDK_OP_NOP                     0x0000U
#define SDK_OP_QUERY_CAPS              0x0001U
#define SDK_OP_PING                    0x0002U
#define SDK_OP_QUERY_SERVICE           0x0004U
#define SDK_OP_QUERY_APERTURE_LAYOUT   0x0005U

#define SDK_OP_ALLOC_SHARED            0x0100U
#define SDK_OP_FREE_SHARED             0x0101U
#define SDK_OP_MEM_FILL                0x0102U
#define SDK_OP_MEM_COPY                0x0103U

#define SDK_OP_ALLOC_SURFACE           0x0200U
#define SDK_OP_FREE_SURFACE            0x0201U
#define SDK_OP_MAP_FRAMEBUFFER_SURFACE 0x0202U
#define SDK_OP_FILL_SURFACE            0x0203U
#define SDK_OP_COPY_SURFACE            0x0204U
#define SDK_OP_QUERY_PALETTE           0x0205U

#define SDK_OP_SCALE_IMAGE             0x0400U
#define SDK_OP_DECODE_JPEG             0x0401U
#define SDK_OP_DECODE_PNG              0x0402U
#define SDK_OP_DECODE_GIF              0x0403U
#define SDK_OP_IMAGE_SESSION_BEGIN     0x0404U
#define SDK_OP_IMAGE_SESSION_FEED      0x0405U
#define SDK_OP_IMAGE_SESSION_CLOSE     0x0406U
#define SDK_OP_SCALE_IMAGE_CLIPPED     0x0407U

#define SDK_OP_DECODE_MP3              0x0500U
#define SDK_OP_AUDIO_STREAM_BEGIN      0x0503U
#define SDK_OP_AUDIO_STREAM_FEED       0x0504U
#define SDK_OP_AUDIO_STREAM_READ       0x0505U
#define SDK_OP_AUDIO_STREAM_CLOSE      0x0506U
#define SDK_OP_AUDIO_STREAM_PLAY       0x0507U
#define SDK_OP_AUDIO_STREAM_STOP       0x0508U
/* Firmware-authoritative audio control plane (scenes, trims, metering).
 * SHORT-task dispatch with 48-byte inline payloads; the payloads mirror
 * the SDK ZZ9KAudio*Payload structs below byte-for-byte. Not advertised
 * in any capability word or service flag until the on-hardware
 * verification session passes. */
#define SDK_OP_AUDIO_SCENE_SELECT      0x0509U
#define SDK_OP_AUDIO_SCENE_WRITE       0x050aU
#define SDK_OP_AUDIO_TRIM_SUBMIT       0x050bU
#define SDK_OP_AUDIO_METER_READ        0x050cU
#define SDK_OP_AUDIO_SCENE_SAVE        0x050dU
#define SDK_OP_AUDIO_CONTROL_STATE_GET 0x050eU

#define SDK_OP_DECOMPRESS              0x0600U
#define SDK_OP_DECOMPRESS_TEST         0x0601U
#define SDK_OP_DECOMPRESS_STREAM_BEGIN 0x0602U
#define SDK_OP_DECOMPRESS_STREAM_READ  0x0603U
#define SDK_OP_DECOMPRESS_STREAM_CLOSE 0x0604U
#define SDK_OP_DECOMPRESS_STREAM_FEED  0x0605U
#define SDK_OP_DECOMPRESS_BATCH        0x0606U

#define SDK_OP_CRYPTO_HASH             0x0800U
#define SDK_OP_CRYPTO_STREAM           0x0801U
#define SDK_OP_CRYPTO_AEAD             0x0802U
#define SDK_OP_CRYPTO_KX               0x0803U
#define SDK_CRYPTO_KX_X25519           1U
#define SDK_CRYPTO_KX_P256             2U

/* KX flags word. KEYGEN turns a P-256 request into scalar*G: `scalar` is the
 * private key, the peer point is ignored, and `dst` receives the full 65-byte
 * uncompressed public point. Any other flag value is rejected (UNSUPPORTED). */
#define SDK_CRYPTO_KX_FLAG_KEYGEN      1U

#define SDK_OP_CRYPTO_VERIFY           0x0804U
#define SDK_CRYPTO_VERIFY_ECDSA_P256_SHA256        1U
#define SDK_CRYPTO_VERIFY_RSA_PKCS1_2048_SHA256    2U

#define SDK_SERVICE_FLAG_CRYPTO_X25519     (1U << 16)
#define SDK_SERVICE_FLAG_CRYPTO_P256       (1U << 17)
#define SDK_SERVICE_FLAG_CRYPTO_ECDSA_P256 (1U << 18)
#define SDK_SERVICE_FLAG_CRYPTO_RSA_2048   (1U << 19)
#define SDK_SERVICE_FLAG_CRYPTO_AES_GCM    (1U << 20)
/* P-256 keygen (scalar*G -> full point) via the KX KEYGEN flag. Distinct from
 * CRYPTO_P256 (derive only), which shipped without keygen. */
#define SDK_SERVICE_FLAG_CRYPTO_P256_KEYGEN (1U << 21)

/* Crypto verify payload: 48 bytes to match the inline mailbox entry size and
 * the SDK ZZ9KCryptoVerifyPayload byte-for-byte. All fields are big-endian
 * uint32_t encoded as byte arrays. The caller supplies a precomputed SHA-256
 * digest (hash_*), the signature (sig_*), and the public key (key_*). For
 * ECDSA-P256 the key buffer is the 65-byte uncompressed point and the
 * signature is raw r||s (64 bytes); for RSA-PKCS1-2048 the key buffer is the
 * 256-byte modulus followed by the 4-byte big-endian public exponent and the
 * signature is 256 bytes. */
struct SDKCryptoVerifyPayload {
	uint8_t algorithm[4];
	uint8_t hash_handle[4];
	uint8_t hash_offset[4];
	uint8_t hash_length[4];
	uint8_t sig_handle[4];
	uint8_t sig_offset[4];
	uint8_t sig_length[4];
	uint8_t key_handle[4];
	uint8_t key_offset[4];
	uint8_t key_length[4];
	uint8_t reserved[8];
};

#define SDK_OP_DIAG_READ               0x0900U
#define SDK_OP_DIAG_TIMING             0x0901U
#define SDK_OP_DIAG_SCHED              0x0902U
#define SDK_OP_DIAG_MEMORY             0x0903U

#define SDK_OP_VIDEO_SESSION_BEGIN     0x0b00U
#define SDK_OP_VIDEO_SESSION_WRITE     0x0b01U
#define SDK_OP_VIDEO_SESSION_DECODE    0x0b02U
#define SDK_OP_VIDEO_SESSION_CLOSE     0x0b03U
#define SDK_OP_MEDIA_SESSION_BEGIN     0x0b04U
#define SDK_OP_MEDIA_SESSION_WRITE     0x0b05U
#define SDK_OP_MEDIA_SESSION_DECODE    0x0b06U
#define SDK_OP_MEDIA_SESSION_AUDIO_READ 0x0b07U
#define SDK_OP_MEDIA_SESSION_PRESENT   0x0b08U
#define SDK_OP_MEDIA_SESSION_DISCARD   0x0b09U
#define SDK_OP_MEDIA_SESSION_STATUS    0x0b0aU
#define SDK_OP_MEDIA_SESSION_AUDIO_BIND 0x0b0bU
#define SDK_OP_MEDIA_SESSION_AUDIO_UNBIND 0x0b0cU
#define SDK_OP_MEDIA_SESSION_CLOSE     0x0b0dU

struct SDKMediaSessionBeginPayload {
	uint8_t video_codec[4];
	uint8_t container[4];
	uint8_t width[4];
	uint8_t height[4];
	uint8_t output_format[4];
	uint8_t audio_codec[4];
	uint8_t pcm_ring_handle[4];
	uint8_t pcm_ring_capacity[4];
	uint8_t pcm_low_water_bytes[4];
	uint8_t pcm_high_water_bytes[4];
	uint8_t flags[4];
	uint8_t reserved[4];
};

struct SDKMediaSessionWritePayload {
	uint8_t session[4];
	uint8_t src_handle[4];
	uint8_t src_offset[4];
	uint8_t src_length[4];
	uint8_t flags[4];
	uint8_t reserved[28];
};

struct SDKMediaSessionCommandPayload {
	uint8_t session[4];
	uint8_t value_hi[4];
	uint8_t value_lo[4];
	uint8_t flags[4];
	uint8_t reserved[32];
};

struct SDKMediaSessionStatusPayload {
	uint8_t session[4];
	uint8_t page[4];
	uint8_t flags[4];
	uint8_t reserved[36];
};

struct SDKMediaSessionMainResultPayload {
	uint8_t session[4];
	uint8_t state[4];
	uint8_t width[4];
	uint8_t height[4];
	uint8_t frame_rate_num[4];
	uint8_t frame_rate_den[4];
	uint8_t frame_number[4];
	uint8_t video_pts_hi[4];
	uint8_t video_pts_lo[4];
	uint8_t bytes_accepted[4];
	uint8_t bytes_written[4];
	uint8_t flags[4];
};

struct SDKMediaSessionAudioResultPayload {
	uint8_t session[4];
	uint8_t state[4];
	uint8_t sample_rate[4];
	uint8_t channels[4];
	uint8_t sample_format[4];
	uint8_t pcm_produced_hi[4];
	uint8_t pcm_produced_lo[4];
	uint8_t pcm_acknowledged_hi[4];
	uint8_t pcm_acknowledged_lo[4];
	uint8_t audio_pts_hi[4];
	uint8_t audio_pts_lo[4];
	uint8_t flags[4];
};

struct SDKMediaSessionStatusResultPayload {
	uint8_t session[4];
	uint8_t state[4];
	uint8_t page[4];
	uint8_t flags[4];
	uint8_t value0_hi[4];
	uint8_t value0_lo[4];
	uint8_t value1_hi[4];
	uint8_t value1_lo[4];
	uint8_t value2_hi[4];
	uint8_t value2_lo[4];
	uint8_t value3_hi[4];
	uint8_t value3_lo[4];
};

typedef char SDKMediaSessionBeginPayload_must_be_48_bytes[
	(sizeof(struct SDKMediaSessionBeginPayload) == 48U) ? 1 : -1];
typedef char SDKMediaSessionWritePayload_must_be_48_bytes[
	(sizeof(struct SDKMediaSessionWritePayload) == 48U) ? 1 : -1];
typedef char SDKMediaSessionCommandPayload_must_be_48_bytes[
	(sizeof(struct SDKMediaSessionCommandPayload) == 48U) ? 1 : -1];
typedef char SDKMediaSessionStatusPayload_must_be_48_bytes[
	(sizeof(struct SDKMediaSessionStatusPayload) == 48U) ? 1 : -1];
typedef char SDKMediaSessionMainResultPayload_must_be_48_bytes[
	(sizeof(struct SDKMediaSessionMainResultPayload) == 48U) ? 1 : -1];
typedef char SDKMediaSessionAudioResultPayload_must_be_48_bytes[
	(sizeof(struct SDKMediaSessionAudioResultPayload) == 48U) ? 1 : -1];
typedef char SDKMediaSessionStatusResultPayload_must_be_48_bytes[
	(sizeof(struct SDKMediaSessionStatusResultPayload) == 48U) ? 1 : -1];

/* Firmware-authoritative audio control plane vocabulary and payloads
 * (opcodes 0x0509+). All payload fields big-endian, 48 bytes to match
 * the inline mailbox entry size and the SDK ZZ9KAudio*Payload structs
 * byte-for-byte. Nothing here is advertised in any capability word or
 * service flag until the on-hardware verification session passes. */

#define SDK_AUDIO_SCENE_COUNT        8U

/* SDK_OP_AUDIO_SCENE_WRITE stages one parameter per call; the param id
 * fixes the value word's semantics. Pair values pack like the historical
 * AP_DSP_SET_VOLUMES register convention: low byte = channel 1,
 * high byte = channel 2. */
#define SDK_AUDIO_SCENE_PARAM_LPF        1U  /* cutoff, Hz */
#define SDK_AUDIO_SCENE_PARAM_EQ_BAND_1  2U  /* gain 0..100, 50 = 0 dB */
#define SDK_AUDIO_SCENE_PARAM_EQ_BAND_2  3U
#define SDK_AUDIO_SCENE_PARAM_EQ_BAND_3  4U
#define SDK_AUDIO_SCENE_PARAM_EQ_BAND_4  5U
#define SDK_AUDIO_SCENE_PARAM_EQ_BAND_5  6U
#define SDK_AUDIO_SCENE_PARAM_EQ_BAND_6  7U
#define SDK_AUDIO_SCENE_PARAM_EQ_BAND_7  8U
#define SDK_AUDIO_SCENE_PARAM_EQ_BAND_8  9U
#define SDK_AUDIO_SCENE_PARAM_EQ_BAND_9  10U
#define SDK_AUDIO_SCENE_PARAM_EQ_BAND_10 11U
#define SDK_AUDIO_SCENE_PARAM_PREFACTOR  12U
#define SDK_AUDIO_SCENE_PARAM_VOLUME     13U /* 0..100, 100 = 0 dB
                                               * (audio_adau_set_vol_pan
                                               * range; the pair params
                                               * BASELINE/trim use the
                                               * 0..255 mixer scale) */
#define SDK_AUDIO_SCENE_PARAM_PAN        14U
#define SDK_AUDIO_SCENE_PARAM_BASELINE   15U /* operator Paula/AX pair;
                                               * scene index is ignored */
#define SDK_AUDIO_SCENE_PARAM_NAME       16U /* scene name chunk: bits
                                               * 15..8 = first char,
                                               * 7..0 = second char,
                                               * both printable ASCII
                                               * (0x20-0x7E) or 0x00;
                                               * a 0x0000 chunk is the
                                               * terminator. A rename
                                               * stages the complete
                                               * name as up to 8
                                               * chunks (16 chars
                                               * incl. the NUL), then
                                               * commits ONCE: the
                                               * first NAME chunk of
                                               * a draft clears the
                                               * name and lands at
                                               * chunk 0, extra
                                               * terminator chunks
                                               * after the first are
                                               * ignored (zero-padding
                                               * is safe), and a
                                               * non-terminator chunk
                                               * after the terminator
                                               * restarts at chunk 0.
                                               * A NAME-only commit
                                               * issues zero DSP
                                               * writes. */

/* Staged edits accumulate firmware-side; COMMIT asks for one atomic
 * glitch-free commit (fade -> ordered verified writes -> restore) of
 * everything staged for that scene. */
#define SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT (1U << 0)

/* Balance words: two 0..255 volumes (127 = 0 dB each) in one word,
 * AP_DSP_SET_VOLUMES packing (low byte = Paula/ch1, high = AX/ch2). */
#define SDK_AUDIO_BALANCE_NEUTRAL    0x7f7fU
#define SDK_AUDIO_BALANCE_CH1(w)     ((uint32_t)(w) & 0xffU)
#define SDK_AUDIO_BALANCE_CH2(w)     (((uint32_t)(w) >> 8) & 0xffU)
#define SDK_AUDIO_BALANCE_PACK(ch1, ch2) \
	((uint32_t)((uint32_t)(uint8_t)(ch1) | \
	 ((uint32_t)(uint8_t)(ch2) << 8)))

/* SDK_OP_AUDIO_TRIM_SUBMIT result flag: the requested balance was
 * bounded by scene policy; balance_bound carries the bound applied. */
#define SDK_AUDIO_TRIM_RESULT_BOUNDED (1U << 0)

#define SDK_AUDIO_METER_DIRECTION_OUTPUT  1U
#define SDK_AUDIO_METER_DIRECTION_CAPTURE 2U

/* Active source identity per direction; 0 is the legacy/unknown default
 * for owners that predate the control plane. */
#define SDK_AUDIO_METER_IDENTITY_UNKNOWN    0U
#define SDK_AUDIO_METER_IDENTITY_AHI        1U
#define SDK_AUDIO_METER_IDENTITY_MEDIA      2U
#define SDK_AUDIO_METER_IDENTITY_SDK_STREAM 3U

/* This meter read reset the per-direction peak-hold (read-and-clear). */
#define SDK_AUDIO_METER_RESULT_HOLD_RESET (1U << 0)

/* SDK_OP_AUDIO_SCENE_SAVE result status word. */
#define SDK_AUDIO_SCENE_SAVE_OK       0U
#define SDK_AUDIO_SCENE_SAVE_REJECTED 1U /* failed boundary validation */
#define SDK_AUDIO_SCENE_SAVE_IO_ERROR 2U /* temp-then-replace failed */

/* SDK_OP_AUDIO_CONTROL_STATE_GET result flag: the current trim was
 * bounded by scene policy. */
#define SDK_AUDIO_CONTROL_FLAG_TRIM_BOUNDED (1U << 0)

struct SDKAudioSceneSelectPayload {
	uint8_t scene[4];
	uint8_t flags[4];
	uint8_t reserved[40];
};

/* Stage one parameter of one scene; firmware accumulates. COMMIT asks
 * for one atomic glitch-free commit of the staged group. */
struct SDKAudioSceneWritePayload {
	uint8_t scene[4];
	uint8_t param[4];
	uint8_t value[4];
	uint8_t flags[4];
	uint8_t reserved[32];
};

/* Owner source trim on top of the operator baseline. */
struct SDKAudioTrimSubmitPayload {
	uint8_t balance[4];
	uint8_t flags[4];
	uint8_t reserved[40];
};

struct SDKAudioTrimResultPayload {
	uint8_t balance_applied[4];
	uint8_t balance_bound[4];
	uint8_t flags[4];
	uint8_t reserved[36];
};

struct SDKAudioMeterReadPayload {
	uint8_t direction[4];
	uint8_t flags[4];
	uint8_t reserved[40];
};

/* One framed, non-tearing snapshot of one direction. All frames of a
 * snapshot carry the same generation; differing generations across
 * frames mean the read tore and must be retried. Peaks are unsigned
 * 16.16 (0x00010000 = digital full scale); counters saturate, never
 * wrap. frame_count is 1 while the whole state fits here; future
 * extensions raise it and append frames. */
struct SDKAudioMeterResultPayload {
	uint8_t direction[4];
	uint8_t generation[4];
	uint8_t frame[4];
	uint8_t frame_count[4];
	uint8_t flags[4];
	uint8_t identity[4];
	uint8_t clip_count[4];
	uint8_t underrun_count[4];
	uint8_t overrun_count[4];
	uint8_t gain_reduction_events[4];
	uint8_t peak_hold_ch1[4];
	uint8_t peak_hold_ch2[4];
};

struct SDKAudioSceneSavePayload {
	uint8_t scene[4];
	uint8_t flags[4];
	uint8_t reserved[40];
};

struct SDKAudioSceneSaveResultPayload {
	uint8_t status[4];
	uint8_t scene[4];
	uint8_t flags[4];
	uint8_t reserved[36];
};

struct SDKAudioControlStateGetPayload {
	uint8_t flags[4];
	uint8_t reserved[44];
};

/* active_scene is the current index; baseline and trim are packed
 * balance words; ceiling is the enforced combined-level boundary in
 * mixer-value units (combined levels above it clamp with a
 * gain-reduction event). */
struct SDKAudioControlStateResultPayload {
	uint8_t active_scene[4];
	uint8_t scene_count[4];
	uint8_t baseline[4];
	uint8_t trim[4];
	uint8_t ceiling[4];
	uint8_t flags[4];
	uint8_t reserved[24];
};

typedef char SDKAudioSceneSelectPayload_must_be_48_bytes[
	(sizeof(struct SDKAudioSceneSelectPayload) == 48U) ? 1 : -1];
typedef char SDKAudioSceneWritePayload_must_be_48_bytes[
	(sizeof(struct SDKAudioSceneWritePayload) == 48U) ? 1 : -1];
typedef char SDKAudioTrimSubmitPayload_must_be_48_bytes[
	(sizeof(struct SDKAudioTrimSubmitPayload) == 48U) ? 1 : -1];
typedef char SDKAudioTrimResultPayload_must_be_48_bytes[
	(sizeof(struct SDKAudioTrimResultPayload) == 48U) ? 1 : -1];
typedef char SDKAudioMeterReadPayload_must_be_48_bytes[
	(sizeof(struct SDKAudioMeterReadPayload) == 48U) ? 1 : -1];
typedef char SDKAudioMeterResultPayload_must_be_48_bytes[
	(sizeof(struct SDKAudioMeterResultPayload) == 48U) ? 1 : -1];
typedef char SDKAudioSceneSavePayload_must_be_48_bytes[
	(sizeof(struct SDKAudioSceneSavePayload) == 48U) ? 1 : -1];
typedef char SDKAudioSceneSaveResultPayload_must_be_48_bytes[
	(sizeof(struct SDKAudioSceneSaveResultPayload) == 48U) ? 1 : -1];
typedef char SDKAudioControlStateGetPayload_must_be_48_bytes[
	(sizeof(struct SDKAudioControlStateGetPayload) == 48U) ? 1 : -1];
typedef char SDKAudioControlStateResultPayload_must_be_48_bytes[
	(sizeof(struct SDKAudioControlStateResultPayload) == 48U) ? 1 : -1];

/* Non-volatile big-endian word accessors shared by the control-plane
 * dispatch and any payload pack/unpack that reads plain buffers. */
static inline uint32_t sdk_get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void sdk_put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline void sdk_meter_put_word(volatile uint8_t *dst, uint32_t v)
{
	dst[0] = (uint8_t)(v >> 24);
	dst[1] = (uint8_t)(v >> 16);
	dst[2] = (uint8_t)(v >> 8);
	dst[3] = (uint8_t)v;
}

/*
 * Pack one framed meter snapshot (U3, KTD3). The whole state fits a
 * single frame, so frame is 0 (the 0-based index of this frame; the
 * SDK client counts frames from 0) and frame_count is 1; all frames
 * of a logical read carry the snapshot's generation so a client can
 * detect a torn multi-frame read. flags are the RESULT flags: pass
 * SDK_AUDIO_METER_RESULT_HOLD_RESET when the serving read consumed
 * the peak-hold window. Fields are big-endian words, mirroring the
 * SDK ZZ9KAudioMeterResultPayload layout byte-for-byte.
 */

static inline void sdk_audio_meter_result_pack(
	volatile struct SDKAudioMeterResultPayload *out,
	const struct audio_meter_snapshot *snapshot, uint32_t flags)
{
	sdk_meter_put_word(out->direction, snapshot->direction);
	sdk_meter_put_word(out->generation, snapshot->generation);
	sdk_meter_put_word(out->frame, 0U);
	sdk_meter_put_word(out->frame_count, 1U);
	sdk_meter_put_word(out->flags, flags);
	sdk_meter_put_word(out->identity, snapshot->identity);
	sdk_meter_put_word(out->clip_count, snapshot->clip_count);
	sdk_meter_put_word(out->underrun_count, snapshot->underrun_count);
	sdk_meter_put_word(out->overrun_count, snapshot->overrun_count);
	sdk_meter_put_word(out->gain_reduction_events,
		snapshot->gain_reduction_events);
	sdk_meter_put_word(out->peak_hold_ch1, snapshot->peak_hold_ch1);
	sdk_meter_put_word(out->peak_hold_ch2, snapshot->peak_hold_ch2);
}

#define SDK_MAX_SHARED_BUFFERS         32U
#define SDK_MAX_SURFACES               16U
#define SDK_SURFACE_HANDLE_FRAMEBUFFER 0x80000000UL
#define SDK_SURFACE_HANDLE_BASE        0x40000000UL

#define SDK_SURFACE_FORMAT_UNKNOWN     0U
#define SDK_SURFACE_FORMAT_RGB565      1U
#define SDK_SURFACE_FORMAT_ARGB8888    2U
#define SDK_SURFACE_FORMAT_RGBA8888    3U
#define SDK_SURFACE_FORMAT_INDEX8      4U
#define SDK_SURFACE_FORMAT_PLANAR      5U
#define SDK_SURFACE_FORMAT_RGB555      6U
#define SDK_SURFACE_FORMAT_BGRA8888    7U
#define SDK_SURFACE_FORMAT_RGB888      8U

#define SDK_SURFACE_FLAG_CPU_VISIBLE   (1U << 0)
#define SDK_SURFACE_FLAG_FRAMEBUFFER   (1U << 1)
#define SDK_SURFACE_FLAG_DISPLAYED     (1U << 2)
#define SDK_SURFACE_FLAG_SHARED_BUFFER (1U << 3)
#define SDK_SURFACE_FLAG_ARM_LOCAL     (1U << 4)

#define SDK_SCALE_NEAREST              0U
#define SDK_SCALE_BILINEAR             1U
#define SDK_SCALE_BICUBIC              2U
#define SDK_SCALE_LANCZOS3             3U

#define SDK_INVALID_HANDLE             0xffffffffUL

#define SDK_MAX_IMAGE_SESSIONS         4U

#define SDK_IMAGE_CODEC_JPEG           1U
#define SDK_IMAGE_CODEC_PNG            2U
#define SDK_IMAGE_CODEC_GIF            3U

#define SDK_IMAGE_OUTPUT_SURFACE       1U
#define SDK_IMAGE_OUTPUT_FRAMEBUFFER   2U
#define SDK_IMAGE_OUTPUT_TILE_BUFFER   3U

#define SDK_IMAGE_DECODE_FLAG_FIT             (1U << 0)
#define SDK_IMAGE_DECODE_FLAG_PRESERVE_ASPECT (1U << 1)
#define SDK_IMAGE_DECODE_FLAG_DITHER          (1U << 2)

#define SDK_IMAGE_SESSION_FEED_EOF     (1U << 0)

#define SDK_IMAGE_SESSION_STATE_NEED_INPUT   1U
#define SDK_IMAGE_SESSION_STATE_HEADER_READY 2U
#define SDK_IMAGE_SESSION_STATE_TILE_READY   3U
#define SDK_IMAGE_SESSION_STATE_COMPLETE     4U
#define SDK_IMAGE_SESSION_STATE_ERROR        5U

#define SDK_IMAGE_SESSION_RESULT_HEADER_READY (1U << 0)
#define SDK_IMAGE_SESSION_RESULT_PARTIAL      (1U << 1)
#define SDK_IMAGE_SESSION_RESULT_SCALED       (1U << 2)

#define SDK_AUDIO_SAMPLE_FORMAT_NONE   0U
#define SDK_AUDIO_SAMPLE_FORMAT_S16LE  1U
#define SDK_AUDIO_SAMPLE_FORMAT_S16BE  2U
#define SDK_AUDIO_DECODE_FLAG_EXPECT_END (1U << 0)
#define SDK_AUDIO_DECODE_RESULT_END    (1U << 0)
#define SDK_MAX_AUDIO_STREAMS          4U
#define SDK_AUDIO_STREAM_FEED_EOF      (1U << 0)
#define SDK_AUDIO_STREAM_FEED_DRAIN    (1U << 1)
#define SDK_AUDIO_STREAM_STATE_NEED_INPUT 1U
#define SDK_AUDIO_STREAM_STATE_STREAMING  2U
#define SDK_AUDIO_STREAM_STATE_DONE       3U
#define SDK_AUDIO_STREAM_STATE_ERROR      4U
#define SDK_AUDIO_STREAM_RESULT_NEED_INPUT (1U << 0)
#define SDK_AUDIO_STREAM_RESULT_PCM_READY  (1U << 1)
#define SDK_AUDIO_STREAM_RESULT_DONE       (1U << 2)
#define SDK_AUDIO_STREAM_RESULT_BACKPRESSURE (1U << 3)
#define SDK_AUDIO_STREAM_RESULT_DRAINED    (1U << 4)

/* Direct-overlay output has one display binding. Multiple decoder sessions
 * would race to own that plane until the ABI grows an explicit binding. */
#define SDK_MAX_VIDEO_SESSIONS         1U
#define SDK_VIDEO_CODEC_MPEG1          1U
#define SDK_VIDEO_CONTAINER_MPEG_PS    1U
#define SDK_VIDEO_OUTPUT_DIRECT_OVERLAY 1U
#define SDK_VIDEO_SESSION_WRITE_EOF    (1U << 0)
#define SDK_VIDEO_SESSION_STATE_NEED_INPUT  1U
#define SDK_VIDEO_SESSION_STATE_READY       2U
#define SDK_VIDEO_SESSION_STATE_FRAME_READY 3U
#define SDK_VIDEO_SESSION_STATE_DONE        4U
#define SDK_VIDEO_SESSION_STATE_ERROR       5U
#define SDK_VIDEO_SESSION_RESULT_HEADER_READY (1U << 0)
#define SDK_VIDEO_SESSION_RESULT_NEED_INPUT   (1U << 1)
#define SDK_VIDEO_SESSION_RESULT_FRAME_READY  (1U << 2)
#define SDK_VIDEO_SESSION_RESULT_DONE         (1U << 3)

#define SDK_MEDIA_NO_PTS UINT64_C(0xffffffffffffffff)
#define SDK_MEDIA_AUDIO_NONE 0U
#define SDK_MEDIA_AUDIO_MP2 1U
#define SDK_MEDIA_SESSION_WRITE_EOF (1U << 0)
#define SDK_MEDIA_AUDIO_BIND_PAUSE (1U << 0)
#define SDK_MEDIA_SESSION_STATE_NEED_INPUT 1U
#define SDK_MEDIA_SESSION_STATE_READY 2U
#define SDK_MEDIA_SESSION_STATE_FRAME_HELD 3U
#define SDK_MEDIA_SESSION_STATE_DONE 4U
#define SDK_MEDIA_SESSION_STATE_ERROR 5U
#define SDK_MEDIA_SESSION_RESULT_HEADER_READY (1U << 0)
#define SDK_MEDIA_SESSION_RESULT_NEED_INPUT (1U << 1)
#define SDK_MEDIA_SESSION_RESULT_FRAME_HELD (1U << 2)
#define SDK_MEDIA_SESSION_RESULT_DONE (1U << 3)
#define SDK_MEDIA_SESSION_RESULT_DERIVED_TIME (1U << 4)
#define SDK_MEDIA_SESSION_RESULT_DISCONTINUITY (1U << 5)
#define SDK_MEDIA_SESSION_RESULT_REBASED (1U << 6)
#define SDK_MEDIA_SESSION_RESULT_AUDIO_READY (1U << 7)
#define SDK_MEDIA_SESSION_RESULT_BACKPRESSURE (1U << 8)
#define SDK_MEDIA_SESSION_RESULT_PRESENTED (1U << 9)
#define SDK_MEDIA_SESSION_RESULT_DISCARDED (1U << 10)
#define SDK_MEDIA_SESSION_RESULT_AUDIO_BOUND (1U << 11)
#define SDK_MEDIA_SESSION_RESULT_AUDIO_PLAYING (1U << 12)
#define SDK_MEDIA_SESSION_RESULT_AUDIO_DRAINED (1U << 13)
#define SDK_MEDIA_SESSION_RESULT_AUDIO_UNDERRUN (1U << 14)
#define SDK_MEDIA_STATUS_TIMING 0U
#define SDK_MEDIA_STATUS_AUDIO 1U
#define SDK_MEDIA_STATUS_COUNTERS 2U
#define SDK_MEDIA_STATUS_AUDIO_OUTPUT 3U
/* Overlay presentation path and geometry. Firmware older than this page
 * rejects it with BAD_REQUEST, which is the intended capability gate: a
 * client that gets BAD_REQUEST reports the path as unavailable. */
#define SDK_MEDIA_STATUS_PRESENTATION 4U
/* Per-stage pipeline timing (U7). Each value packs one stage as
 * (microseconds << 32) | calls, in SDK_MEDIA_PROFILE_* order. Firmware
 * older than this page rejects it with BAD_REQUEST, so a client reports
 * profiling as unavailable rather than failing. */
#define SDK_MEDIA_STATUS_PROFILE 5U
/* Set in the profile page's flags when the NEON YUY2 pack is the live
 * kernel, so a before/after comparison names what it measured. */
#define SDK_MEDIA_PROFILE_FLAG_NEON_PACK (1U << 0)

/* SDK_MEDIA_STATUS_PRESENTATION flag bits (status result `flags`). */
#define SDK_MEDIA_PRESENT_CONFIGURED (1U << 0)
#define SDK_MEDIA_PRESENT_ACTIVE (1U << 1)
#define SDK_MEDIA_PRESENT_NATIVE (1U << 2)
#define SDK_MEDIA_PRESENT_OWNED (1U << 3)

/* Each presentation value carries two 16-bit halves: width/height, or x/y.
 * Signed coordinates travel as their two's-complement 16-bit pattern and are
 * sign-extended by the reader. Mirrored as ZZ9K_MEDIA_PACK_PAIR in the SDK. */
#define SDK_MEDIA_PACK_PAIR(hi, lo) \
	(((uint64_t)(uint16_t)(hi) << 16) | (uint64_t)(uint16_t)(lo))

/* SDK_OP_QUERY_PALETTE flags. Bit 0 is reserved for the secondary (HI) CLUT,
 * which is not shadowed; queries setting it get UNSUPPORTED. */
#define SDK_PALETTE_QUERY_FLAG_SECONDARY  (1U << 0)

#define SDK_CRYPTO_HASH_NONE           0U
#define SDK_CRYPTO_HASH_SHA1           1U
#define SDK_CRYPTO_HASH_SHA256         2U
#define SDK_CRYPTO_HASH_POLY1305       6U
#define SDK_CRYPTO_HASH_FLAG_HMAC      (1U << 0)
#define SDK_CRYPTO_STREAM_NONE         0U
#define SDK_CRYPTO_STREAM_CHACHA20     1U
#define SDK_CRYPTO_AEAD_NONE           0U
#define SDK_CRYPTO_AEAD_CHACHA20_POLY1305 1U
#define SDK_CRYPTO_AEAD_AES128_GCM     2U
#define SDK_CRYPTO_AEAD_AES256_GCM     3U
#define SDK_CRYPTO_AEAD_FLAG_DECRYPT   (1U << 0)

/* The AEAD payload has no algorithm field, so the AEAD algorithm rides in the
 * flags field at bits 8-15. A zero algorithm nibble means the legacy default,
 * ChaCha20-Poly1305, so existing callers stay byte-compatible. */
#define SDK_CRYPTO_AEAD_ALG_SHIFT      8
#define SDK_CRYPTO_AEAD_ALG_MASK       (0xFFU << 8)
#define SDK_CRYPTO_AEAD_FLAG_GET_ALG(flags) \
	(((flags) & SDK_CRYPTO_AEAD_ALG_MASK) >> SDK_CRYPTO_AEAD_ALG_SHIFT)

#define SDK_COMPRESSION_NONE           0U
#define SDK_COMPRESSION_DEFLATE_RAW    1U
#define SDK_COMPRESSION_ZLIB           2U
#define SDK_COMPRESSION_GZIP           3U
#define SDK_COMPRESSION_LZ4_BLOCK      4U
#define SDK_COMPRESSION_LZMA_ALONE     5U
#define SDK_COMPRESSION_LZMA2          6U
#define SDK_COMPRESSION_LH1            7U
#define SDK_COMPRESSION_LH5            8U
#define SDK_COMPRESSION_LH6            9U
#define SDK_COMPRESSION_LH7            10U

/* ABI contract for single-op SDK_OP_DECOMPRESS with LH1/LH5/LH6/LH7: LZH
 * streams carry no end-of-stream marker, so dst_capacity IS the decode
 * length -- it must equal the member's EXACT uncompressed size. A larger
 * capacity decodes garbage past the real end and still reports OK (with a
 * wrong CRC), because the decoder has no other way to know where the
 * stream ends. The CRC-16 is returned in the completion's checksum field so
 * the client can verify. This does not apply to zlib/gzip/LZMA, which are
 * self-terminating. */

/* Batched LZH decode arena (SDK_OP_DECOMPRESS_BATCH). All fields
 * big-endian; offsets relative to the arena base (arena_handle's buffer +
 * arena_offset). Layout: header / desc[N] / compressed blob / output
 * region (EXTRACT only) / result[N]. Mirrors ZZ9K_BATCH_* in the SDK's
 * include/zz9k/abi.h.
 *
 * Arena v1 CRC contract: the per-member EXPECTED_CRC / FLAG_HAVE_CRC fields
 * are ADVISORY -- the firmware does NOT compare them. Each result row
 * carries the computed LHA CRC-16 in its checksum word, and the CLIENT
 * must verify it (the reply's members_ok/members_failed counts reflect
 * decode completion only, not CRC correctness). */
#define SDK_BATCH_ARENA_MAGIC          0x5A424154UL /* 'ZBAT' */
#define SDK_BATCH_ARENA_VERSION        1U
#define SDK_BATCH_MODE_TEST            0U
#define SDK_BATCH_MODE_EXTRACT         1U
#define SDK_BATCH_HEADER_SIZE          48U
#define SDK_BATCH_DESC_SIZE            32U
#define SDK_BATCH_RESULT_SIZE          16U
#define SDK_BATCH_MEMBER_LIMIT         1024U
#define SDK_BATCH_MEMBER_FLAG_HAVE_CRC (1U << 0)

/* TEST-mode members decode-and-discard, so uncompressed_size is not
 * bounded by any arena region. Cap it so a corrupt descriptor cannot pin
 * the core-1 worker for minutes producing discarded output. Mirrors
 * ZZ9K_BATCH_TEST_MAX_EXPECTED in the SDK's abi.h. */
#define SDK_BATCH_TEST_MAX_EXPECTED    0x04000000UL /* 64 MB */

#define SDK_BATCH_HDR_MAGIC            0U
#define SDK_BATCH_HDR_VERSION          4U  /* u16 */
#define SDK_BATCH_HDR_MODE             6U  /* u16 */
#define SDK_BATCH_HDR_MEMBER_COUNT     8U
#define SDK_BATCH_HDR_DESC_OFFSET      12U
#define SDK_BATCH_HDR_BLOB_OFFSET      16U
#define SDK_BATCH_HDR_BLOB_LENGTH      20U
#define SDK_BATCH_HDR_OUTPUT_OFFSET    24U
#define SDK_BATCH_HDR_OUTPUT_CAPACITY  28U
#define SDK_BATCH_HDR_RESULT_OFFSET    32U

#define SDK_BATCH_DESC_ALGORITHM       0U
#define SDK_BATCH_DESC_SRC_OFFSET      4U
#define SDK_BATCH_DESC_SRC_LENGTH      8U
#define SDK_BATCH_DESC_DST_OFFSET      12U
#define SDK_BATCH_DESC_UNCOMP_SIZE     16U
#define SDK_BATCH_DESC_EXPECTED_CRC    20U
#define SDK_BATCH_DESC_FLAGS           24U

#define SDK_BATCH_RESULT_STATUS        0U
#define SDK_BATCH_RESULT_BYTES_WRITTEN 4U
#define SDK_BATCH_RESULT_CHECKSUM      8U
#define SDK_BATCH_RESULT_RESERVED      12U

#define SDK_DECOMPRESS_FLAG_EXPECT_END (1U << 0)
#define SDK_DECOMPRESS_FLAG_FEED_INPUT (1U << 1)

#define SDK_DECOMPRESS_STREAM_FEED_EOF (1U << 0)

#define SDK_DECOMPRESS_RESULT_STREAM_END      (1U << 0)
#define SDK_DECOMPRESS_RESULT_CHECKSUM_VALID  (1U << 1)
#define SDK_DECOMPRESS_RESULT_NEED_INPUT      (1U << 2)

void sdk_mailbox_init(void);
/* Refresh descriptor capability bits after a late driver layout ack. */
void sdk_mailbox_refresh_capabilities(void);
void sdk_mailbox_activate(void);
void sdk_mailbox_doorbell(void);
void sdk_mailbox_ack_irq(void);
void sdk_mailbox_irq_enable(void);
void sdk_mailbox_irq_disable(void);
void sdk_mailbox_task(void);
uint16_t sdk_mailbox_status(void);
uint32_t sdk_mailbox_address(void);
/* After a core-1 fault: mark core-1-affine audio streams faulted so their
 * feeds/reads fail cleanly (the embedded decoder may be mid-frame). */
void sdk_mailbox_poison_core1_audio_streams(void);
/* Enqueue an internal (request_id 0, no client completion) core-1 task.
 * Core-0 main-loop context ONLY (single-producer queue). Returns 1 if
 * queued. */
int sdk_mailbox_enqueue_internal(uint32_t opcode, const void *params,
                                 uint32_t params_len);
/* AX playback pump, split in two:
 *  - sdk_mailbox_audio_playback_pump_isr: TX-fill half, called from the
 *    audio-formatter period interrupt (isr_audio, every 20 ms) so
 *    main-loop load cannot starve the TX frontier. Integer-only.
 *  - sdk_mailbox_audio_playback_pump: core-1 refill kick, call from the
 *    core-0 main loop every pass.
 * Both are no-ops when nothing is bound. */
void sdk_mailbox_audio_playback_pump(void);
void sdk_mailbox_audio_playback_pump_isr(void);
int sdk_mailbox_audio_playback_active(void);

/*
 * Run a crypto task's compute on the calling core. op_params points at one of
 * the crypto_*_params structs packed by the core-0 fronts; result_payload is a
 * 48-byte SDKCryptoResultPayload buffer written by the compute. Returns an
 * SDK_STATUS_* code. Shared by the dual-core scheduler's core-1 worker and the
 * core-0 inline/fallback path (see scheduler.h). Cache maintenance for the data
 * buffers is done inside, on whichever core runs it.
 */
uint16_t sdk_mailbox_run_crypto_task(uint16_t opcode, const void *op_params,
                                     uint8_t *result_payload);

/*
 * Opcode-dispatched offload executor: routes a claimed taskq_desc_t (see
 * scheduler.h) to its service handler on the calling core, filling
 * result_payload (a TASKQ_RESULT_PAYLOAD-byte buffer) and *result_len.
 * Both the crypto opcode class and SDK_OP_DECOMPRESS are wired up, each
 * byte-identical to the pre-extraction inline dispatch: full result-payload
 * length (SDKCryptoResultPayload or SDKDecompressResultPayload) on
 * SDK_STATUS_OK, zero otherwise. Used by the dual-core scheduler's core-1
 * worker and core-0 inline/fallback path (scheduler_arm.c: scheduler_run_slot).
 */
uint16_t sdk_mailbox_run_offload_task(const taskq_desc_t *d,
                                      uint8_t *result_payload,
                                      uint32_t *result_len);

/*
 * Post a deferred completion for a task the core-1 scheduler finished (see
 * scheduler_core0_poll). Returns 1 if posted, 0 if the completion ring is full
 * (retry) or the mailbox is inactive. Runs only on core 0's main loop.
 */
int sdk_mailbox_post_deferred(uint32_t request_id, uint32_t user_cookie,
                              uint16_t opcode, uint16_t status,
                              const uint8_t *payload, uint16_t payload_len);

/*
 * True while a harvested task's slot still belongs to the current mailbox
 * lifetime. scheduler_core0_poll drops (releases without posting) a task for
 * which this returns 0, so a task that outlived the mailbox it was submitted
 * under never posts a stale request_id/user_cookie into the new completion ring.
 */
int sdk_mailbox_task_gen_ok(int slot);

#endif /* SDK_MAILBOX_H */
