/*
 * Console encoder service (SDK service 0x8200): the encoder session.
 *
 * Offload of the httpd web-console framebuffer encode.  The 68k host maps
 * the displayed framebuffer, allocates a shared output buffer, and sends one
 * tile-row band [ty0,ty1) per call together with the encoder geometry it
 * configured; the shared RFB encoder (src/rfb) runs over a snapshot of the
 * framebuffer into that buffer.  Wire contract: AmiNetXDuo
 * src/tools/httpzz.h (HttpZzEncodeReq/Reply) and sdk/include/zz9k/abi.h
 * (ZZ9KConsoleEncodePayload), all ints big-endian, fields at fixed offsets.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_console_encode.h"

#include <stdlib.h>
#include <string.h>

#include "sdk_mailbox.h"

static rfb_encoder cenc_enc;
static rfb_geom    cenc_geom;
static rfb_u32     cenc_flags;
static rfb_u8     *cenc_shadow;
static rfb_u8     *cenc_scratch;
static uint32_t    cenc_shadow_len;
static uint32_t    cenc_scratch_len;
static int         cenc_ready;
static rfb_u8     *cenc_snap;       /* one coherent whole-frame copy per pass */
static uint32_t    cenc_snap_len;
/* The snapshot was taken by a band 0 under the current geometry and covers
 * it.  Cleared when the geometry changes: a later band of a new geometry
 * must not encode from a copy of the old, smaller frame. */
static int         cenc_snap_ok;
/* The client the encoder state belongs to, by its output buffer: the
 * request has no session field, and each client allocates its own shared
 * buffer.  Another buffer is another client. */
static uint32_t    cenc_owner;

static uint32_t rd_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t rd_be16(const uint8_t *p)
{
	return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}

void sdk_console_parse_request(const uint8_t p[SDK_CONSOLE_REQ_LEN],
                               struct sdk_console_request *r)
{
	r->surface_handle = rd_be32(p + SDK_CONSOLE_REQ_SURFACE);
	r->out_handle     = rd_be32(p + SDK_CONSOLE_REQ_OUT_HANDLE);
	r->out_capacity   = rd_be32(p + SDK_CONSOLE_REQ_OUT_CAPACITY);
	r->enc_flags      = rd_be32(p + SDK_CONSOLE_REQ_ENC_FLAGS);
	r->width          = rd_be16(p + SDK_CONSOLE_REQ_WIDTH);
	r->height         = rd_be16(p + SDK_CONSOLE_REQ_HEIGHT);
	r->bytes_per_row  = rd_be16(p + SDK_CONSOLE_REQ_BYTES_PER_ROW);
	r->ty0            = rd_be16(p + SDK_CONSOLE_REQ_TY0);
	r->ty1            = rd_be16(p + SDK_CONSOLE_REQ_TY1);
	r->flags          = rd_be16(p + SDK_CONSOLE_REQ_FLAGS);
	r->codec          = rd_be16(p + SDK_CONSOLE_REQ_CODEC);
	r->depth          = p[SDK_CONSOLE_REQ_DEPTH];
	r->tile_w         = p[SDK_CONSOLE_REQ_TILE_W];
	r->tile_h         = p[SDK_CONSOLE_REQ_TILE_H];
	r->format         = p[SDK_CONSOLE_REQ_FORMAT];
}

/* A 32-bit screen (BGRA8888: memory bytes B, G, R, A) served as RGB565, the
 * wire's truecolour format, big-endian.  Converting while the snapshot is
 * taken costs the ARM about what the copy did; without it the host reads four
 * bytes a pixel over Zorro and converts every one on the 68k. */
void sdk_console_snap_bgra_rgb565(uint8_t *dst, uint32_t dst_pitch,
                                  const uint8_t *src, uint32_t src_pitch,
                                  uint32_t width, uint32_t height)
{
	uint32_t y;

	for (y = 0u; y < height; y++) {
		const uint8_t *s = src + y * src_pitch;
		uint8_t *d = dst + y * dst_pitch;
		uint32_t x;

		for (x = 0u; x < width; x++, s += 4, d += 2) {
			uint32_t v = ((uint32_t)(s[2] & 0xf8u) << 8) |
			             ((uint32_t)(s[1] & 0xfcu) << 3) |
			             ((uint32_t)s[0] >> 3);
			d[0] = (uint8_t)(v >> 8);
			d[1] = (uint8_t)v;
		}
	}
}

/* The surface format an RFB chunky format is encoded from as it is. */
static uint32_t cenc_surface_format(rfb_u8 format)
{
	switch (format) {
	case RFB_FMT_CLUT8:
		return SDK_SURFACE_FORMAT_INDEX8;
	case RFB_FMT_RGB565:
		return SDK_SURFACE_FORMAT_RGB565;
	default:
		return SDK_SURFACE_FORMAT_UNKNOWN;
	}
}

