/*
 * MNT ZZ9000 Amiga Graphics and ARM Coprocessor Card Operating System
 * (ZZ9000OS)
 *
 * Overlay presentation-path snapshot, split out of overlay.h so the media
 * host tests can use it. overlay.h itself pulls in video.h/gfx.h and the
 * Xilinx BSP, which host builds cannot provide; this header needs only
 * <stdint.h>.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ_OVERLAY_PATH_H
#define ZZ_OVERLAY_PATH_H

#include <stdint.h>

/* Live presentation-path snapshot for the media STATUS presentation page.
 * The overlay decides natively-scanned versus shadow-composited internally,
 * so a player has no other way to report which path actually presented its
 * frames. Facts only: the classification into 1:1 / scaled / clipped is
 * arithmetic on this geometry and is done host-side. */
struct overlay_path_info {
	uint8_t configured;     /* a SET has validated geometry */
	uint8_t active;         /* overlay enabled and not mode-stale */
	uint8_t hw_active;      /* native PL plane drives scanout */
	uint8_t owns_session;   /* the queried session feeds this overlay */
	uint16_t src_w, src_h;
	int16_t dst_x, dst_y, dst_w, dst_h;
	uint16_t screen_w, screen_h;
};

/* Fills `out` with the current overlay path state. `session` is the media
 * session asking; `owns_session` reports whether it is the one bound. Safe to
 * call from the core-0 op dispatch. */
void overlay_path_snapshot(uint32_t session, struct overlay_path_info *out);

#endif /* ZZ_OVERLAY_PATH_H */
