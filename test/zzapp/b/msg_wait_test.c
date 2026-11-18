/*
 * aar_msg_wait: park until the Amiga queues a host-to-app message. The Amiga
 * writes the ring in the launcher window with no doorbell, so the wait is a
 * level test the core-1 service pass repeats; a message that lands between
 * the ring check and the park is seen by that pass.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define ZZAPP_SERVICES_STANDALONE_TEST
#include "zzapp_services.c"

#include <stdio.h>
#include <string.h>

static uint8_t window[ZZAPP_WINDOW_BYTES];
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
	memset(window, 0, sizeof(window));
	zzapp_services_test_reset();
	memset(&service_test_kernel, 0, sizeof(service_test_kernel));
	zzapp_services_test_set_window(window, sizeof(window));
	zzapp_services_set_generation(1U);
	zzapp_services_test_set_time(test_time);
	zzapp_services_test_set_yield(test_yield);
	now_value = 1000U;
	yields = 0U;
	on_yield = NULL;
}

/* What zz9k_app_ring_push does from the 68k. */
static void host_push(uint8_t fill)
{
	uint32_t head = zzapp_get_be32(window + ZZAPP_MSG_H2A_OFF);

	memset(window + ZZAPP_MSG_H2A_OFF + ZZAPP_MSG_HDR +
	       (head & (ZZAPP_MSG_CAP - 1U)) * ZZAPP_MSG_BYTES_PER, fill,
	       ZZAPP_MSG_BYTES_PER);
	zzapp_put_be32(window + ZZAPP_MSG_H2A_OFF, head + 1U);
}

static uint32_t list_block_reason(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len = 0U;

	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LIST, request, sizeof(request), reply,
				   &reply_len, 1, 1U, 0U) != SDK_STATUS_OK)
		return 0xffU;
	return reply[ZZAPP_LIST_REASONS];
}

static void test_ready_message_returns_at_once(void)
{
	uint8_t msg[ZZAPP_MSG_BYTES_PER];

	reset();
	host_push(0x5a);
	CHECK(zzapp_svc_msg_wait(1000000U) == AAR_OK);
	CHECK(yields == 0U);
	CHECK(!zzapp_services_wait_registered(0U));
	/* The wait only reports readiness; the message is still queued. */
	CHECK(zzapp_svc_msg_poll(msg) == 1);
	CHECK(msg[0] == 0x5a && msg[31] == 0x5a);
	CHECK(zzapp_svc_msg_poll(msg) == 0);
}

static void arrive_later(unsigned n)
{
	/* Parked: registered as an EVENT wait with the deadline recorded. */
	CHECK(zzapp_services_wait_registered(0U));
	CHECK(runtime[0].wait_kind == ZZAPP_WAIT_EVENT);
	CHECK(runtime[0].wait_deadline == 1000U + 5000000U);
	service_test_kernel.threads[0].state = ZZAPP_THREAD_BLOCKED;
	CHECK(list_block_reason() == ZZAPP_BLOCK_EVENT);
	if (n == 1U) {
		/* Nothing queued: the service pass leaves the app parked. */
		service_test_wake_count = 0U;
		zzapp_services_poll();
		CHECK(service_test_wake_count == 0U);
		CHECK(service_test_kernel.threads[0].state ==
		      ZZAPP_THREAD_BLOCKED);
		return;
	}
	host_push(0x11);
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U);
	CHECK(service_test_kernel.threads[0].state == ZZAPP_THREAD_RUNNABLE);
}

static void test_message_that_arrives_later_wakes(void)
{
	uint8_t msg[ZZAPP_MSG_BYTES_PER];

	reset();
	on_yield = arrive_later;
	CHECK(zzapp_svc_msg_wait(5000000U) == AAR_OK);
	CHECK(yields == 2U);
	CHECK(!zzapp_services_wait_registered(0U));
	CHECK(zzapp_svc_msg_poll(msg) == 1);
	CHECK(msg[5] == 0x11);
}

static void arrive_between_check_and_park(unsigned n)
{
	(void)n;
	/* The ring was empty at the check; the Amiga pushes before the app is
	 * actually blocked and no service pass runs. Whatever resumes the app
	 * next finds the message because the wait re-reads the ring. */
	host_push(0x22);
}

static void test_message_between_check_and_park_is_not_missed(void)
{
	reset();
	on_yield = arrive_between_check_and_park;
	CHECK(zzapp_svc_msg_wait(5000000U) == AAR_OK);
	CHECK(yields == 1U);

	/* A service pass that sees the ring ready wakes a parked waiter even
	 * though the message arrived before the pass ever saw the wait empty. */
	reset();
	runtime[0].wait_kind = ZZAPP_WAIT_EVENT;
	runtime[0].wait_deadline = 1000U + 5000000U;
	service_test_kernel.threads[0].state = ZZAPP_THREAD_BLOCKED;
	host_push(0x33);
	service_test_wake_count = 0U;
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U);
	CHECK(service_test_kernel.threads[0].state == ZZAPP_THREAD_RUNNABLE);
}

static void run_to_deadline(unsigned n)
{
	(void)n;
	CHECK(runtime[0].wait_kind == ZZAPP_WAIT_EVENT);
	service_test_kernel.threads[0].state = ZZAPP_THREAD_BLOCKED;
	/* Time passes with nothing queued; the pass wakes the waiter TIMEOUT. */
	now_value = runtime[0].wait_deadline;
	zzapp_services_poll();
	CHECK(service_test_kernel.threads[0].wake_reason == ZZAPP_WAKE_TIMEOUT);
}

