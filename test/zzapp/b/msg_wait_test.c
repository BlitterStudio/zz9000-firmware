/*
 * aar_msg_wait: park until the Amiga queues a client-to-app message. The
 * Amiga posts the message on the completion queue, within its credit, and
 * rings no doorbell the wait depends on: the wait is a level test the core-1
 * service pass repeats; a message that lands between the check and the park
 * is seen by that pass. A message counts as consumed, and hands its credit
 * back through AAR_CB_MSG_CONSUMED, when the app takes it.
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
}

/* What AARSendMessage does from the 68k, within its credit. */
static void host_push(uint8_t fill)
{
	uint8_t msg[AAR_MSG_BYTES];

	memset(msg, fill, sizeof(msg));
	CHECK(mock_client_post_msg(&CL(0), msg) == AAR_OK);
}

static uint32_t list_block_reason(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	mock_client probe;

	mock_client_init(&probe);
	svc_client_wire(&probe);
	if (mock_client_list(&probe, PID_OF(0U), reply) != SDK_STATUS_OK)
		return 0xffU;
	return reply[ZZAPP_LIST_REASONS];
}

static void test_ready_message_returns_at_once(void)
{
	uint8_t msg[AAR_MSG_BYTES];

	reset();
	host_push(0x5a);
	CHECK(zzapp_svc_msg_wait(1000000U) == AAR_OK);
	CHECK(yields == 0U);
	CHECK(!zzapp_services_wait_registered(PID_OF(0U)));
	/* The wait only reports readiness; the message is still queued and its
	 * credit is still out. */
	CHECK(CB(0, AAR_CB_MSG_CONSUMED) == 0U);
	CHECK(zzapp_svc_msg_poll(msg) == 1);
	CHECK(msg[0] == 0x5a && msg[31] == 0x5a);
	CHECK(CB(0, AAR_CB_MSG_CONSUMED) == 1U);
	CHECK(zzapp_svc_msg_poll(msg) == 0);
	CHECK(CB(0, AAR_CB_MSG_CONSUMED) == 1U);
}

static void arrive_later(unsigned n)
{
	/* Parked: registered as an EVENT wait with the deadline recorded. */
	CHECK(zzapp_services_wait_registered(PID_OF(0U)));
	CHECK(RT(0).wait_kind == ZZAPP_WAIT_EVENT);
	CHECK(RT(0).wait_deadline == 1000U + 5000000U);
	TH(0).state = ZZAPP_THREAD_BLOCKED;
	CHECK(list_block_reason() == ZZAPP_BLOCK_EVENT);
	if (n == 1U) {
		/* Nothing queued: the service pass leaves the app parked. */
		service_test_wake_count = 0U;
		zzapp_services_poll();
		CHECK(service_test_wake_count == 0U);
		CHECK(TH(0).state == ZZAPP_THREAD_BLOCKED);
		return;
	}
	host_push(0x11);
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U);
	CHECK(TH(0).state == ZZAPP_THREAD_RUNNABLE);
}

static void test_message_that_arrives_later_wakes(void)
{
	uint8_t msg[AAR_MSG_BYTES];

	reset();
	on_yield = arrive_later;
	CHECK(zzapp_svc_msg_wait(5000000U) == AAR_OK);
	CHECK(yields == 2U);
	CHECK(!zzapp_services_wait_registered(PID_OF(0U)));
	CHECK(zzapp_svc_msg_poll(msg) == 1);
	CHECK(msg[5] == 0x11);
}

static void arrive_between_check_and_park(unsigned n)
{
	(void)n;
	/* The CQ was empty at the check; the Amiga posts before the app is
	 * actually blocked and no service pass runs. Whatever resumes the app
	 * next finds the message because the wait re-reads the queue. */
	host_push(0x22);
}

static void test_message_between_check_and_park_is_not_missed(void)
{
	reset();
	on_yield = arrive_between_check_and_park;
	CHECK(zzapp_svc_msg_wait(5000000U) == AAR_OK);
	CHECK(yields == 1U);

	/* A service pass that sees the CQ ready wakes a parked waiter even
	 * though the message arrived before the pass ever saw the wait empty. */
	reset();
	RT(0).wait_kind = ZZAPP_WAIT_EVENT;
	RT(0).wait_deadline = 1000U + 5000000U;
	TH(0).state = ZZAPP_THREAD_BLOCKED;
	host_push(0x33);
	service_test_wake_count = 0U;
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U);
	CHECK(TH(0).state == ZZAPP_THREAD_RUNNABLE);
}

static void run_to_deadline(unsigned n)
{
	(void)n;
	CHECK(RT(0).wait_kind == ZZAPP_WAIT_EVENT);
	TH(0).state = ZZAPP_THREAD_BLOCKED;
	/* Time passes with nothing queued; the pass wakes the waiter TIMEOUT. */
	now_value = RT(0).wait_deadline;
	zzapp_services_poll();
	CHECK(TH(0).wake_reason == ZZAPP_WAKE_TIMEOUT);
}

static void test_timeout(void)
{
	reset();
	/* Zero only tests the queue. */
	CHECK(zzapp_svc_msg_wait(0U) == AAR_E_TIMEDOUT);
	CHECK(yields == 0U);
	CHECK(!zzapp_services_wait_registered(PID_OF(0U)));

	reset();
	on_yield = run_to_deadline;
	CHECK(zzapp_svc_msg_wait(250000U) == AAR_E_TIMEDOUT);
	CHECK(yields == 1U);
	CHECK(!zzapp_services_wait_registered(PID_OF(0U)));
	CHECK(RT(0).wait_deadline == 0U);
}

