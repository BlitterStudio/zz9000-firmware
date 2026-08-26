#include <assert.h>
#include <stdint.h>

#include "overlay_schedule.h"

int main(void)
{
	struct overlay_schedule_state state;

	assert(overlay_staging_pitch(640U, 1280U) == 1280U);
	assert(overlay_staging_pitch(641U, 1344U) == 1344U);
	assert(overlay_staging_pitch(641U, 0U) == 1284U);
	assert(overlay_buffer_bytes(1280U, 480U, 1344U, 480U) ==
	       1344U * 480U);
	assert(overlay_buffer_bytes(5120U, 720U, 1280U, 480U) ==
	       5120U * 720U);

	{
		uint32_t handoff = 0U;

		assert(overlay_vblank_take_rearm(0x1000U, &handoff) ==
		       0x1000U);
		assert(handoff == 0U);
		handoff = 0x2000U;
		assert(overlay_vblank_take_rearm(0x1000U, &handoff) ==
		       0x2000U);
		assert(handoff == 0U);
	}
	overlay_schedule_reset(&state);
	assert(overlay_schedule_refresh_driven(&state));

	overlay_schedule_frame_ready(&state, 0x101U);
	assert(!overlay_schedule_refresh_driven(&state));
	/* Repeated frames from the active direct-overlay session are idempotent. */
	overlay_schedule_frame_ready(&state, 0x101U);
	overlay_schedule_session_closed(&state, 0x101U);
	assert(overlay_schedule_refresh_driven(&state));

	/* Invalid/no-session notifications are harmless. */
	overlay_schedule_frame_ready(&state, 0U);
	overlay_schedule_session_closed(&state, 0U);
	assert(overlay_schedule_refresh_driven(&state));

	return 0;
}
