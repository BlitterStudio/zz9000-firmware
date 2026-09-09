/*
 * Cursor mapping for the media-session preconvert view.
 *
 * The audio fabric compositor runs in the audio formatter period ISR on
 * core 0. For a bound media session whose PCM is not 48 kHz it executed
 * the full polyphase rate conversion inside that ISR; the ISR is long
 * enough that a vblank IRQ arriving during it delayed isr_video past the
 * overlay rearm window, which showed as windowed-PIP flicker
 * (zz9000-drivers#83). The fix stages 48 kHz PCM ahead of the ISR
 * through the audio_pump_preconvert ring (main loop) so the ISR only
 * copies; this header owns the exact cursor mapping between the 48 kHz
 * view the fabric consumes and the session ring the decoder publishes.
 *
 * The mapping is exact by construction: the preconvert ring always holds
 * whole 20 ms periods (3840 bytes), the fabric stages and retires whole
 * view periods, and one view period corresponds to exactly one source
 * period of (rate/50)*channels*2 bytes. Sessions at 48 kHz do not use
 * the view (the direct path already costs a memcpy only).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZ_AUDIO_PUMP_MEDIA_VIEW_H
#define ZZ_AUDIO_PUMP_MEDIA_VIEW_H

#include <stdint.h>

#define AUDIO_PUMP_MEDIA_VIEW_PERIOD_BYTES 3840U
#define AUDIO_PUMP_MEDIA_VIEW_PERIOD_FRAMES \
	(AUDIO_PUMP_MEDIA_VIEW_PERIOD_BYTES / 4U)
struct audio_pump_media_view {
	uint64_t src_retired;      /* session bytes retired (playback clock) */
	uint64_t src_staged;       /* session bytes the converter consumed;
	                             * bounds retirement (the final converted
	                             * period may be zero-padded) */
	uint32_t src_period_bytes; /* source bytes per 20 ms period */
	uint32_t src_rate;
	uint8_t active;
};

static inline void audio_pump_media_view_reset(
	struct audio_pump_media_view *view)
{
	if (!view)
		return;
	view->active = 0U;
	view->src_retired = 0U;
	view->src_staged = 0U;
	view->src_period_bytes = 0U;
	view->src_rate = 0U;
}

/* A view is admissible when one 20 ms source period is a whole number of
 * frames that fits the converted period. 48 kHz needs no conversion, so
 * it stays on the (already memcpy-only) direct path. */
static inline int audio_pump_media_view_begin(
	struct audio_pump_media_view *view, uint32_t sample_rate,
	uint32_t channels)
{
	uint32_t frames;

	if (!view || sample_rate == 0U || sample_rate == 48000U ||
	    channels == 0U || channels > 2U || (sample_rate % 50U) != 0U)
		return 0;
	frames = sample_rate / 50U;
	if (frames == 0U || frames > AUDIO_PUMP_MEDIA_VIEW_PERIOD_FRAMES)
		return 0;
	view->src_period_bytes = frames * channels * 2U;
	view->src_rate = sample_rate;
	view->src_retired = 0U;
	view->src_staged = 0U;
	view->active = 1U;
	return 1;
}

/* The fill publishes how many session bytes it has consumed into the
 * converted ring; retirement may never advance past it. */
static inline void audio_pump_media_view_note_staged(
	struct audio_pump_media_view *view, uint64_t src_staged)
{
	if (view && src_staged >= view->src_staged)
		view->src_staged = src_staged;
}

/* Map whole view periods retired by the fabric compositor onto session
 * bytes and advance the retired cursor. Non-period deltas map to zero:
 * the compositor stages and retires whole periods only, so anything else
 * is a caller bug that must not corrupt the playback clock. The final
 * converted period can be zero-padded (a source tail shorter than one
 * period), so a full-period mapping is clamped to what was really
 * staged -- retirement then lands exactly on the produced cursor and
 * end-of-stream drain completes. */
static inline uint64_t audio_pump_media_view_retire(
	struct audio_pump_media_view *view, uint32_t view_bytes)
{
	uint64_t src;

	if (!view || !view->active || view_bytes == 0U ||
	    view->src_period_bytes == 0U ||
	    (view_bytes % AUDIO_PUMP_MEDIA_VIEW_PERIOD_BYTES) != 0U)
		return 0U;
	src = (uint64_t)(view_bytes / AUDIO_PUMP_MEDIA_VIEW_PERIOD_BYTES) *
	      (uint64_t)view->src_period_bytes;
	if (view->src_staged - view->src_retired < src)
		src = view->src_staged - view->src_retired;
	view->src_retired += src;
	return src;
}

#endif /* ZZ_AUDIO_PUMP_MEDIA_VIEW_H */