static void test_timeout(void)
{
	reset();
	/* Zero only tests the ring. */
	CHECK(zzapp_svc_msg_wait(0U) == AAR_E_TIMEDOUT);
	CHECK(yields == 0U);
	CHECK(!zzapp_services_wait_registered(0U));

	reset();
	on_yield = run_to_deadline;
	CHECK(zzapp_svc_msg_wait(250000U) == AAR_E_TIMEDOUT);
	CHECK(yields == 1U);
	CHECK(!zzapp_services_wait_registered(0U));
	CHECK(runtime[0].wait_deadline == 0U);
}

/* The Amiga's APP_STOP, as the mailbox dispatches it. It only latches the
 * stop word; nothing wakes the waiter except the service pass. */
static void send_app_stop(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len = 0U;

	CHECK(zzapp_mailbox_dispatch(ZZAPP_OP_STOP, NULL, 0U, reply, &reply_len,
				     1, 1U, 0U) == SDK_STATUS_OK);
}

static int stop_with_message;

static void stop_while_parked(unsigned n)
{
	(void)n;
	CHECK(runtime[0].wait_kind == ZZAPP_WAIT_EVENT);
	service_test_kernel.threads[0].state = ZZAPP_THREAD_BLOCKED;
	/* No stop yet: the pass leaves the app parked. */
	service_test_wake_count = 0U;
	zzapp_services_poll();
	CHECK(service_test_wake_count == 0U);
	if (stop_with_message)
		host_push(0x55);
	send_app_stop();
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U);
	CHECK(service_test_kernel.threads[0].state == ZZAPP_THREAD_RUNNABLE);
}

/* The message lands in the same pass that sees the deadline. The pass picks
 * READY, not TIMEOUT. */
static void message_at_deadline(unsigned n)
{
	(void)n;
	service_test_kernel.threads[0].state = ZZAPP_THREAD_BLOCKED;
	now_value = runtime[0].wait_deadline;
	host_push(0x66);
	zzapp_services_poll();
	CHECK(service_test_kernel.threads[0].wake_reason == ZZAPP_WAKE_READY);
}

/* The pass saw the deadline first and woke the app TIMEOUT; the message
 * lands before the app runs again. */
static void message_after_timeout_wake(unsigned n)
{
	(void)n;
	service_test_kernel.threads[0].state = ZZAPP_THREAD_BLOCKED;
	now_value = runtime[0].wait_deadline;
	zzapp_services_poll();
	CHECK(service_test_kernel.threads[0].wake_reason == ZZAPP_WAKE_TIMEOUT);
	host_push(0x77);
}

static void test_message_during_timeout_is_not_lost(void)
{
	uint8_t msg[ZZAPP_MSG_BYTES_PER];

	reset();
	on_yield = message_at_deadline;
	CHECK(zzapp_svc_msg_wait(250000U) == AAR_OK);
	CHECK(zzapp_svc_msg_poll(msg) == 1 && msg[0] == 0x66);

	reset();
	on_yield = message_after_timeout_wake;
	CHECK(zzapp_svc_msg_wait(250000U) == AAR_OK);
	CHECK(zzapp_svc_msg_poll(msg) == 1 && msg[0] == 0x77);
	CHECK(!zzapp_services_wait_registered(0U));
}

static void test_stop_cancels_the_wait(void)
{
	reset();
	slots[0].state = ZZAPP_ST_RUNNING;
	stop_with_message = 0;
	on_yield = stop_while_parked;
	CHECK(zzapp_svc_msg_wait(5000000U) == AAR_E_IO);
	CHECK(yields == 1U);
	CHECK(!zzapp_services_wait_registered(0U));

	/* A message queued with the stop is still returned. */
	reset();
	slots[0].state = ZZAPP_ST_RUNNING;
	stop_with_message = 1;
	on_yield = stop_while_parked;
	CHECK(zzapp_svc_msg_wait(5000000U) == AAR_OK);

	/* Already stopping: no park at all. */
	reset();
	zzapp_put_be32(window + ZZAPP_LIFE_STOP_OFF, 1U);
	CHECK(zzapp_svc_msg_wait(5000000U) == AAR_E_IO);
	CHECK(yields == 0U);

	/* A queued message still wins over a stop, so none is dropped. */
	host_push(0x44);
	CHECK(zzapp_svc_msg_wait(5000000U) == AAR_OK);
}

static void test_ring_corruption_is_an_error(void)
{
	reset();
	zzapp_put_be32(window + ZZAPP_MSG_H2A_OFF, ZZAPP_MSG_CAP + 5U);
	CHECK(zzapp_svc_msg_wait(5000000U) == AAR_E_IO);
	CHECK(yields == 0U);
	CHECK(!zzapp_services_wait_registered(0U));
}

int main(void)
{
	test_ready_message_returns_at_once();
	test_message_that_arrives_later_wakes();
	test_message_between_check_and_park_is_not_missed();
	test_timeout();
	test_stop_cancels_the_wait();
	test_message_during_timeout_is_not_lost();
	test_ring_corruption_is_an_error();
	printf("msg_wait_test: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
