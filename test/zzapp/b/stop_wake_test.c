/*
 * A stop request ends every parked call the spec lists as returning on stop
 * (aar-api 3.7: files, console, display, audio requests, aar_msg_wait; sleep is
 * the system pass's own, a/sleep_stop_test.c). The AARStop only latches the
 * stop on the record; nothing wakes the waiter except the core-1 service
 * pass, so the pass must treat a stop as a reason to wake any wait whose
 * loop honours it. A wait that does not look at a stop (core 0's audio
 * handoff, which is bounded) is left parked, or it would spin.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define ZZAPP_SERVICES_STANDALONE_TEST
#include "zzapp_services.c"
#include "svc_env.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

static uint64_t now_value;
static unsigned yields;
static void (*on_yield)(unsigned n);

static uint64_t test_time(void)
{
	return now_value;
}

static void test_yield(void)
{
	yields++;
	if (on_yield)
		on_yield(yields);
}

static void reset(void)
{
	svc_apps_reset();
	memset(&service_test_kernel, 0, sizeof(service_test_kernel));
	svc_app_new(0U);
	zzapp_services_test_set_core(1);
	zzapp_services_test_set_time(test_time);
	zzapp_services_test_set_yield(test_yield);
	now_value = 1000U;
	yields = 0U;
	on_yield = NULL;
	test_force_state(0U, AAR_STATE_RUNNING);
}

static void send_app_stop(void)
{
	CHECK(mock_client_stop(&CL(0)) == SDK_STATUS_OK);
}

/* A wait of `kind` is registered and the app is parked in it, as the real
 * wait paths leave it before they yield. */
static void park(enum zzapp_wait_kind kind, uint32_t value)
{
	wait_begin(kind, value, 0U);
	TH(0).state = ZZAPP_THREAD_BLOCKED;
	service_test_wake_count = 0U;
}

static void test_stop_wakes_each_cancellable_wait(void)
{
	static const struct {
		enum zzapp_wait_kind kind;
		const char *name;
	} kinds[] = {
		{ ZZAPP_WAIT_FILE, "file" },
		{ ZZAPP_WAIT_CONSOLE_READ, "console read" },
		{ ZZAPP_WAIT_DISPLAY_OPEN, "display open" },
		{ ZZAPP_WAIT_AUDIO_REQ, "audio request" },
		{ ZZAPP_WAIT_FLIP, "display flip" },
		{ ZZAPP_WAIT_EVENT, "message" },
	};
	size_t i;

	for (i = 0U; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		reset();
		/* A completion id and a flip generation nothing has reached. */
		park(kinds[i].kind, 0x7777U);
		/* No stop: the pass leaves the app parked. */
		zzapp_services_poll();
		if (service_test_wake_count != 0U)
			printf("woken without stop: %s\n", kinds[i].name);
		CHECK(service_test_wake_count == 0U);
		CHECK(TH(0).state == ZZAPP_THREAD_BLOCKED);
		send_app_stop();
		zzapp_services_poll();
		if (service_test_wake_count != 1U)
			printf("stop did not wake: %s\n", kinds[i].name);
		CHECK(service_test_wake_count == 1U);
		CHECK(TH(0).state == ZZAPP_THREAD_RUNNABLE);
		wait_end();
	}

	/* Console output parked on a full ring waits for the tail to move. */
	reset();
	park(ZZAPP_WAIT_CONSOLE_WRITE, RT(0).console_tail);
	zzapp_services_poll();
	CHECK(service_test_wake_count == 0U);
	send_app_stop();
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U);
	wait_end();

	/* The client's own stop word, with no STOP op, counts the same. */
	reset();
	park(ZZAPP_WAIT_FILE, 0x7777U);
	zzapp_services_poll();
	CHECK(service_test_wake_count == 0U);
	mock_client_request_stop(&CL(0));
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U);
	wait_end();
}

