/*
 * MNT ZZ9000 Amiga Graphics and ARM Coprocessor Card Operating System
 * (ZZ9000OS)
 *
 * P96 video window (PIP) overlay: native PL plane plus software fallback.
 * The ZZ9000.card driver configures the overlay through OP_VIDEO_OVERLAY;
 * Fully visible 1:1 sources use packed-YUV scanout-time composition; scaled
 * or clipped sources retain the RGB shadow compositor. Legacy sources refresh
 * every vblank; SDK video sources notify on decoded frames.
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
 * regular framebuffer + pan offset otherwise). Legacy sources request the
 * next compose here; SDK video sources request it from frame completion.
 * ISR context only. */
uint32_t overlay_present_bufpos(struct ZZ_VIDEO_STATE *vs);

/* Called from the core-0 main loop: enqueues the compose task requested
 * by the ISR (respecting the single-producer task queue rule) and
 * services deferred shadow frees. */
void overlay_main_poll(struct ZZ_VIDEO_STATE *vs);

/* Core-1 task body: composite one frame into the target shadow and
 * publish it for the next vblank. */
uint16_t overlay_run_compose(const struct overlay_compose_params *p);

/* Retire hook for the internal compose task (request_id == 0), called
 * on core 0 from the task-queue harvest; publishes the completed
 * shadow when `ok` (task status was SDK_STATUS_OK). */
void overlay_compose_retired(int ok);

/* Amiga reset: the surface heap was reinitialized underneath the
 * shadows - drop all overlay state and the feature gate. */
void overlay_amiga_reset(struct ZZ_VIDEO_STATE *vs);

/* Task-queue quiesce+reinit outside an Amiga reset (fw update): clear
 * the in-flight marker whose completion was dropped with the queue. */
void overlay_scheduler_reset(void);

/* Core-0 SDK-video lifecycle hooks. A frame-ready completion switches that
 * session's active overlay to source-frame-driven composition; close removes
 * the binding and restores refresh-driven behavior after the final session. */
int overlay_video_frame_ready(uint32_t session);
void overlay_video_session_closed(uint32_t session);

/* ISR helpers for the videocap takeover: while videocap owns the
 * scanout decisions the overlay present hook does not run, so the ISR
 * asks whether a shadow is still being scanned (to repoint VDMA away
 * from it once) and then reports the release. */
int overlay_scanout_active(void);
void overlay_scanout_released(void);

/* Vblank ISR hook, called immediately after the mandatory full L1+L2 flush.
 * Publishes a completed packed-YUV staging buffer to the non-coherent VDMA. */
void overlay_vblank_cache_flushed(void);

#endif /* ZZ_OVERLAY_H */
