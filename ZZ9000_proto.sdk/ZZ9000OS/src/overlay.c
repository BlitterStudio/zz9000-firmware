/*
 * MNT ZZ9000 Amiga Graphics and ARM Coprocessor Card Operating System
 * (ZZ9000OS)
 *
 * P96 video window (PIP) overlay. Packed-YUV windows with a visible screen
 * intersection use the native PL overlay plane, including nearest-neighbour
 * scaling and clipping. Unsupported geometries retain the software shadow
 * compositor. SDK planar video frames are packed into two staging buffers
 * and handed to the same PL plane at vblank.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include "xil_cache.h"
#include "overlay.h"
#include "overlay_color.h"
#include "overlay_hw.h"
#include "overlay_schedule.h"
#include "memorymap.h"
#include "surface_allocator.h"
#include "sdk_mailbox.h"
#include "sdk_media_profile.h"
#include "sdk_media_session.h"
#include "sdk_video_stream.h"
#include "sdk_video_yuy2.h"
#include "scheduler.h"
#include "video_vdma.h"

enum overlay_status {
	OVERLAY_STATUS_OK = 0,
	OVERLAY_STATUS_BAD_PARAMS = 1,
	OVERLAY_STATUS_NO_MEMORY = 2,
};

static struct {
	volatile uint8_t configured;
	volatile uint8_t active;
	volatile uint8_t hw_active;
	uint8_t key_enabled;
	uint8_t variant;

	/* mode snapshot at SET time; the overlay hides on any mismatch */
	uint32_t snap_hsize, snap_vsize;
	uint32_t snap_rows;         /* scanned rows = vsize / vdiv */
	uint32_t snap_stride;       /* scanout stride in bytes */
	int snap_colormode;

	uint32_t src_addr;
	uint16_t src_pitch, src_w, src_h;
	int16_t dst_x, dst_y, dst_w, dst_h;
	uint32_t key_native;

	uint32_t shadow[2];
	uint32_t shadow_size;
	volatile int8_t front;      /* shadow being scanned, -1 none */
	volatile int8_t ready_idx;  /* completed shadow awaiting present */
	volatile uint8_t compose_request;   /* set in ISR, cleared by poll */
	volatile uint8_t compose_in_flight; /* core-0 main-loop owned */
	volatile uint8_t free_pending;
	volatile uint8_t discard_stale;     /* drop the next published frame */
	volatile uint8_t presenting;        /* ISR: scanout is on a shadow */
	volatile uint8_t mode_stale;        /* mode left the snapshot: latch
	                                     * hidden until a fresh SET (P96
	                                     * reallocates bitmaps across
	                                     * mode switches) */
	uint8_t compose_target;
	uint32_t direct_session;
	uint32_t hw_generation;
	uint32_t hw_scan_addr;
	volatile uint8_t hw_flip_pending;
	volatile uint8_t hw_restore_pending;
	uint32_t hw_handoff_addr;
	struct overlay_schedule_state schedule;
} ov;

extern uint8_t stride_div;

static void overlay_free_shadows(void)
{
	for (int i = 0; i < 2; i++) {
		if (ov.shadow[i]) {
			surface_allocator_free(ov.shadow[i]);
			ov.shadow[i] = 0;
		}
	}
	ov.shadow_size = 0;
}

/* Amiga reset: the legacy surface heap was just reinitialized, so the
 * shadow addresses are no longer ours - drop ALL overlay state without
 * touching the allocator. A compose still in flight may scribble into
 * the freshly reset heap once; the first P96 allocations happen
 * seconds later (driver boot), long after it retires. */
