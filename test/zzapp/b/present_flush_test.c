/*
 * Present-path steady state: a flip must not remap the surface alias, must
 * clean exactly the presented card range (surface-sized), and must not issue
 * any wider flush. The L1 data cache is PIPT, so this clean also covers app
 * alias writes. Drives the production display open/flip/refresh code through
 * the recorded host flush hooks, with the Amiga as mock_client: the open
 * completes with the client's buffers, the flip completes SUBMITTED, and the
 * card's vertical blank acknowledges it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define ZZAPP_SERVICES_STANDALONE_TEST
#define ZZAPP_PROTO_NO_WINDOW
#include "zzapp_services.c"
#include "svc_env.h"

#include <stdio.h>
#include <string.h>

#define FRONT 0x00200000U
#define BACK  0x00210000U
#define SPARE 0x00220000U

static unsigned g_yields;
static int g_fails;

static void fail(const char *what)
{
	fprintf(stderr, "fail: %s\n", what);
	g_fails++;
}

/* Counts scheduler round trips, and bounds a runaway wait: if the ack is
 * never seen, force it after a while so the suite reports a failure
 * instead of hanging. */
static void count_yield(void)
{
	svc_yield();
	if (++g_yields >= 1000U)
		zzapp_services_display_vblank(current_runtime()->display_present);
}

static void open_display(void)
{
	aar_surface surface;

	svc_apps_reset();
	svc_app_claim(0U);
	give_space(0U);
	if (svc_load_begin(0U, ZZAPP_CLIENT_SCANOUT_ACK, NULL, NULL) != SDK_STATUS_OK)
		fail("present flush load begin");
	zzapp_services_test_set_core(1);
	zzapp_services_test_set_yield(svc_yield);
	CL(0).display_front = SVC_TOKEN(FRONT);
	CL(0).display_back = SVC_TOKEN(BACK);
	CL(0).display_spare = SVC_TOKEN(SPARE);
	CL(0).flip_flags = ZZAPP_FLIPF_SUBMITTED;
	memset(&surface, 0, sizeof(surface));
	if (zzapp_svc_display_open(320U, 200U, AAR_FMT_RGB565, 0U, &surface) != AAR_OK)
		fail("present flush display open");
}

static void test_steady_flip_flushes_surface_only(void)
{
	const unsigned long bytes = 640U * 200U;
	aar_surface surface;
	unsigned aliases_before;
	unsigned flushes_before;
	unsigned flip;

	memset(&surface, 0, sizeof(surface));
	surface.id = 1U;
	aliases_before = service_proc()->as.alias_count;
	flushes_before = zzapp_test_present_flush_count;

	for (flip = 0U; flip < 3U; flip++) {
		uint32_t presented;
		unsigned base;

		if (zzapp_svc_display_flip_async(surface.id) != AAR_OK) {
			fail("steady flip");
			return;
		}
		presented = current_runtime()->display_present;
		/* No remap and no grant churn across a steady-state flip:
		 * the surface stays mapped from open to close. */
		if (service_proc()->as.alias_count != aliases_before) {
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
		/* The client submits the flip; the card shows it. */
		svc_yield();
		if (mock_client_count(&CL(0), AAR_OP_DISPLAY_FLIP) != flip + 1U)
			fail("one flip request per flip");
		zzapp_services_display_vblank(presented);
		if (zzapp_svc_display_refresh(&surface) != AAR_OK)
			fail("steady refresh");
	}
}

static void test_idle_wait_completes_without_round_trip(void)
{
	g_yields = 0U;
	zzapp_services_test_set_yield(count_yield);
	/* The steady-state present loop's flipwait: the previous flip is
	 * already acknowledged, so the wait must complete from the poll
	 * without parking the app behind a scheduler round trip. */
	if (zzapp_display_wait_flip_idle() != AAR_OK)
		fail("idle flip wait result");
	if (g_yields != 0U)
		fail("idle flip wait parked the app");
	zzapp_services_test_set_yield(NULL);
}

int main(void)
{
	svc_env_init(8U, 0U, NULL);
	open_display();
	test_steady_flip_flushes_surface_only();
	test_idle_wait_completes_without_round_trip();
	svc_apps_reset();
	if (g_fails) {
		printf("present_flush_test: %d failure(s)\n", g_fails);
		return 1;
	}
	printf("present_flush_test: ok\n");
	return 0;
}
