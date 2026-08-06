/*
 * Per-stage media pipeline timing. See sdk_media_profile.h for the
 * concurrency argument.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_media_profile.h"

#include <string.h>

#ifndef SDK_MEDIA_HOST_TEST
#include "memorymap.h"
#include "xtime_l.h"
#endif

#ifdef SDK_MEDIA_HOST_TEST
static struct SDKMediaProfile host_profile;
static struct SDKMediaProfile *const profile_state = &host_profile;
/* Host tests drive the clock explicitly so the accumulation logic can be
 * checked without a global timer. */
static uint32_t host_now_us;

void sdk_media_profile_host_set_now(uint32_t now_us)
{
	host_now_us = now_us;
}
#else
static struct SDKMediaProfile *const profile_state =
	(struct SDKMediaProfile *)SDK_MEDIA_PROFILE_ADDRESS;
typedef char media_profile_fit_check[
	(sizeof(struct SDKMediaProfile) <= SDK_MEDIA_PROFILE_MAX_BYTES)
		? 1 : -1];
#endif

uint32_t sdk_media_profile_now_us(void)
{
#ifdef SDK_MEDIA_HOST_TEST
	return host_now_us;
#else
	XTime now;

	XTime_GetTime(&now);
	/* COUNTS_PER_SECOND is the global timer rate; reduce to microseconds
	 * before truncating so the division does not lose the whole value on a
	 * 32-bit result. */
	return (uint32_t)((now / (COUNTS_PER_SECOND / 1000000U)) & 0xffffffffU);
#endif
}

void sdk_media_profile_record(uint32_t stage, uint32_t start_us)
{
	uint32_t now;
	uint32_t elapsed;

	if (stage >= SDK_MEDIA_PROFILE_STAGES || start_us == 0U)
		return;
	now = sdk_media_profile_now_us();
	/* Unsigned subtraction gives the right answer across a 32-bit wrap of
	 * the timer itself, which happens every ~71 minutes of uptime. */
	elapsed = now - start_us;
	profile_state->stage[stage].microseconds += elapsed;
	profile_state->stage[stage].calls++;
}

void sdk_media_profile_reset(void)
{
	memset((void *)profile_state, 0, sizeof(*profile_state));
}

int sdk_media_profile_read(uint32_t stage, uint32_t *microseconds,
                           uint32_t *calls)
{
	if (stage >= SDK_MEDIA_PROFILE_STAGES)
		return 0;
	if (microseconds)
		*microseconds = profile_state->stage[stage].microseconds;
	if (calls)
		*calls = profile_state->stage[stage].calls;
	return 1;
}