void overlay_amiga_reset(struct ZZ_VIDEO_STATE *vs)
{
	overlay_hw_stop();
	ov.active = 0;
	ov.hw_active = 0;
	ov.configured = 0;
	ov.compose_request = 0;
	ov.free_pending = 0;
	ov.front = -1;
	ov.ready_idx = -1;
	ov.shadow[0] = 0;
	ov.shadow[1] = 0;
	ov.shadow_size = 0;
	/* The mailbox reinit drops internal completions, so a compose that was
	 * queued/running never retires. Clear both markers or the first frame after
	 * reboot would be discarded for a completion that can no longer publish. */
	ov.discard_stale = 0;
	ov.compose_in_flight = 0;
	ov.presenting = 0; /* the reset path reprograms the scanout */
	ov.mode_stale = 0;
	ov.hw_flip_pending = 0;
	ov.hw_restore_pending = 0;
	ov.hw_handoff_addr = 0U;
	overlay_schedule_reset(&ov.schedule);
	ov.direct_session = 0U;
	ov.hw_scan_addr = 0U;
	vs->card_feature_enabled[CARD_FEATURE_VIDEO_OVERLAY] = 0;
}

static void overlay_stop(void)
{
	overlay_hw_stop();
	if (ov.compose_in_flight)
		ov.discard_stale = 1;
	ov.active = 0;
	ov.hw_active = 0;
	ov.configured = 0;
	ov.compose_request = 0;
	ov.front = -1;
	ov.ready_idx = -1;
	overlay_schedule_reset(&ov.schedule);
	ov.direct_session = 0U;
	ov.hw_scan_addr = 0U;
	ov.hw_flip_pending = 0;
	ov.hw_restore_pending = 0;
	ov.hw_handoff_addr = 0U;
	/* always deferred: core 1 may still be composing into a shadow,
	 * and the VDMA keeps scanning the presented one until the next
	 * vblank repoints it - the poll frees once both have moved on */
	if (ov.shadow_size)
		ov.free_pending = 1;
}

