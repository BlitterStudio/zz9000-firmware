/*
 * MNT ZZ9000 Amiga Graphics and ARM Coprocessor Card Operating System
 * (ZZ9000OS)
 *
 * P96 video window (PIP) overlay. The scanout VDMA cannot composite, so
 * while an overlay is active the vblank ISR presents a shadow buffer
 * instead of the framebuffer; core 1 re-composites the shadow every
 * frame (full screen copy + color-keyed, scaled YUV overlay) via the
 * scheduler. Two shadows ping-pong: a compose only ever targets the
 * buffer that is not being scanned, and flips happen at vblank.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include "xil_cache.h"
#include "overlay.h"
#include "memorymap.h"
#include "surface_allocator.h"
#include "sdk_mailbox.h"
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
	uint8_t compose_target;
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
	ov.active = 0;
	ov.configured = 0;
	ov.compose_request = 0;
	ov.free_pending = 0;
	ov.front = -1;
	ov.ready_idx = -1;
	ov.shadow[0] = 0;
	ov.shadow[1] = 0;
	ov.shadow_size = 0;
	/* the mailbox reinit drops internal completions, so a compose that
	 * was queued/running never retires: clear the marker here or every
	 * post-reboot SET would fail on the in-flight guard forever. If it
	 * WAS running, drop whatever it still publishes. */
	ov.discard_stale = ov.compose_in_flight ? 1 : 0;
	ov.compose_in_flight = 0;
	ov.presenting = 0; /* the reset path reprograms the scanout */
	vs->card_feature_enabled[CARD_FEATURE_VIDEO_OVERLAY] = 0;
}

static void overlay_stop(void)
{
	ov.active = 0;
	ov.configured = 0;
	ov.compose_request = 0;
	ov.front = -1;
	ov.ready_idx = -1;
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
	    src_addr + (uint32_t)src_pitch * src_h > LEGACY_SURFACE_HEAP_END) {
		printf("[overlay] SET rejected: src %ux%u pitch %u dst %dx%d "
		       "act %d variant %u scale %d cm %d rows %lu stride %lu\n",
		       src_w, src_h, src_pitch, dst_w, dst_h, activating,
		       variant, vs->scalemode, vs->colormode,
		       (unsigned long)rows, (unsigned long)stride);
		status = OVERLAY_STATUS_BAD_PARAMS;
		goto out;
	}

	uint32_t need = stride * rows;
	if (ov.free_pending) {
		if (need == ov.shadow_size) {
			/* re-open while the previous overlay's deferred free is
			 * still pending: reclaim the buffers (the free is no
			 * longer wanted) and drop whatever the still-running
			 * compose publishes - it used the old parameters and
			 * possibly a freed source bitmap */
			ov.free_pending = 0;
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

out:
	data->u32_user[0] = status;
	SWAP32(data->u32_user[0]);
}

static int overlay_presentable(struct ZZ_VIDEO_STATE *vs)
{
	if (!ov.configured || !ov.active)
		return 0;
	if (!vs->card_feature_enabled[CARD_FEATURE_VIDEO_OVERLAY])
		return 0;
	/* no compositor without core 1: fall back to the live framebuffer
	 * instead of freezing the screen on the last composited shadow */
	if (!scheduler_core1_available())
		return 0;
	if (vs->split_pos != 0)
		return 0;
	if (vs->colormode != MNTVA_COLOR_32BIT &&
	    vs->colormode != MNTVA_COLOR_16BIT565 &&
	    vs->colormode != MNTVA_COLOR_15BIT)
		return 0;
	if ((uint32_t)vs->vmode_hsize != ov.snap_hsize ||
	    (uint32_t)vs->vmode_vsize != ov.snap_vsize ||
	    vs->colormode != ov.snap_colormode ||
	    vs->scalemode != 0)
		return 0;
	/* the scanout stride can change without a mode switch (OP_PAN with
	 * a different pan width); the shadows were laid out and the VDMA
	 * would scan them with mismatched strides -> hide until re-SET */
	if (video_vdma_stride_bytes(vs->vmode_hsize, vs->vmode_hdiv,
			vs->framebuffer_pan_width, stride_div) != ov.snap_stride)
		return 0;
	return 1;
}

uint32_t overlay_present_bufpos(struct ZZ_VIDEO_STATE *vs)
{
	uint32_t normal = (uint32_t)vs->framebuffer + vs->framebuffer_pan_offset;

	if (!overlay_presentable(vs)) {
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
	if (!ov.compose_request || ov.compose_in_flight)
		return;
	/* a completed frame is waiting for the flip: with two buffers both
	 * are spoken for (one scanned, one ready) - composing now would
	 * overwrite the buffer the next vblank presents */
	if (ov.ready_idx >= 0)
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

	if (sdk_mailbox_enqueue_internal(TASKQ_OP_VIDEO_COMPOSE, &p, sizeof(p))) {
		ov.compose_request = 0;
		ov.compose_in_flight = 1;
		ov.compose_target = target;
	}
}

uint16_t overlay_run_compose(const struct overlay_compose_params *p)
{
	overlay_composite_frame((uint8_t *)p->dst_addr, p->dst_pitch,
			(const uint8_t *)p->screen_addr, p->screen_pitch,
			p->scr_w, p->scr_h, p->color_format,
			(const uint8_t *)p->src_addr, p->src_pitch,
			p->src_w, p->src_h, p->variant,
			p->dst_x, p->dst_y, p->dst_w, p->dst_h,
			p->key_native, p->key_enabled);

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
}

void overlay_compose_retired(int ok)
{
	/* core 0 (main-loop harvest): the compose this retires targeted
	 * compose_target, which only core 0 writes. Failed/stranded tasks
	 * retire without publishing (the shadow was never composed). */
	if (ok && ov.compose_in_flight)
		ov.ready_idx = (int8_t)ov.compose_target;
	ov.compose_in_flight = 0;
}
