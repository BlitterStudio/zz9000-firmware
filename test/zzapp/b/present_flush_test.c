/*
 * Present-path steady state: a flip must not remap the surface alias, must
 * clean exactly what the app wrote (the alias range of the presented
 * buffer, surface-sized) and the presented card range, and must not issue
 * any wider flush.  Drives the production display bind/flip/refresh code
 * through the recorded host flush hooks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define ZZAPP_SERVICES_STANDALONE_TEST
#include "zzapp_services.c"

#include <stdio.h>
#include <string.h>

static uint8_t g_window[ZZAPP_WINDOW_BYTES];
static unsigned g_yields;
static int g_fails;

static void fail(const char *what)
{
	fprintf(stderr, "fail: %s\n", what);
	g_fails++;
}
static void ack_pending_flip(uint32_t gen);
/* Counts scheduler round trips, and bounds a runaway wait: if the ack is
 * never seen, force it after a while so the suite reports a failure
 * instead of hanging. */
static void count_yield(void)
{
	if (++g_yields >= 1000U)
		ack_pending_flip(current_runtime()->display_flip_gen);
}

static void bind_triple(uint32_t front, uint32_t back, uint32_t spare)
{
	uint8_t payload[ZZAPP_BIND_EXT_REQ_BYTES];
	uint8_t reply[48];
	uint16_t reply_len = 0U;

	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLAGS_OFF,
		       ZZAPP_F_LAUNCHER_SCANOUT_ACK | ZZAPP_F_SCANOUT_ACK);
	zzapp_build_display_bind_request_triple(payload, 320U, 200U, 640U,
						ZZ_FMT_RGB565, front, back,
						ZZ_LAYOUT_RGB565_LE, spare);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, payload,
				   sizeof(payload), reply, &reply_len, 1, 1U,
				   0U) != SDK_STATUS_OK)
		fail("present flush display bind");
}

static void ack_pending_flip(uint32_t gen)
{
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLIP_ACK_OFF,
		       gen);
}
static void test_steady_flip_flushes_surface_only(void)
{
	const unsigned long bytes = 640U * 200U;
	zzapp_surface surface;
	unsigned grants_before;
	unsigned ungrants_before;
	unsigned flushes_before;
	unsigned flip;

	memset(&surface, 0, sizeof(surface));
	surface.id = 1U;
	grants_before = service_test_grant_count;
	ungrants_before = service_test_ungrant_count;
	flushes_before = zzapp_test_present_flush_count;

	for (flip = 0U; flip < 3U; flip++) {
		uint32_t presented;
		unsigned base;

		ack_pending_flip(current_runtime()->display_flip_gen);
		if (zzapp_svc_display_flip_async(surface.id) != ZZ_OK) {
			fail("steady flip");
			return;
		}
		presented = current_runtime()->display_present;
		/* No remap and no grant churn across a steady-state flip:
		 * the surface stays mapped from bind to close. */
		if (service_test_grant_count != grants_before ||
		    service_test_ungrant_count != ungrants_before) {
			fail("flip remapped the surface alias");
			return;
		}
		base = zzapp_test_present_flush_count;
		if (base - flushes_before != flip + 1U) {
			fprintf(stderr, "flip %u flushed %u time(s)\n", flip,
				base - flushes_before);
			fail("unexpected flush count for flip");
			return;
		}
		/* Exactly one flush: the presented card range, bounded by
		 * the surface size.  The L1 data cache is PIPT, so this one
		 * clean by the card VA also covers the app's alias writes. */
		if (zzapp_test_present_flushes[base - 1U].card != presented ||
		    zzapp_test_present_flushes[base - 1U].bytes != bytes) {
			fprintf(stderr, "flip %u card=%lx/%luB want %lx/%luB\n",
				flip,
				zzapp_test_present_flushes[base - 1U].card,
				zzapp_test_present_flushes[base - 1U].bytes,
				(unsigned long)presented, bytes);
			fail("flip flush range");
		}
		if (zzapp_svc_display_refresh(&surface) != ZZ_OK)
			fail("steady refresh");
	}
}

static void test_idle_wait_completes_without_round_trip(void)
{
	ack_pending_flip(current_runtime()->display_flip_gen);
	g_yields = 0U;
	zzapp_services_test_set_yield(count_yield);
	/* The steady-state present loop's flipwait: the previous flip is
	 * already acknowledged, so the wait must complete from the poll
	 * without parking the app behind a scheduler round trip. */
	if (zzapp_display_wait_flip_idle() != ZZ_OK)
		fail("idle flip wait result");
	if (g_yields != 0U)
		fail("idle flip wait parked the app");
	zzapp_services_test_set_yield(NULL);
}

int main(void)
{
	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	bind_triple(0x00200000U, 0x00210000U, 0x00220000U);
	test_steady_flip_flushes_surface_only();
	test_idle_wait_completes_without_round_trip();
	zzapp_services_test_reset();
	if (g_fails) {
		printf("present_flush_test: %d failure(s)\n", g_fails);
		return 1;
	}
	printf("present_flush_test: ok\n");
	return 0;
}