void overlay_handle_op(struct ZZ_VIDEO_STATE *vs, struct GFXData *data)
{
	uint32_t status = OVERLAY_STATUS_OK;

	if (data->u8_user[GFXDATA_U8_OVERLAY_SUBCMD] == OVERLAY_SUBCMD_OFF) {
		overlay_stop();
		data->u32_user[0] = 0;
		SWAP32(data->u32_user[0]);
		return;
	}

	uint32_t src_addr = (uint32_t)vs->framebuffer + data->offset[1];
	uint16_t src_pitch = data->pitch[1];
	int16_t dst_x = (int16_t)data->x[0];
	int16_t dst_y = (int16_t)data->y[0];
	int16_t dst_w = (int16_t)data->x[1];
	int16_t dst_h = (int16_t)data->y[1];
	uint16_t src_w = data->x[2];
	uint16_t src_h = data->y[2];
	uint16_t flags = data->user[0];
	uint8_t variant = data->u8_user[GFXDATA_U8_YUV_VARIANT];

	uint32_t rows = vs->vmode_vdiv ? (vs->vmode_vsize / vs->vmode_vdiv) : 0;
	uint32_t stride = video_vdma_stride_bytes(vs->vmode_hsize,
			vs->vmode_hdiv, vs->framebuffer_pan_width, stride_div);

	/* scalemode folds a scan-double divide into hdiv/vdiv; the
	 * compositor only understands plain progressive RTG modes. The
	 * screen depth must be convertible (no YUV->pen for 8-bit CLUT):
	 * rejecting here fails the PIP open cleanly instead of opening an
	 * invisible overlay. Source rows must hold whole 4-byte
	 * macropixels - an odd width still reads the full last one.
	 * The dest rect is only required when ACTIVATING: P96 creates the
	 * feature before the window geometry exists (captured logs show no
	 * FA_Left/Top/Width/Height at CreateFeature) and sends the rect
	 * with the activating SetFeatureAttrs. */
	int activating = (flags & 2) != 0;
	if (!src_w || !src_h ||
	    (activating && (dst_w < 1 || dst_h < 1)) ||
	    variant >= YUV422_VARIANT_NUM ||
	    vs->scalemode != 0 ||
	    (vs->colormode != MNTVA_COLOR_32BIT &&
	     vs->colormode != MNTVA_COLOR_16BIT565 &&
	     vs->colormode != MNTVA_COLOR_15BIT) ||
	    !rows || !stride ||
	    src_pitch < (((uint32_t)src_w + 1) / 2) * 4 ||
	    /* non-wrapping bounds: the product fits (65535*65535 < 2^32)
	     * but src_addr + span could wrap past the heap-end test, and
	     * offset[1] can wrap src_addr itself below the window. The
	     * range spans all card VRAM the driver can hand out: the
	     * source comes from P96's own pool (the board window, low)
	     * when rtg.library's constructor made it, or from the legacy
	     * surface heap (high) via the ZZ AllocBitMap hook. */
	    src_addr < (uint32_t)FRAMEBUFFER_ADDRESS ||
	    src_addr >= LEGACY_SURFACE_HEAP_END ||
	    (uint32_t)src_pitch * src_h >
	    (uint32_t)LEGACY_SURFACE_HEAP_END - src_addr) {
		printf("[overlay] SET rejected: src %ux%u pitch %u dst %dx%d "
		       "act %d variant %u scale %d cm %d rows %lu stride %lu\n",
		       src_w, src_h, src_pitch, dst_w, dst_h, activating,
		       variant, vs->scalemode, vs->colormode,
		       (unsigned long)rows, (unsigned long)stride);
		status = OVERLAY_STATUS_BAD_PARAMS;
		goto out;
	}

	uint32_t need = overlay_buffer_bytes(stride, rows, src_pitch, src_h);
	if (ov.free_pending) {
		if (need == ov.shadow_size) {
			/* re-open while the previous overlay's deferred free is
			 * still pending: reclaim the buffers (the free is no
			 * longer wanted) and drop whatever the still-running
			 * compose publishes - it used the old parameters and
			 * possibly a freed source bitmap */
			ov.free_pending = 0;
			if (ov.compose_in_flight)
				ov.discard_stale = 1;
		} else {
			/* wrong size and the old compose still owns the buffers;
			 * refuse - the driver fails/retries the open cleanly */
			status = OVERLAY_STATUS_NO_MEMORY;
			goto out;
		}
	} else if (need != ov.shadow_size) {
		if (ov.compose_in_flight || ov.presenting) {
			/* the old shadows are still owned by a running compose or
			 * by the scanout (until the next vblank): refuse for now,
			 * a later SET retries once they are quiescent */
			status = OVERLAY_STATUS_NO_MEMORY;
			goto out;
		}
		/* the vblank ISR preempts this handler: make the overlay
		 * unpresentable BEFORE the old shadows are freed, or a
		 * mid-surgery vblank can present a just-freed buffer;
		 * configured is restored below once the state is coherent */
		ov.configured = 0;
		ov.front = -1;
		ov.ready_idx = -1;
		overlay_free_shadows();
		ov.shadow[0] = surface_allocator_alloc(need);
		ov.shadow[1] = surface_allocator_alloc(need);
		if (!ov.shadow[0] || !ov.shadow[1]) {
			overlay_free_shadows();
			status = OVERLAY_STATUS_NO_MEMORY;
			goto out;
		}
		ov.shadow_size = need;
	}

	ov.src_addr = src_addr;
	ov.src_pitch = src_pitch;
	ov.src_w = src_w;
	ov.src_h = src_h;
	ov.dst_x = dst_x;
	ov.dst_y = dst_y;
	ov.dst_w = dst_w;
	ov.dst_h = dst_h;
	ov.variant = variant;
	ov.key_enabled = (flags & 1) ? 1 : 0;

	/* The key arrives via the pen convention (unswapped): the raw
	 * little-endian read of the 68k-written ULONG already matches the
	 * in-memory pixel form on 32-bit screens; on 16/15-bit screens the
	 * stored (byte-swapped) u16 sits in the top half. */
	if (vs->colormode == MNTVA_COLOR_32BIT)
		ov.key_native = data->u32_user[1];
	else
		ov.key_native = data->u32_user[1] >> 16;

	ov.snap_hsize = vs->vmode_hsize;
	ov.snap_vsize = vs->vmode_vsize;
	ov.snap_rows = rows;
	ov.snap_stride = stride;
	ov.snap_colormode = vs->colormode;

	ov.configured = 1;
	ov.active = (flags & 2) ? 1 : 0;
	/* a fresh SET re-validated everything against the live mode */
	ov.mode_stale = 0;

	/* Every P96 and cgxvideo client reaches the PL scaler through the existing
	 * driver ABI; no player-specific interface is involved. Keep the software
	 * compositor for rectangles that do not intersect the screen or exceed
	 * the native line fetcher's source bounds. */
	uint8_t was_hw_active = ov.hw_active;
	uint32_t hw_src_addr = ov.src_addr;
	int32_t dst_right = (int32_t)dst_x + dst_w;
	int32_t dst_bottom = (int32_t)dst_y + dst_h;

	if (ov.compose_in_flight)
		ov.discard_stale = 1;
	if (was_hw_active && ov.direct_session != 0U && ov.front >= 0)
		hw_src_addr = ov.shadow[ov.front];
	ov.hw_restore_pending = 0;
	ov.hw_handoff_addr = 0U;
	ov.hw_active = 0;
	if (overlay_hw_supported() && ov.active &&
	    dst_w > 0 && dst_h > 0 &&
	    dst_right > 0 && dst_bottom > 0 &&
	    dst_x < (int32_t)vs->vmode_hsize &&
	    dst_y < (int32_t)rows) {
		ov.hw_generation++;
		if (ov.hw_generation == 0U)
			ov.hw_generation = 1U;
		ov.hw_active = overlay_hw_start_scaled(
			hw_src_addr, ov.src_pitch, ov.src_w, ov.src_h,
			ov.dst_x, ov.dst_y, (uint16_t)ov.dst_w,
			(uint16_t)ov.dst_h, ov.variant,
			overlay_key_to_rgb(ov.key_native, ov.snap_colormode),
			ov.key_enabled, ov.hw_generation) ? 1U : 0U;
		if (ov.hw_active)
			ov.hw_scan_addr = hw_src_addr;
	}
	if (was_hw_active && !ov.hw_active) {
		overlay_hw_stop();
		ov.hw_scan_addr = 0U;
	}
	/* Geometry/source changes need a fresh staged frame when the direct PL
	 * path is ineligible or an SDK session later supplies planar data. */
	if (was_hw_active != ov.hw_active) {
		/* A hardware staging shadow is packed YUV; a software shadow is a
		 * full RGB screen. Never let either path present the other's content. */
		ov.front = -1;
		ov.ready_idx = -1;
		ov.hw_flip_pending = 0;
		ov.hw_restore_pending = 0;
		ov.hw_handoff_addr = 0U;
	}
	ov.compose_request = (ov.hw_active && ov.direct_session == 0U) ? 0U : 1U;

	/* UART fingerprint: proves which firmware build handled the SET
	 * (stale-BOOT.bin bench rounds are otherwise undetectable) and
	 * captures the geometry the compositor will run with */
	printf("[overlay] SET ok build " __DATE__ " " __TIME__
	       " src %lux%lu pitch %u dst %d,%d %dx%d act %d "
	       "scr %lux%lu stride %lu cm %d\n",
	       (unsigned long)src_w, (unsigned long)src_h, src_pitch,
	       dst_x, dst_y, dst_w, dst_h, ov.active,
	       (unsigned long)vs->vmode_hsize, (unsigned long)rows,
	       (unsigned long)stride, vs->colormode);

out:
	data->u32_user[0] = status;
	SWAP32(data->u32_user[0]);
}

