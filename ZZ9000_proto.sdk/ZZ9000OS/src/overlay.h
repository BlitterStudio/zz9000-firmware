/*
 * MNT ZZ9000 Amiga Graphics and ARM Coprocessor Card Operating System
 * (ZZ9000OS)
 *
 * P96 video window (PIP) overlay: shadow-scanout compositor state.
 * The ZZ9000.card driver configures the overlay through OP_VIDEO_OVERLAY;
 * while it is active the vblank ISR presents a shadow buffer that core 1
 * re-composites every frame (screen copy + color-keyed scaled YUV).
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ_OVERLAY_H
#define ZZ_OVERLAY_H

#include <stdint.h>
#include "video.h"
#include "gfx.h"

/* Core-1 compose job parameters (fits TASKQ_OP_PARAM_BYTES = 48). */
struct overlay_compose_params {
	uint32_t dst_addr;      /* shadow buffer (ARM address) */
	uint32_t dst_pitch;
	uint32_t screen_addr;   /* live framebuffer incl. pan offset */
	uint32_t screen_pitch;
	uint32_t src_addr;      /* YUV source bitmap (ARM address) */
	uint32_t key_native;    /* screen-format key, in-memory form */
	uint16_t scr_w, scr_h;
	uint16_t src_pitch;
	uint16_t src_w, src_h;
	int16_t dst_x, dst_y, dst_w, dst_h;
	uint8_t color_format;   /* MNTVA_* */
	uint8_t variant;        /* enum yuv422_variant */
	uint8_t key_enabled;
	uint8_t target_idx;     /* which shadow this composes into */
};

/* OP_VIDEO_OVERLAY handler (core 0, Zorro op dispatch). GFXData fields
 * must already be byte-swapped by the caller, EXCEPT u32_user[1] (the
 * color key), which follows the pen convention and is read raw. Writes
 * a status into u32_user[0] (0 = OK, 1 = bad params, 2 = shadow
 * allocation failed) and SWAP32s it for the driver. */
void overlay_handle_op(struct ZZ_VIDEO_STATE *vs, struct GFXData *data);

/* Called from the vblank ISR: returns the buffer address to present
 * this frame (a completed shadow while the overlay is presentable, the
 * regular framebuffer + pan offset otherwise) and requests the next
 * compose. ISR context only. */
uint32_t overlay_present_bufpos(struct ZZ_VIDEO_STATE *vs);

/* Called from the core-0 main loop: enqueues the compose task requested
 * by the ISR (respecting the single-producer task queue rule) and
 * services deferred shadow frees. */
void overlay_main_poll(struct ZZ_VIDEO_STATE *vs);

/* Core-1 task body: composite one frame into the target shadow and
 * publish it for the next vblank. */
uint16_t overlay_run_compose(const struct overlay_compose_params *p);

/* Retire hook for the internal compose task (request_id == 0). */
void overlay_compose_retired(void);

/* Amiga reset: the surface heap was reinitialized underneath the
 * shadows - drop all overlay state and the feature gate. */
void overlay_amiga_reset(struct ZZ_VIDEO_STATE *vs);

#endif /* ZZ_OVERLAY_H */
