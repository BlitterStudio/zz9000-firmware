/* SDK service descriptors shared by firmware discovery and host tests. */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef SDK_SERVICE_CATALOG_H
#define SDK_SERVICE_CATALOG_H

#include <stdint.h>
#include <string.h>
#include "sdk_mailbox.h"

struct SDKServiceInfoPayload {
	uint8_t service_id[4];
	uint8_t version[4];
	uint8_t capability_bits[4];
	uint8_t flags[4];
	uint8_t opcode_base[4];
	uint8_t opcode_count[4];
	uint8_t max_inline_payload[4];
	uint8_t name[20];
};

struct SDKServiceDescriptor {
	uint32_t service_id;
	uint32_t version;
	uint32_t capability_bits;
	uint32_t flags;
	uint32_t opcode_base;
	uint32_t opcode_count;
	const char *name;
};

static const struct SDKServiceDescriptor sdk_services[] = {
	{
		.service_id = SDK_SERVICE_CORE,
		.version = 0x00020000U,
		.capability_bits = SDK_CAP_MAILBOX | SDK_CAP_POLLING_COMPLETION |
			SDK_CAP_SERVICE_DISCOVERY,
		.flags = SDK_SERVICE_FLAG_FIRMWARE,
		.opcode_base = SDK_SERVICE_CORE,
		.opcode_count = 6,
		.name = "core"
	},
	{
		.service_id = SDK_SERVICE_MEMORY,
		.version = 0x00020000U,
		.capability_bits = SDK_CAP_SHARED_ALLOC | SDK_CAP_MEMORY_OPS,
		.flags = SDK_SERVICE_FLAG_FIRMWARE,
		.opcode_base = SDK_SERVICE_MEMORY,
		.opcode_count = 4,
		.name = "memory"
	},
	{
		.service_id = SDK_SERVICE_SURFACE,
		.version = 0x00020000U,
		.capability_bits = SDK_CAP_SURFACES | SDK_CAP_FRAMEBUFFER_SURFACE |
			SDK_CAP_SURFACE_OPS,
		.flags = SDK_SERVICE_FLAG_FIRMWARE | SDK_SERVICE_FLAG_ZERO_COPY |
			SDK_SERVICE_FLAG_SURFACE_PALETTE_QUERY,
		.opcode_base = SDK_SERVICE_SURFACE,
		.opcode_count = 6,
		.name = "surface"
	},
	{
		.service_id = SDK_SERVICE_IMAGE,
		.version = 0x00020000U,
		.capability_bits = SDK_CAP_IMAGE_SCALE | SDK_CAP_IMAGE_DECODE,
		.flags = SDK_SERVICE_FLAG_FIRMWARE |
			SDK_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
			SDK_SERVICE_FLAG_IMAGE_TILE_OUTPUT |
			SDK_SERVICE_FLAG_IMAGE_FRAMEBUFFER_OUTPUT |
			SDK_SERVICE_FLAG_IMAGE_SCALE_BILINEAR |
			SDK_SERVICE_FLAG_IMAGE_SCALE_CLIPPED |
			SDK_SERVICE_FLAG_IMAGE_PNG_DIRECT_BGRA |
			SDK_SERVICE_FLAG_IMAGE_RGB888_OUTPUT |
			SDK_SERVICE_FLAG_IMAGE_SCALE_BGRA_TO_RGB555_RGB565,
		.opcode_base = SDK_SERVICE_IMAGE,
		.opcode_count = 8,
		.name = "image"
	},
	{
		.service_id = SDK_SERVICE_CODEC,
		.version = 0x00020000U,
		.capability_bits = SDK_CAP_COMPRESSION,
		.flags = SDK_SERVICE_FLAG_FIRMWARE |
			SDK_SERVICE_FLAG_CODEC_DEFLATE_RAW |
			SDK_SERVICE_FLAG_CODEC_ZLIB |
			SDK_SERVICE_FLAG_CODEC_GZIP |
			SDK_SERVICE_FLAG_CODEC_LZMA_ALONE |
			SDK_SERVICE_FLAG_CODEC_LZMA2 |
			SDK_SERVICE_FLAG_CODEC_CHECKSUM |
			SDK_SERVICE_FLAG_CODEC_DECOMPRESS_TEST |
			SDK_SERVICE_FLAG_CODEC_DECOMPRESS_STREAM |
			SDK_SERVICE_FLAG_CODEC_DECOMPRESS_FEED |
			SDK_SERVICE_FLAG_CODEC_DEFLATE_FEED |
			SDK_SERVICE_FLAG_CODEC_ZLIB_FEED |
			SDK_SERVICE_FLAG_CODEC_GZIP_FEED |
			SDK_SERVICE_FLAG_CODEC_LZH |
			SDK_SERVICE_FLAG_CODEC_DECOMPRESS_BATCH,
		.opcode_base = SDK_SERVICE_CODEC,
		.opcode_count = 7,	/* 0x0600..0x0606 incl. SDK_OP_DECOMPRESS_BATCH */
		.name = "codec"
	},
	{
		.service_id = SDK_SERVICE_AUDIO,
		.version = 0x00020001U,
		.capability_bits = SDK_CAP_AUDIO_DECODE | SDK_CAP_AUDIO_PLAYBACK |
			SDK_CAP_AUDIO_CONTROL | SDK_CAP_AUDIO_METERING |
			SDK_CAP_AUDIO_FABRIC,
		.flags = SDK_SERVICE_FLAG_FIRMWARE |
			SDK_SERVICE_FLAG_AUDIO_MP3_DECODE |
			SDK_SERVICE_FLAG_AUDIO_MP3_STREAM |
			SDK_SERVICE_FLAG_AUDIO_CONTROL |
			SDK_SERVICE_FLAG_AUDIO_FABRIC |
			SDK_SERVICE_FLAG_AUDIO_FABRIC_RATE,
		.opcode_base = SDK_SERVICE_AUDIO,
		.opcode_count = 21,	/* 0x0500..0x0514 incl. audio control plane and the
			 * fabric lease plane (0x0512-0x0514; 0x050f..0x0511
			 * reserved gaps); the on-hardware qualification gate
			 * passed 2026-08-28 (docs/audio-fabric.md), so the
			 * lease opcodes are counted and advertised */
		.name = "audio"
	},
	{
		.service_id = SDK_SERVICE_CRYPTO,
		.version = 0x00020000U,
		.capability_bits = SDK_CAP_CRYPTO,
		.flags = SDK_SERVICE_FLAG_FIRMWARE | SDK_SERVICE_FLAG_CRYPTO_X25519 |
			SDK_SERVICE_FLAG_CRYPTO_P256 |
			SDK_SERVICE_FLAG_CRYPTO_P256_KEYGEN |
			SDK_SERVICE_FLAG_CRYPTO_ECDSA_P256 |
			SDK_SERVICE_FLAG_CRYPTO_RSA_2048 |
			SDK_SERVICE_FLAG_CRYPTO_AES_GCM,
		.opcode_base = SDK_SERVICE_CRYPTO,
		.opcode_count = 5,
		.name = "crypto"
	},
	{
		.service_id = SDK_SERVICE_DIAG,
		.version = 0x00020000U,
		.capability_bits = SDK_CAP_DIAGNOSTICS,
		.flags = SDK_SERVICE_FLAG_FIRMWARE,
		.opcode_base = SDK_SERVICE_DIAG,
		.opcode_count = 4,
		.name = "diag"
	},
	{
		.service_id = SDK_SERVICE_VIDEO,
		.version = 0x00020000U,
		.capability_bits = SDK_CAP_VIDEO_DECODE | SDK_CAP_MEDIA_SESSION,
		.flags = SDK_SERVICE_FLAG_FIRMWARE |
			SDK_SERVICE_FLAG_ASYNC |
			SDK_SERVICE_FLAG_VIDEO_MPEG1 |
			SDK_SERVICE_FLAG_VIDEO_MPEG_PS |
			SDK_SERVICE_FLAG_VIDEO_DIRECT_OVERLAY |
			SDK_SERVICE_FLAG_VIDEO_STREAMING_INPUT |
			SDK_SERVICE_FLAG_VIDEO_CORE1 |
			SDK_SERVICE_FLAG_VIDEO_MEDIA_SESSION |
			SDK_SERVICE_FLAG_VIDEO_MEDIA_MP2 |
			SDK_SERVICE_FLAG_VIDEO_EXPLICIT_PRESENT |
			SDK_SERVICE_FLAG_VIDEO_TIMELINE_90KHZ |
			SDK_SERVICE_FLAG_VIDEO_PCM_RING_STATUS,
		.opcode_base = SDK_SERVICE_VIDEO,
		.opcode_count = 14,
		.name = "video"
	}
};