void overlay_path_snapshot(uint32_t session, struct overlay_path_info *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	out->configured = ov.configured ? 1U : 0U;
	/* mode_stale latches the overlay hidden until a fresh SET, so an
	 * overlay that is "enabled" but stale is not presenting anything and
	 * must not be reported as an active path. */
	out->active = (ov.configured && ov.active && !ov.mode_stale) ? 1U : 0U;
	out->hw_active = ov.hw_active ? 1U : 0U;
	out->owns_session =
		(session != 0U && ov.direct_session == session) ? 1U : 0U;
	out->src_w = ov.src_w;
	out->src_h = ov.src_h;
	out->dst_x = ov.dst_x;
	out->dst_y = ov.dst_y;
	out->dst_w = ov.dst_w;
	out->dst_h = ov.dst_h;
	out->screen_w = (uint16_t)ov.snap_hsize;
	out->screen_h = (uint16_t)ov.snap_rows;
}

static int overlay_presentable(struct ZZ_VIDEO_STATE *vs)
{
	if (!ov.configured || !ov.active || ov.mode_stale)
		return 0;
	if (!vs->card_feature_enabled[CARD_FEATURE_VIDEO_OVERLAY])
		return 0;
	/* no compositor without core 1: fall back to the live framebuffer
	 * instead of freezing the screen on the last composited shadow */
	if (!ov.hw_active && !scheduler_core1_available())
		return 0;
	if (vs->split_pos != 0)
		return 0;
	/* a snapshot mismatch LATCHES hidden (mode_stale) until the driver
	 * re-SETs: P96 tears down or reallocates bitmaps across mode
	 * switches, so a mode that later returns to the same geometry must
	 * not resurrect the old source/shadow state. The transient hides
	 * above (split, core 1, feature gate) resume by themselves.
	 * Unsupported color modes (8-bit CLUT) need no separate check: the
	 * snapshot is always a supported mode (SET rejects the rest), so
	 * they latch here as a colormode mismatch - an early return before
	 * this block would skip the latch and resurrect the old overlay
	 * when the original mode comes back. */
	if ((uint32_t)vs->vmode_hsize != ov.snap_hsize ||
	    (uint32_t)vs->vmode_vsize != ov.snap_vsize ||
	    vs->colormode != ov.snap_colormode ||
	    vs->scalemode != 0) {
		ov.mode_stale = 1;
		return 0;
	}
	/* the scanout stride can change without a mode switch (OP_PAN with
	 * a different pan width); the shadows were laid out and the VDMA
	 * would scan them with mismatched strides */
	if (video_vdma_stride_bytes(vs->vmode_hsize, vs->vmode_hdiv,
			vs->framebuffer_pan_width, stride_div) != ov.snap_stride) {
		ov.mode_stale = 1;
		return 0;
	}
	return 1;
}

