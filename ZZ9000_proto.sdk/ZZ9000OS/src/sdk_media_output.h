/*
 * Shared-buffer output of a media session: scale a decoded planar 4:2:0
 * picture and convert it to INDEX8, bitplanes, RGB565 or RGB555 in memory
 * the host can read.
 *
 * Everything here is pure computation with no firmware dependencies, so the
 * host suite compares it against a scalar reference. The core-1 task that
 * drives it, and the cache maintenance around it, live in
 * sdk_media_session.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_MEDIA_OUTPUT_H
#define SDK_MEDIA_OUTPUT_H

#include <stdint.h>

#include "sdk_mailbox.h"
#include "sdk_video_backend.h"

/* Largest output and largest source width. The scratch tables are sized for
 * these; the source height is only limited by the decoder's own geometry. */
#define SDK_MEDIA_OUTPUT_MAX_WIDTH 1920U
#define SDK_MEDIA_OUTPUT_MAX_HEIGHT 1088U
#define SDK_MEDIA_OUTPUT_MAX_SOURCE_WIDTH 1920U

/* A host palette is 256 big-endian 0x00RRGGBB words. */
#define SDK_MEDIA_OUTPUT_PALETTE_BYTES 1024U

#define SDK_MEDIA_OUTPUT_FLAGS_KNOWN \
	(SDK_MEDIA_OUTPUT_DEPTH_MASK | SDK_MEDIA_OUTPUT_PLANAR_INTERLEAVED | \
	 SDK_MEDIA_OUTPUT_DITHER_ORDERED | SDK_MEDIA_OUTPUT_DITHER_DIFFUSION | \
	 SDK_MEDIA_OUTPUT_PALETTE_HOST | SDK_MEDIA_OUTPUT_SCALE_NEAREST)

/* The flags this firmware accepts, reported on status page 6. Error-diffusion
 * dithering is not implemented: OUTPUT_CONFIG answers UNSUPPORTED for it. */
#define SDK_MEDIA_OUTPUT_FLAGS_SUPPORTED \
	(SDK_MEDIA_OUTPUT_FLAGS_KNOWN & ~SDK_MEDIA_OUTPUT_DITHER_DIFFUSION)

/* Decoded OUTPUT_CONFIG request, as the client sent it. */
struct SDKMediaOutputConfig {
	uint32_t width;
	uint32_t height;
	uint32_t format;        /* SDK_SURFACE_FORMAT_* */
	uint32_t pitch;
	uint32_t flags;         /* SDK_MEDIA_OUTPUT_* */
	uint32_t buffer_offset; /* slot 0, bytes from the buffer start */
	uint32_t slot_stride;   /* 0 = one slot */
};

/* What a validated configuration occupies. */
struct SDKMediaOutputLayout {
	uint32_t depth;         /* 1..8 for INDEX8 and PLANAR, 0 for RGB */
	uint32_t row_bytes;     /* smallest legal pitch */
	uint32_t picture_bytes; /* one picture */
	uint32_t slots;         /* 1 or 2 */
	uint32_t span_bytes;    /* from slot 0 to the end of the last slot */
};

/* Core-1 conversion job. It carries everything the task needs so core 1
 * never reads session state that core 0 may change. It fits the 48-byte
 * task parameter area. */
struct SDKMediaOutputJob {
	uint32_t session;
	uint32_t dst_addr;      /* ARM address of the slot */
	uint32_t palette_addr;  /* 0 unless PALETTE_HOST */
	uint32_t pitch;
	uint16_t width;
	uint16_t height;
	uint16_t flags;
	uint8_t format;
	uint8_t depth;
	uint8_t slot;
	uint8_t reserved[3];
};

/* Validates a configuration. Returns SDK_STATUS_OK, BAD_REQUEST for a
 * malformed one, or UNSUPPORTED for a format or flag this firmware does not
 * implement. `layout` is filled only on success. */
uint16_t sdk_media_output_check(const struct SDKMediaOutputConfig *cfg,
	                            struct SDKMediaOutputLayout *layout);

/* Bytes one picture occupies for a job (pitch * height * planes). */
uint32_t sdk_media_output_job_bytes(const struct SDKMediaOutputJob *job);

/* The fixed palette of an INDEX8 or PLANAR output of `depth` bits (1..8):
 * 256 words, 0x00RRGGBB, entries beyond the colours in use are zero.
 * Returns the number of colours in use. */
uint32_t sdk_media_output_fixed_palette(uint32_t depth, uint32_t out[256]);

/* Scales `frame` to the job's size and converts it into job->dst_addr.
 * `palette` is the host palette (SDK_MEDIA_OUTPUT_PALETTE_BYTES big-endian
 * bytes) when job->flags has PALETTE_HOST, else NULL. Returns
 * SDK_STATUS_OK or SDK_STATUS_BAD_REQUEST for a frame it cannot read. */
uint16_t sdk_media_output_convert(const struct SDKMediaOutputJob *job,
	                              const struct SDKVideoDecodedFrame *frame,
	                              const uint8_t *palette);

#endif /* SDK_MEDIA_OUTPUT_H */