static int cenc_geom_same(const rfb_geom *a, const rfb_geom *b)
{
	return a->width == b->width && a->height == b->height &&
	       a->bytes_per_row == b->bytes_per_row && a->depth == b->depth &&
	       a->tile_w == b->tile_w && a->tile_h == b->tile_h &&
	       a->format == b->format;
}

/* (Re)configure the persistent encoder for geometry g + flags.  The sequence
 * number is preserved across a reconfigure so the viewer never sees a spurious
 * gap.  Returns 1 on success, 0 if geometry is unusable or memory is short. */
static int cenc_configure(const rfb_geom *g, rfb_u32 flags)
{
	rfb_scroll_cfg cfg;
	uint32_t shadow_len;
	uint32_t scratch_len;
	rfb_u16 seq_keep = cenc_ready ? cenc_enc.seq : 0;

	rfb_scroll_defaults(&cfg);
	shadow_len = rfb_shadow_size(g);
	scratch_len = rfb_scratch_size(g, flags, &cfg);
	if (shadow_len == 0u || scratch_len == 0u)
		return 0;

	/* Build the replacement first and swap only on success.  Freeing
	 * the old buffers before the new ones existed left cenc_ready and the
	 * encoder pointing at freed memory when an allocation failed. */
	{
		rfb_u8 *shadow = cenc_shadow;
		rfb_u8 *scratch = cenc_scratch;
		rfb_encoder enc;

		if (shadow_len != cenc_shadow_len)
			shadow = (rfb_u8 *)malloc(shadow_len);
		if (scratch_len != cenc_scratch_len)
			scratch = (rfb_u8 *)malloc(scratch_len);
		if (!shadow || !scratch) {
			if (shadow && shadow != cenc_shadow)
				free(shadow);
			if (scratch && scratch != cenc_scratch)
				free(scratch);
			cenc_ready = 0;		/* the old state stays allocated, unused */
			return 0;
		}

		/* Zeroed shadow => the first band codes as a full frame from the
		 * same all-zero the viewer starts from. */
		memset(shadow, 0, shadow_len);
		if (rfb_encoder_init(&enc, g, flags, &cfg, shadow, shadow_len,
		                     scratch, scratch_len) != 0) {
			if (shadow != cenc_shadow)
				free(shadow);
			if (scratch != cenc_scratch)
				free(scratch);
			cenc_ready = 0;
			return 0;
		}

		if (shadow != cenc_shadow) {
			free(cenc_shadow);
			cenc_shadow = shadow;
			cenc_shadow_len = shadow_len;
		}
		if (scratch != cenc_scratch) {
			free(cenc_scratch);
			cenc_scratch = scratch;
			cenc_scratch_len = scratch_len;
		}
		cenc_enc = enc;
	}

	/* A new geometry voids the snapshot; a reset of the same one (the
	 * viewer's refresh, which may arrive on any band) does not. */
	if (!cenc_ready || !cenc_geom_same(g, &cenc_geom))
		cenc_snap_ok = 0;
	cenc_enc.seq = seq_keep;
	cenc_geom = *g;
	cenc_flags = flags;
	cenc_ready = 1;
	return 1;
}