uint32_t overlay_present_bufpos(struct ZZ_VIDEO_STATE *vs)
{
	uint32_t normal = (uint32_t)vs->framebuffer + vs->framebuffer_pan_offset;

	if (!overlay_presentable(vs)) {
		if (ov.hw_active) {
			overlay_hw_stop();
			ov.hw_active = 0;
		}
		ov.presenting = 0;
		return normal;
	}
	if (ov.hw_active) {
		if (ov.ready_idx >= 0) {
			ov.front = ov.ready_idx;
			ov.ready_idx = -1;
			ov.hw_flip_pending = 1;
		}
		ov.presenting = 0;
		return normal;
	}

	if (ov.ready_idx >= 0) {
		if (ov.discard_stale) {
			/* frame composed with pre-re-open parameters: drop it */
			ov.discard_stale = 0;
			ov.ready_idx = -1;
		} else {
			ov.front = ov.ready_idx;
			ov.ready_idx = -1;
		}
	}
	/* Legacy overlay writers cannot signal source changes, so retain their
	 * refresh-driven behavior. SDK video decode completions explicitly mark
	 * new frames and avoid recomposing the same frame at every vblank. */
	if (overlay_schedule_refresh_driven(&ov.schedule))
		ov.compose_request = 1;

	if (ov.front < 0) {
		ov.presenting = 0;
		return normal;
	}
	ov.presenting = 1;
	return ov.shadow[ov.front];
}