static const struct SDKServiceDescriptor *find_service(uint32_t service_id)
{
	uint32_t i;

	for (i = 0; i < sizeof(sdk_services) / sizeof(sdk_services[0]); i++) {
		if (sdk_services[i].service_id == service_id)
			return &sdk_services[i];
	}

	return 0;
}

static void sdk_service_put_be32(volatile uint8_t *dst, uint32_t value)
{
	dst[0] = (uint8_t)(value >> 24);
	dst[1] = (uint8_t)(value >> 16);
	dst[2] = (uint8_t)(value >> 8);
	dst[3] = (uint8_t)value;
}

static void sdk_service_write_info(volatile struct SDKServiceInfoPayload *info,
				   const struct SDKServiceDescriptor *service,
				   uint32_t capabilities, uint32_t flags,
				   uint32_t max_inline_payload)
{
	uint32_t i;

	memset((void *)info, 0, sizeof(*info));
	sdk_service_put_be32(info->service_id, service->service_id);
	sdk_service_put_be32(info->version, service->version);
	sdk_service_put_be32(info->capability_bits, capabilities);
	sdk_service_put_be32(info->flags, flags);
	sdk_service_put_be32(info->opcode_base, service->opcode_base);
	sdk_service_put_be32(info->opcode_count, service->opcode_count);
	sdk_service_put_be32(info->max_inline_payload, max_inline_payload);
	for (i = 0; i < sizeof(info->name) && service->name[i] != '\0'; i++)
		info->name[i] = (uint8_t)service->name[i];
}

#endif
