/*
 * Overlay composition scheduling policy shared by firmware and host tests.
 *
 * Legacy P96 video sources can change without notifying the firmware, so they
 * remain refresh-driven. SDK video sessions provide an exact frame-ready
 * completion; once one feeds the active overlay, composition can instead run
 * only for new source frames and explicit overlay SET operations.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ_OVERLAY_SCHEDULE_H
#define ZZ_OVERLAY_SCHEDULE_H

#include <stdint.h>

#define OVERLAY_VIDEO_SESSION_SLOTS 1U

struct overlay_schedule_state {
	uint32_t video_session[OVERLAY_VIDEO_SESSION_SLOTS];
};

/* The PL VDMA retains the source bitmap's P96 stride when an SDK planar frame
 * is packed into a shadow buffer.  Staging must use that exact stride too;
 * compacting rows to their visible byte count makes every line after the
 * first start at a different address from the one fetched by the VDMA. */
static inline uint32_t overlay_staging_pitch(uint16_t width,
	uint32_t source_pitch)
{
	uint32_t visible = ((uint32_t)width + 1U) / 2U * 4U;

	return source_pitch >= visible ? source_pitch : visible;
}

static inline uint32_t overlay_buffer_bytes(uint32_t screen_pitch,
	uint32_t screen_rows, uint32_t source_pitch, uint32_t source_rows)
{
	uint32_t screen_bytes = screen_pitch * screen_rows;
	uint32_t source_bytes = source_pitch * source_rows;

	return screen_bytes >= source_bytes ? screen_bytes : source_bytes;
}

/* Scanout must restart at vblank entry, before the full cache flush. A buffer
 * composed during the previous frame takes precedence once that flush has
 * made it safe; consuming the handoff unblocks composition into the old
 * scan buffer. */
static inline uint32_t overlay_vblank_take_rearm(
	uint32_t scan_address, uint32_t *handoff_address)
{
	uint32_t address;

	if (!handoff_address)
		return scan_address;
	address = *handoff_address != 0U
		? *handoff_address : scan_address;
	*handoff_address = 0U;
	return address;
}

static inline void overlay_schedule_reset(struct overlay_schedule_state *state)
{
	uint32_t i;

	for (i = 0U; i < OVERLAY_VIDEO_SESSION_SLOTS; i++)
		state->video_session[i] = 0U;
}

static inline void overlay_schedule_frame_ready(
	struct overlay_schedule_state *state, uint32_t session)
{
	uint32_t i;

	if (session == 0U)
		return;
	for (i = 0U; i < OVERLAY_VIDEO_SESSION_SLOTS; i++) {
		if (state->video_session[i] == session)
			return;
	}
	for (i = 0U; i < OVERLAY_VIDEO_SESSION_SLOTS; i++) {
		if (state->video_session[i] == 0U) {
			state->video_session[i] = session;
			return;
		}
	}
}

static inline void overlay_schedule_session_closed(
	struct overlay_schedule_state *state, uint32_t session)
{
	uint32_t i;

	for (i = 0U; i < OVERLAY_VIDEO_SESSION_SLOTS; i++) {
		if (state->video_session[i] == session)
			state->video_session[i] = 0U;
	}
}

static inline int overlay_schedule_refresh_driven(
	const struct overlay_schedule_state *state)
{
	uint32_t i;

	for (i = 0U; i < OVERLAY_VIDEO_SESSION_SLOTS; i++) {
		if (state->video_session[i] != 0U)
			return 0;
	}
	return 1;
}

#endif /* ZZ_OVERLAY_SCHEDULE_H */