void overlay_main_poll(struct ZZ_VIDEO_STATE *vs)
{
	if (ov.free_pending && !ov.compose_in_flight && !ov.presenting) {
		ov.free_pending = 0;
		overlay_free_shadows();
	}

	if (!ov.configured || !ov.active)
		return;
	if (ov.hw_active && ov.direct_session == 0U)
		return;
	if (!ov.compose_request || ov.compose_in_flight)
		return;
	/* a completed frame is waiting for the flip: with two buffers both
	 * are spoken for (one scanned, one ready) - composing now would
	 * overwrite the buffer the next vblank presents */
	if (ov.ready_idx >= 0)
		return;
	if (ov.hw_active &&
	    (ov.hw_flip_pending || ov.hw_handoff_addr != 0U))
		return;
	if (!scheduler_core1_available())
		return;

	struct overlay_compose_params p;
	uint8_t target = (ov.front == 0) ? 1 : 0;

	p.dst_addr = ov.shadow[target];
	p.dst_pitch = ov.snap_stride;
	p.screen_addr = (uint32_t)vs->framebuffer + vs->framebuffer_pan_offset;
	p.screen_pitch = ov.snap_stride;
	p.src_addr = ov.src_addr;
	p.key_native = ov.key_native;
	/* vmode_hsize is the mode's PIXEL width; the bytes-per-pixel
	 * factor lives in hdiv (video_mode_init_internal), which the
	 * stride already accounts for */
	p.scr_w = (uint16_t)ov.snap_hsize;
	p.scr_h = (uint16_t)ov.snap_rows;
	p.src_pitch = ov.src_pitch;
	p.src_w = ov.src_w;
	p.src_h = ov.src_h;
	p.dst_x = ov.dst_x;
	p.dst_y = ov.dst_y;
	p.dst_w = ov.dst_w;
	p.dst_h = ov.dst_h;
	p.color_format = (uint8_t)ov.snap_colormode;
	p.variant = ov.variant;
	p.key_enabled = ov.key_enabled;
	p.target_idx = target;
	if (ov.direct_session != 0U) {
		p.src_addr = ov.direct_session;
		if (ov.hw_active) {
			/* Convert decoder-owned planar420 into the non-scanned packed
			 * staging buffer using the pitch already programmed into the PL
			 * VDMA. PL still performs RGB conversion/key/composition; this is
			 * roughly half a frame write, not a full-screen shadow. */
			p.dst_addr = ov.shadow[target];
			p.dst_pitch = overlay_staging_pitch(ov.src_w, ov.src_pitch);
			p.scr_h = ov.src_h;
			p.variant = 0xfeU;
		} else {
			p.variant = 0xffU;
		}
	}

	if (sdk_mailbox_enqueue_internal(TASKQ_OP_VIDEO_COMPOSE, &p, sizeof(p))) {
		ov.compose_request = 0;
		ov.compose_in_flight = 1;
		ov.compose_target = target;
		if (ov.direct_session != 0U)
			sdk_media_session_present_queued(ov.direct_session);
	}
}

