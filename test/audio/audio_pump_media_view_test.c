#include <assert.h>
#include <stdint.h>

#include "audio_pump_media_view.h"

int main(void)
{
	struct audio_pump_media_view view;

	/* Admission: 44.1 kHz stereo MP2 (the ZZPlay case, drivers#83). */
	assert(audio_pump_media_view_begin(&view, 44100U, 2U));
	assert(view.active);
	assert(view.src_period_bytes == 3528U); /* 882 frames * 2ch * 2B */
	assert(view.src_rate == 44100U);

	/* 48 kHz stays on the memcpy-only direct path: no view. */
	assert(!audio_pump_media_view_begin(&view, 48000U, 2U));
	/* Rates without a whole-frame 20 ms period are refused. */
	assert(!audio_pump_media_view_begin(&view, 44101U, 2U));
	assert(!audio_pump_media_view_begin(&view, 0U, 2U));
	assert(!audio_pump_media_view_begin(&view, 44100U, 0U));
	assert(!audio_pump_media_view_begin(&view, 44100U, 3U));
	/* A period longer than the converted 960 frames cannot map. */
	assert(!audio_pump_media_view_begin(&view, 50000U, 2U));

	/* Mono sources map their own (narrower) period. */
	assert(audio_pump_media_view_begin(&view, 44100U, 1U));
	assert(view.src_period_bytes == 1764U);

	/* Retirement is exact whole-period math and never loses frames:
	 * one view period (3840 B) retires exactly one source period. */
	assert(audio_pump_media_view_begin(&view, 44100U, 2U));
	audio_pump_media_view_note_staged(&view, 3U * 3528U);
	assert(audio_pump_media_view_retire(&view, 3840U) == 3528U);
	assert(audio_pump_media_view_retire(&view, 2U * 3840U) == 2U * 3528U);
	assert(view.src_retired == 3U * 3528U);

	/* Non-period deltas (caller bugs) map to zero and change nothing. */
	assert(audio_pump_media_view_retire(&view, 1U) == 0U);
	assert(audio_pump_media_view_retire(&view, 0U) == 0U);
	assert(view.src_retired == 3U * 3528U);

	/* An inactive view retires nothing. */
	audio_pump_media_view_reset(&view);
	assert(!view.active);
	assert(audio_pump_media_view_retire(&view, 3840U) == 0U);
	assert(view.src_retired == 0U);


	/* The final converted period can be zero-padded: retirement is
	 * clamped to what the converter really consumed, so the EOS tail
	 * retires exactly and drain completes instead of wedging the
	 * session a period short. */
	assert(audio_pump_media_view_begin(&view, 44100U, 2U));
	audio_pump_media_view_note_staged(&view, 2U * 3528U + 1000U);
	assert(audio_pump_media_view_retire(&view, 3840U) == 3528U);
	assert(audio_pump_media_view_retire(&view, 2U * 3840U) ==
	       3528U + 1000U);
	assert(view.src_retired == 2U * 3528U + 1000U);
	/* Nothing staged beyond retirement: further retires map to zero. */
	assert(audio_pump_media_view_retire(&view, 3840U) == 0U);
	/* Staged notes only move forward. */
	audio_pump_media_view_note_staged(&view, 5U);
	assert(view.src_staged == 2U * 3528U + 1000U);
	/* A drain-shaped burst retires the same total either way: three
	 * single periods and one triple must agree (exactness, not drift). */
	assert(audio_pump_media_view_begin(&view, 44100U, 2U));
	audio_pump_media_view_note_staged(&view, 3U * 3528U);
	(void)audio_pump_media_view_retire(&view, 3840U);
	(void)audio_pump_media_view_retire(&view, 3840U);
	(void)audio_pump_media_view_retire(&view, 3840U);
	assert(view.src_retired == 3U * 3528U);

	return 0;
}