/* Core 0's audio handoff does not look at a stop: waking it would only spin
 * until core 0 is done. */
static void test_audio_handoff_is_not_woken_by_stop(void)
{
	reset();
	RT(0).audio_request = AUDIO_REQUEST_OPEN;
	park(ZZAPP_WAIT_AUDIO, 0U);
	send_app_stop();
	zzapp_services_poll();
	CHECK(service_test_wake_count == 0U);
	CHECK(TH(0).state == ZZAPP_THREAD_BLOCKED);
	RT(0).audio_request = AUDIO_REQUEST_NONE;
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U);
	wait_end();
}

static void stop_when_parked(unsigned n)
{
	(void)n;
	CHECK(zzapp_services_wait_registered(PID_OF(0U)));
	TH(0).state = ZZAPP_THREAD_BLOCKED;
	service_test_wake_count = 0U;
	zzapp_services_poll();
	CHECK(service_test_wake_count == 0U);
	send_app_stop();
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U);
	CHECK(TH(0).state == ZZAPP_THREAD_RUNNABLE);
}

/* The calls themselves: the client never answers, the stop arrives while the
 * app is parked, and the call returns AAR_E_IO after one wake. */
static void test_calls_return_on_stop(void)
{
	uint8_t buf[16];
	uint32_t n = 0U;

	reset();
	on_yield = stop_when_parked;
	CHECK(zzapp_svc_console_read(buf, sizeof(buf), &n) == AAR_E_IO);
	CHECK(yields == 1U);
	CHECK(n == 0U);
	CHECK(!zzapp_services_wait_registered(PID_OF(0U)));

	reset();
	on_yield = stop_when_parked;
	CHECK(zzapp_svc_file_open("RAM:never-answered", AAR_FILE_READ) == AAR_E_IO);
	CHECK(yields == 1U);
	CHECK(!zzapp_services_wait_registered(PID_OF(0U)));
}

static void stop_word_when_parked(unsigned n)
{
	(void)n;
	TH(0).state = ZZAPP_THREAD_BLOCKED;
	mock_client_request_stop(&CL(0));
	zzapp_services_poll();
}

/* The stop word alone (no STOP op, no tick): the call that ended on it has
 * latched the stop on the record, which is what aar_should_stop() reads. */
static void test_wait_ended_by_stop_word_latches(void)
{
	uint8_t buf[16];
	uint32_t n = 0U;

	reset();
	CHECK(__atomic_load_n(&APP(0).rec->host.stop_requested,
			      __ATOMIC_ACQUIRE) == 0U);
	on_yield = stop_word_when_parked;
	CHECK(zzapp_svc_console_read(buf, sizeof(buf), &n) == AAR_E_IO);
	CHECK(__atomic_load_n(&APP(0).rec->host.stop_requested,
			      __ATOMIC_ACQUIRE) != 0U);
}

/* Spec 5.1: RUNNING becomes STOPPING on the stop request, before the
 * teardown ends it. */
static void test_stop_publishes_stopping(void)
{
	reset();
	CHECK(CBSTATE(0) == AAR_STATE_RUNNING);
	send_app_stop();
	CHECK(CBSTATE(0) == AAR_STATE_STOPPING);
	CHECK(APP(0).rec->claim.state == ZZAPP_ST_STOPPING);
	CHECK(__atomic_load_n(&APP(0).rec->host.stop_requested,
			      __ATOMIC_ACQUIRE) != 0U);
	/* A second stop leaves it STOPPING. */
	send_app_stop();
	CHECK(CBSTATE(0) == AAR_STATE_STOPPING);
}

int main(void)
{
	svc_env_init(8U, 0U, NULL);
	test_stop_wakes_each_cancellable_wait();
	test_audio_handoff_is_not_woken_by_stop();
	test_calls_return_on_stop();
	test_wait_ended_by_stop_word_latches();
	test_stop_publishes_stopping();
	printf("stop_wake_test: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