uint16_t overlay_run_compose(const struct overlay_compose_params *p)
{
	/* No invalidation needed for the host-written source/screen: the
	 * Zorro bridge enters the PS through the cache-coherent ACP port
	 * (see the flush comment in isr_video), so host writes snoop the
	 * ARM caches. A defensive L1 range-invalidate pass here was
	 * benched as a no-op and cost milliseconds per frame. */
	if (p->variant == 0xfeU) {
		struct SDKVideoDecodedFrame frame;
		uint32_t bytes_written;
		uint32_t profile_start;
		int packed;

		if (!sdk_video_stream_get_direct_frame(p->src_addr, &frame) ||
		    frame.width != p->src_w || frame.height != p->src_h)
			return SDK_STATUS_BAD_REQUEST;
		/* Timed on its own: whether this pack is material against decode
		 * is what gates the planar FPGA subproject (R13, U7). */
		profile_start = sdk_media_profile_now_us();
		packed = sdk_video_yuv420_to_yuy2((uint8_t *)p->dst_addr,
			p->dst_pitch, frame.width, frame.height,
			frame.y, frame.y_pitch, frame.cb, frame.cr,
			frame.chroma_pitch, &bytes_written);
		sdk_media_profile_record(
			SDK_MEDIA_PROFILE_YUY2_PACK, profile_start);
		if (!packed)
			return SDK_STATUS_BAD_REQUEST;
	} else if (p->variant == 0xffU) {
		struct SDKVideoDecodedFrame frame;

		if (sdk_video_stream_get_direct_frame(p->src_addr, &frame) &&
		    frame.width == p->src_w && frame.height == p->src_h) {
			overlay_composite_planar420_frame(
				(uint8_t *)p->dst_addr, p->dst_pitch,
				(const uint8_t *)p->screen_addr, p->screen_pitch,
				p->scr_w, p->scr_h, p->color_format,
				frame.y, frame.y_pitch, frame.cb, frame.cr,
				frame.chroma_pitch, frame.width, frame.height,
				p->dst_x, p->dst_y, p->dst_w, p->dst_h,
				p->key_native, p->key_enabled);
		} else {
			overlay_composite_frame((uint8_t *)p->dst_addr, p->dst_pitch,
				(const uint8_t *)p->screen_addr, p->screen_pitch,
				p->scr_w, p->scr_h, p->color_format, 0, 0, 0, 0, 0,
				p->dst_x, p->dst_y, p->dst_w, p->dst_h,
				p->key_native, p->key_enabled);
		}
	} else {
		overlay_composite_frame((uint8_t *)p->dst_addr, p->dst_pitch,
				(const uint8_t *)p->screen_addr, p->screen_pitch,
				p->scr_w, p->scr_h, p->color_format,
				(const uint8_t *)p->src_addr, p->src_pitch,
				p->src_w, p->src_h, p->variant,
				p->dst_x, p->dst_y, p->dst_w, p->dst_h,
				p->key_native, p->key_enabled);
	}

	/* L1-ONLY flush, deliberately: Xil_DCacheFlushRange would issue
	 * megabytes of line operations against the shared PL310 L2
	 * controller every frame, concurrently with the vblank ISR's full
	 * L2 way-flush on core 0 - the BSP cache routines are not
	 * multi-core safe against each other and the contention can stall
	 * the ISR (and with it Zorro DTACK). CP15 L1 ops are per-core and
	 * touch no shared hardware; the ISR's existing per-vblank full L2
	 * flush (video.c, "do not make conditional") carries the lines the
	 * rest of the way to DDR before the VDMA scans them. */
	Xil_L1DCacheFlushRange((INTPTR)p->dst_addr,
			(INTPTR)p->dst_pitch * p->scr_h);

	/* No ov state is touched here: the completed frame is published by
	 * overlay_compose_retired() on CORE 0 (via the coherent task-queue
	 * harvest), so every overlay field stays single-core. */
	return 0; /* SDK_STATUS_OK */
}

/* Called when the task queue is quiesced and reinitialized outside an
 * Amiga reset (firmware-update path): any queued/running compose was
 * drained or dropped without a deferred completion, so the in-flight
 * marker must be cleared here or no frame would ever compose again.
 * A dropped compose never publishes, so this is the only cleanup. */
void overlay_scheduler_reset(void)
{
	ov.compose_in_flight = 0;
	ov.discard_stale = 0;
	overlay_schedule_reset(&ov.schedule);
	ov.direct_session = 0U;
	if (ov.configured && ov.active)
		ov.compose_request = 1;
}

int overlay_video_frame_ready(uint32_t session)
{
	/* Legacy decode retirement runs before overlay_main_poll. Media sessions
	 * additionally keep their decoder frame held until that poll confirms the
	 * compose was enqueued ahead of any later decode. */
	if (!ov.configured || !ov.active)
		return 0;
	/* Decoder-owned planar420 is packed into the non-scanned YUV staging
	 * buffer on core 1, then flipped into the same native PL plane at vblank.
	 * A later planar fetcher can remove this final compact conversion. */
	overlay_schedule_frame_ready(&ov.schedule, session);
	ov.direct_session = session;
	ov.compose_request = 1;
	return 1;
}

