/*
 * Console encoder service (SDK service 0x8200): the encoder session.
 *
 * The mailbox handler resolves the request's surface and output buffer and
 * posts the completion; this file holds the persistent encoder state, checks
 * the request against the surface, takes the band-0 snapshot and runs the
 * RFB encoder.  It has no hardware dependency, so the host tests in
 * test/console build it as it is.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_CONSOLE_ENCODE_H
#define SDK_CONSOLE_ENCODE_H

#include <stdint.h>
#include <rfb/rfb_encode.h>

/* Request payload, inline, big-endian; mirrored as ZZ9KConsoleEncodePayload
 * in sdk/include/zz9k/abi.h. */
#define SDK_CONSOLE_REQ_SURFACE        0U
#define SDK_CONSOLE_REQ_OUT_HANDLE     4U
#define SDK_CONSOLE_REQ_OUT_CAPACITY   8U
#define SDK_CONSOLE_REQ_ENC_FLAGS      12U
#define SDK_CONSOLE_REQ_WIDTH          16U
#define SDK_CONSOLE_REQ_HEIGHT         18U
#define SDK_CONSOLE_REQ_BYTES_PER_ROW  20U
#define SDK_CONSOLE_REQ_TY0            22U
#define SDK_CONSOLE_REQ_TY1            24U
#define SDK_CONSOLE_REQ_FLAGS          26U
#define SDK_CONSOLE_REQ_CODEC          28U
#define SDK_CONSOLE_REQ_DEPTH          30U
#define SDK_CONSOLE_REQ_TILE_W         31U
#define SDK_CONSOLE_REQ_TILE_H         32U
#define SDK_CONSOLE_REQ_FORMAT         33U
#define SDK_CONSOLE_REQ_LEN            34U

/* Reply payload; mirrored as ZZ9KConsoleEncodeResultPayload. */
#define SDK_CONSOLE_REPLY_OUT_LEN      0U
#define SDK_CONSOLE_REPLY_CODEC        4U
#define SDK_CONSOLE_REPLY_FLAGS        6U
#define SDK_CONSOLE_REPLY_LEN          8U

#define SDK_CONSOLE_F_RESET            0x0001U  /* drop the delta baseline */
#define SDK_CONSOLE_RF_KEYFRAME        0x0001U  /* reply is a full frame */
#define SDK_CONSOLE_CODEC_NONE         0U

struct sdk_console_request {
	uint32_t surface_handle;
	uint32_t out_handle;
	uint32_t out_capacity;
	uint32_t enc_flags;
	uint16_t width;
	uint16_t height;
	uint16_t bytes_per_row;
	uint16_t ty0;
	uint16_t ty1;
	uint16_t flags;
	uint16_t codec;
	uint8_t depth;
	uint8_t tile_w;
	uint8_t tile_h;
	uint8_t format;
};

/* The surface the request names, as the mailbox resolved it. */
struct sdk_console_surface {
	uint32_t format;        /* SDK_SURFACE_FORMAT_* */
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t length;
	const uint8_t *pixels;
};

void sdk_console_parse_request(const uint8_t p[SDK_CONSOLE_REQ_LEN],
                               struct sdk_console_request *r);

/* Encode band [ty0, ty1) of the request into out.  Returns an SDK_STATUS_*;
 * on SDK_STATUS_OK *out_len holds the bytes written and *reply_flags the
 * SDK_CONSOLE_RF_* bits. */
uint16_t sdk_console_encode_band(const struct sdk_console_request *r,
                                 const struct sdk_console_surface *fb,
                                 uint8_t *out, uint32_t out_capacity,
                                 uint32_t *out_len, uint16_t *reply_flags);

/* A 32-bit screen (BGRA8888) converted to the wire's big-endian RGB565.
 * Bytes of each dst row past width * 2 are zeroed. */
void sdk_console_snap_bgra_rgb565(uint8_t *dst, uint32_t dst_pitch,
                                  const uint8_t *src, uint32_t src_pitch,
                                  uint32_t width, uint32_t height);

#endif
