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
#define SDK_CAP_MEMORY_OPS             (1U << 10)
#define SDK_CAP_DIAGNOSTICS            (1U << 11)
#define SDK_CAP_DOORBELL               (1U << 12)
#define SDK_CAP_POLLING_COMPLETION     (1U << 13)
#define SDK_CAP_SERVICE_DISCOVERY      (1U << 14)
#define SDK_CAP_SURFACE_OPS            (1U << 15)
#define SDK_CAP_COMPRESSION            (1U << 16)

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

#define SDK_SERVICE_FLAG_FIRMWARE      (1U << 0)
#define SDK_SERVICE_FLAG_MODULE        (1U << 1)
#define SDK_SERVICE_FLAG_ASYNC         (1U << 2)
#define SDK_SERVICE_FLAG_ZERO_COPY     (1U << 3)
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
#define SDK_SERVICE_FLAG_AUDIO_MP3_DECODE       (1U << 16)
#define SDK_SERVICE_FLAG_AUDIO_MP3_STREAM       (1U << 20)
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
#define SDK_SERVICE_FLAG_CRYPTO_X25519          (1U << 16)

#define SDK_OP_NOP                     0x0000U
#define SDK_OP_QUERY_CAPS              0x0001U
#define SDK_OP_PING                    0x0002U
#define SDK_OP_QUERY_SERVICE           0x0004U

#define SDK_OP_ALLOC_SHARED            0x0100U
#define SDK_OP_FREE_SHARED             0x0101U
#define SDK_OP_MEM_FILL                0x0102U
#define SDK_OP_MEM_COPY                0x0103U

#define SDK_OP_ALLOC_SURFACE           0x0200U
#define SDK_OP_FREE_SURFACE            0x0201U
#define SDK_OP_MAP_FRAMEBUFFER_SURFACE 0x0202U
#define SDK_OP_FILL_SURFACE            0x0203U
#define SDK_OP_COPY_SURFACE            0x0204U

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

#define SDK_OP_DECOMPRESS              0x0600U
#define SDK_OP_DECOMPRESS_TEST         0x0601U
#define SDK_OP_DECOMPRESS_STREAM_BEGIN 0x0602U
#define SDK_OP_DECOMPRESS_STREAM_READ  0x0603U
#define SDK_OP_DECOMPRESS_STREAM_CLOSE 0x0604U
#define SDK_OP_DECOMPRESS_STREAM_FEED  0x0605U

#define SDK_OP_CRYPTO_HASH             0x0800U
#define SDK_OP_CRYPTO_STREAM           0x0801U
#define SDK_OP_CRYPTO_AEAD             0x0802U
#define SDK_OP_CRYPTO_KX               0x0803U
#define SDK_CRYPTO_KX_X25519           1U
#define SDK_CRYPTO_KX_P256             2U

#define SDK_OP_CRYPTO_VERIFY           0x0804U
#define SDK_CRYPTO_VERIFY_ECDSA_P256_SHA256        1U
#define SDK_CRYPTO_VERIFY_RSA_PKCS1_2048_SHA256    2U

#define SDK_SERVICE_FLAG_CRYPTO_X25519     (1U << 16)
#define SDK_SERVICE_FLAG_CRYPTO_P256       (1U << 17)
#define SDK_SERVICE_FLAG_CRYPTO_ECDSA_P256 (1U << 18)
#define SDK_SERVICE_FLAG_CRYPTO_RSA_2048   (1U << 19)
#define SDK_SERVICE_FLAG_CRYPTO_AES_GCM    (1U << 20)

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
#define SDK_AUDIO_STREAM_STATE_NEED_INPUT 1U
#define SDK_AUDIO_STREAM_STATE_STREAMING  2U
#define SDK_AUDIO_STREAM_STATE_DONE       3U
#define SDK_AUDIO_STREAM_STATE_ERROR      4U
#define SDK_AUDIO_STREAM_RESULT_NEED_INPUT (1U << 0)
#define SDK_AUDIO_STREAM_RESULT_PCM_READY  (1U << 1)
#define SDK_AUDIO_STREAM_RESULT_DONE       (1U << 2)
#define SDK_AUDIO_STREAM_RESULT_BACKPRESSURE (1U << 3)

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

#define SDK_DECOMPRESS_FLAG_EXPECT_END (1U << 0)
#define SDK_DECOMPRESS_FLAG_FEED_INPUT (1U << 1)

#define SDK_DECOMPRESS_STREAM_FEED_EOF (1U << 0)

#define SDK_DECOMPRESS_RESULT_STREAM_END      (1U << 0)
#define SDK_DECOMPRESS_RESULT_CHECKSUM_VALID  (1U << 1)
#define SDK_DECOMPRESS_RESULT_NEED_INPUT      (1U << 2)

void sdk_mailbox_init(void);
void sdk_mailbox_activate(void);
void sdk_mailbox_doorbell(void);
void sdk_mailbox_ack_irq(void);
void sdk_mailbox_irq_enable(void);
void sdk_mailbox_irq_disable(void);
void sdk_mailbox_task(void);
uint16_t sdk_mailbox_status(void);
uint32_t sdk_mailbox_address(void);

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
 * Post a deferred completion for a task the core-1 scheduler finished (see
 * scheduler_core0_poll). Returns 1 if posted, 0 if the completion ring is full
 * (retry) or the mailbox is inactive. Runs only on core 0's main loop.
 */
int sdk_mailbox_post_deferred(uint32_t request_id, uint32_t user_cookie,
                              uint16_t opcode, uint16_t status,
                              const uint8_t *payload, uint16_t payload_len);

#endif /* SDK_MAILBOX_H */