void overlay_video_session_closed(uint32_t session)
{
	overlay_schedule_session_closed(&ov.schedule, session);
	if (ov.direct_session == session) {
		ov.direct_session = 0U;
		if (ov.compose_in_flight)
			ov.discard_stale = 1;
		if (ov.hw_active) {
			/* Return ownership to the live P96 bitmap at the next flushed
			 * vblank; the VDMA may currently point at an SDK staging shadow. */
			ov.front = -1;
			ov.ready_idx = -1;
			ov.hw_flip_pending = 0;
			ov.hw_handoff_addr = 0U;
			ov.hw_restore_pending = 1;
		}
	}
	/* If this was the final SDK producer, immediately restore the legacy
	 * periodic policy rather than waiting an extra vblank to request work. */
	if (overlay_schedule_refresh_driven(&ov.schedule) &&
	    ov.configured && ov.active)
		ov.compose_request = 1;
}

int overlay_scanout_active(void)
{
	return ov.presenting;
}

void overlay_scanout_released(void)
{
	ov.presenting = 0;
}

static void overlay_hw_rearm(uint32_t address)
{
	if (!address)
		return;
	ov.hw_generation++;
	if (ov.hw_generation == 0U)
		ov.hw_generation = 1U;
	overlay_hw_set_buffer(address, ov.hw_generation);
	ov.hw_scan_addr = address;
}

void overlay_vblank_rearm(void)
{
	uint32_t address;

	if (!ov.hw_active)
		return;
	/* Early rearm is safe only for immutable SDK staging (shadows or
	 * direct-session frames flushed before publication). A legacy
	 * P96/cgxvideo source is the host-written src_addr: restarting the
	 * non-coherent VDMA before the mandatory global cache flush can
	 * fetch stale or partially flushed source data (PR #88 review), so
	 * the legacy path rearms after the flush instead (in
	 * overlay_vblank_cache_flushed). */
	if (ov.direct_session == 0U && ov.hw_scan_addr == ov.src_addr)
		return;
	/* The currently scanned staging buffer was flushed before it was first
	 * published and is immutable while scanned. Restart it at vblank entry,
	 * before unrelated dirty cache lines can delay the mandatory global
	 * flush and leave the native-overlay VDMA stalled on a black frame. A
	 * newly composed buffer becomes eligible one vblank after that flush. */
	address = overlay_vblank_take_rearm(
		ov.hw_scan_addr, &ov.hw_handoff_addr);
	overlay_hw_rearm(address);
}

void overlay_vblank_cache_flushed(void)
{
	if (!ov.hw_active)
		return;
	/* Do not reset the VDMA a second time in the same vblank. Remember the
	 * freshly flushed handoff for the next vblank's early rearm; main-poll
	 * composition stays blocked until that handoff has been consumed. */
	if (ov.hw_restore_pending) {
		ov.hw_restore_pending = 0;
		ov.hw_handoff_addr = ov.src_addr;
	} else if (ov.hw_flip_pending && ov.front >= 0) {
		ov.hw_flip_pending = 0;
		ov.hw_handoff_addr = ov.shadow[ov.front];
	}

	/* Legacy direct sources skipped the early rearm: their scan address
	 * is the host-written src_addr, so restart the VDMA only now that
	 * the global cache flush has landed (same vblank, correct order). */
	if (ov.direct_session == 0U && ov.hw_scan_addr == ov.src_addr)
		overlay_hw_rearm(ov.hw_scan_addr);
}

void overlay_compose_retired(int ok)
{
	/* core 0 (main-loop harvest): the compose this retires targeted
	 * compose_target, which only core 0 writes. Failed/stranded tasks
	 * retire without publishing (the shadow was never composed). */
	if (ov.compose_in_flight) {
		if (ov.discard_stale)
			ov.discard_stale = 0;
		else if (ok)
			ov.ready_idx = (int8_t)ov.compose_target;
	}
	ov.compose_in_flight = 0;
}