uint16_t sdk_console_encode_band(const struct sdk_console_request *r,
                                 const struct sdk_console_surface *fb,
                                 uint8_t *out, uint32_t out_capacity,
                                 uint32_t *out_len, uint16_t *reply_flags)
{
	uint16_t width = r->width;
	uint16_t height = r->height;
	uint16_t bpr = r->bytes_per_row;
	rfb_geom g;
	const rfb_u8 *planes[1];
	uint8_t conv;
	long n;
	int reinit = 0;

	*out_len = 0u;
	*reply_flags = 0u;

	g.depth  = r->depth;
	g.tile_w = r->tile_w;
	g.tile_h = r->tile_h;
	g.format = r->format;

	/* The host frames bands from the geometry it sent; it must match the real
	 * surface or the pixel stride and tiling disagree.  Mismatch => let the
	 * host fall back to its own encode. */
	/* RGB565 asked of a 32-bit screen is converted into the snapshot, whose
	 * rows are the host's (bpr), not the card's. */
	conv = (uint8_t)(g.format == RFB_FMT_RGB565 &&
	                 fb->format == SDK_SURFACE_FORMAT_BGRA8888);
	if (width != fb->width || height != fb->height ||
	    (!conv && bpr != fb->pitch) ||
	    (conv && (uint32_t)width * 4u > fb->pitch))
		return SDK_STATUS_BAD_REQUEST;

	g.width = width;
	g.height = height;
	g.bytes_per_row = bpr;

	/* The encoder is handed one plane, the snapshot, so only the
	 * chunky formats are served (one plane whatever the depth); a planar
	 * request would make it read planes[1..depth-1], which do not exist.
	 * For chunky formats depth is bits per pixel (8 CLUT8, 16 RGB565) and
	 * must agree with the format, and the surface must hold that format
	 * (or BGRA8888, converted).  Pixel size alone is not enough: a 15-bit
	 * screen (RGB555) is two bytes a pixel too, and its bytes sent as
	 * RGB565 come out in the wrong colours. */
	if (!RFB_FMT_IS_CHUNKY(g.format) ||
	    g.depth != 8u * RFB_FMT_PIXEL_BYTES(g.format) ||
	    (!conv && fb->format != cenc_surface_format(g.format)) ||
	    (uint32_t)width * RFB_FMT_PIXEL_BYTES(g.format) > bpr ||
	    (!conv && (uint32_t)height * bpr > fb->length) ||
	    (conv && (uint32_t)height * fb->pitch > fb->length))
		return SDK_STATUS_BAD_REQUEST;

	/* One encoder, one owner.  A request through another output
	 * buffer starts a new session from a zeroed shadow and a fresh band 0,
	 * so no client is sent deltas computed against another's screen state. */
	if (cenc_ready && r->out_handle != cenc_owner) {
		cenc_ready = 0;
		cenc_snap_ok = 0;
	}

	if (!cenc_ready || cenc_flags != r->enc_flags ||
	    !cenc_geom_same(&g, &cenc_geom) || (r->flags & SDK_CONSOLE_F_RESET)) {
		if (!cenc_configure(&g, r->enc_flags))
			return SDK_STATUS_NO_MEMORY;
		cenc_owner = r->out_handle;
		reinit = 1;
	}

	/* One coherent snapshot of the whole framebuffer per pass.  The card reads
	 * the live framebuffer and an encode spans ~10-30 ms -- long enough for a
	 * dragged window to smear across it.  A single memcpy is a ~1 ms read that a
	 * drag cannot noticeably move, so band 0 copies the frame and every band of
	 * the pass encodes off the stable copy: clean AND live, no layer lock. */
	if (r->ty0 == 0) {
		uint32_t snap_need = conv ? (uint32_t)height * bpr : fb->length;

		if (cenc_snap_len < snap_need) {
			free(cenc_snap);
			cenc_snap = (rfb_u8 *)malloc(snap_need);
			cenc_snap_len = cenc_snap ? snap_need : 0u;
		}
		if (!cenc_snap)
			return SDK_STATUS_NO_MEMORY;
		/* Read the live RTG framebuffer COHERENTLY -- do NOT invalidate.
		 * The Amiga's RTG pixel writes enter the PS through the cache-coherent
		 * ACP port (MNTZorro m00_axi, AWCACHE=0x3), so they snoop/update the ARM
		 * caches directly while DDR can stay stale.  An invalidate here would
		 * DISCARD those freshly host-written lines and re-read stale DDR -- the
		 * "top rows live, everything below frozen" bug.  This matches overlay.c
		 * overlay_run_compose(), which reads this exact address with no
		 * invalidate; whole-frame coherency is kept by the per-vblank
		 * Xil_L1DCacheFlush()/Xil_L2CacheFlush() in video.c's isr_video(). */
		if (conv)
			sdk_console_snap_bgra_rgb565(cenc_snap, bpr, fb->pixels,
			                             fb->pitch, width, height);
		else
			memcpy(cenc_snap, fb->pixels, fb->length);
		cenc_snap_ok = 1;
	}
	/* A band past 0 encodes from the snapshot its pass's band 0 took; with
	 * none since the last reconfigure there is nothing valid to read. */
	if (!cenc_snap || !cenc_snap_ok ||
	    cenc_snap_len < (uint32_t)height * bpr)
		return SDK_STATUS_BAD_REQUEST;

	planes[0] = (const rfb_u8 *)cenc_snap;
	n = rfb_encode_band(&cenc_enc, planes, out, out_capacity, r->ty0, r->ty1);
	if (n < 0) {
		/* The encoder may already have advanced its sequence and
		 * shadow for bytes that were never delivered.  An error therefore
		 * ends the session: the next request reconfigures from a zeroed
		 * shadow, and the caller restarts from band 0 with
		 * SDK_CONSOLE_F_RESET, which yields a keyframe -- never a retry of
		 * the same band against a half-applied delta. */
		cenc_ready = 0;
		return SDK_STATUS_INTERNAL_ERROR;
	}

	if (reinit && r->ty0 == 0)
		*reply_flags |= SDK_CONSOLE_RF_KEYFRAME;
	*out_len = (uint32_t)n;
	return SDK_STATUS_OK;
}