/* The Amiga's STOP, as the mailbox dispatches it. It only latches the stop;
 * nothing wakes the waiter except the service pass. */
static void send_app_stop(void)
{
	CHECK(mock_client_stop(&CL(0)) == SDK_STATUS_OK);
}

static int stop_with_message;

static void stop_while_parked(unsigned n)
{
	(void)n;
	CHECK(RT(0).wait_kind == ZZAPP_WAIT_EVENT);
	TH(0).state = ZZAPP_THREAD_BLOCKED;
	/* No stop yet: the pass leaves the app parked. */
	service_test_wake_count = 0U;
	zzapp_services_poll();
	CHECK(service_test_wake_count == 0U);
	if (stop_with_message)
		host_push(0x55);
	send_app_stop();
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U);
	CHECK(TH(0).state == ZZAPP_THREAD_RUNNABLE);
}

/* The message lands in the same pass that sees the deadline. The pass picks
 * READY, not TIMEOUT. */
static void message_at_deadline(unsigned n)
{
	(void)n;
	TH(0).state = ZZAPP_THREAD_BLOCKED;
	now_value = RT(0).wait_deadline;
	host_push(0x66);
	zzapp_services_poll();
	CHECK(TH(0).wake_reason == ZZAPP_WAKE_READY);
}

/* The pass saw the deadline first and woke the app TIMEOUT; the message
 * lands before the app runs again. */
static void message_after_timeout_wake(unsigned n)
{
	(void)n;
	TH(0).state = ZZAPP_THREAD_BLOCKED;
	now_value = RT(0).wait_deadline;
	zzapp_services_poll();
	CHECK(TH(0).wake_reason == ZZAPP_WAKE_TIMEOUT);
	host_push(0x77);
}

static void test_message_during_timeout_is_not_lost(void)
{
	uint8_t msg[AAR_MSG_BYTES];

	reset();
	on_yield = message_at_deadline;
	CHECK(zzapp_svc_msg_wait(250000U) == AAR_OK);
	CHECK(zzapp_svc_msg_poll(msg) == 1 && msg[0] == 0x66);

	reset();
	on_yield = message_after_timeout_wake;
	CHECK(zzapp_svc_msg_wait(250000U) == AAR_OK);
	CHECK(zzapp_svc_msg_poll(msg) == 1 && msg[0] == 0x77);
	CHECK(!zzapp_services_wait_registered(PID_OF(0U)));
}

static void test_stop_cancels_the_wait(void)
{
	reset();
	test_force_state(0U, AAR_STATE_RUNNING);
	stop_with_message = 0;
	on_yield = stop_while_parked;
	CHECK(zzapp_svc_msg_wait(5000000U) == AAR_E_IO);
	CHECK(yields == 1U);
	CHECK(!zzapp_services_wait_registered(PID_OF(0U)));

	/* A message queued with the stop is still returned. */
	reset();
	test_force_state(0U, AAR_STATE_RUNNING);
	stop_with_message = 1;
	on_yield = stop_while_parked;
	CHECK(zzapp_svc_msg_wait(5000000U) == AAR_OK);

	/* Already stopping, by the client's own stop word: no park at all. */
	reset();
	mock_client_request_stop(&CL(0));
	CHECK(zzapp_svc_msg_wait(5000000U) == AAR_E_IO);
	CHECK(yields == 0U);

	/* A queued message still wins over a stop, so none is dropped. */
	host_push(0x44);
	CHECK(zzapp_svc_msg_wait(5000000U) == AAR_OK);
}

static void test_queue_corruption_is_an_error(void)
{
	reset();
	/* A completion queue whose tail is further ahead than it is deep. */
	aar_put_be32(CL(0).cq.tail, aar_get_be32(CL(0).cq.head) + ZZAPP_XPORT_DEPTH + 5U);
	CHECK(zzapp_svc_msg_wait(5000000U) == AAR_E_IO);
	CHECK(yields == 0U);
	CHECK(!zzapp_services_wait_registered(PID_OF(0U)));
	CHECK(zzapp_xport_flag_test(APP(0).rec, ZZAPP_XPORT_F_CORRUPT) != 0);
}

/* A client that keeps within its credit never fills the inbox: 63 messages
 * are accepted, the 64th is refused at the client, and taking one hands the
 * credit back. */
static void test_message_credit_is_returned_on_take(void)
{
	uint8_t msg[AAR_MSG_BYTES];
	unsigned i;

	reset();
	memset(msg, 0x42, sizeof(msg));
	for (i = 0U; i < AAR_MSG_CAPACITY; i++)
		CHECK(mock_client_post_msg(&CL(0), msg) == AAR_OK);
	CHECK(mock_client_post_msg(&CL(0), msg) == AAR_E_BUSY);
	CHECK(zzapp_svc_msg_wait(1000000U) == AAR_OK);
	CHECK(zzapp_svc_msg_poll(msg) == 1);
	CHECK(mock_client_post_msg(&CL(0), msg) == AAR_OK);
	CHECK(mock_client_post_msg(&CL(0), msg) == AAR_E_BUSY);
}

int main(void)
{
	svc_env_init(8U, 0U, NULL);
	test_ready_message_returns_at_once();
	test_message_that_arrives_later_wakes();
	test_message_between_check_and_park_is_not_missed();
	test_timeout();
	test_stop_cancels_the_wait();
	test_message_during_timeout_is_not_lost();
	test_queue_corruption_is_an_error();
	test_message_credit_is_returned_on_take();
	printf("msg_wait_test: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
