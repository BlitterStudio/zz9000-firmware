/*
 * The transport queues through the runtime, end to end: what the app's
 * service calls put on the submission queue is what the client sees, what the
 * client puts on the completion queue is what the app's calls return, and the
 * runtime keeps its own rules over both:
 *   - messages, log records and file requests produced by the app arrive in
 *     order and intact; events and messages produced by the client arrive in
 *     order, across more wraps than the queue is deep;
 *   - a full submission queue refuses a message (BUSY) and parks a request,
 *     never losing either;
 *   - a completion is not stuck behind the client's credited messages;
 *   - every mailbox operation that names a claim is refused for a stale,
 *     zero or foreign handle and changes nothing;
 *   - a teardown detaches a block from its record before it is freed;
 *   - many more apps than the old four launcher windows each run their own
 *     queues without touching one another.
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

static uint64_t test_time(void)
{
	return now_value;
}

static void reset(unsigned apps)
{
	unsigned n;

	svc_apps_reset();
	memset(&service_test_kernel, 0, sizeof(service_test_kernel));
	for (n = 0U; n < apps; n++)
		(void)svc_app_new(n);
	zzapp_services_test_set_core(1);
	zzapp_services_test_set_time(test_time);
	zzapp_services_test_set_yield(svc_yield);
	now_value = 1000U;
	if (apps != 0U)
		svc_select(0U);
}

/* ---- App to client --------------------------------------------------------- */

/* Messages and log records of the app reach the client in order and intact,
 * for more entries than the queue holds: the client drains between bursts. */
static void test_app_to_client_in_order_across_wraps(void)
{
	uint8_t msg[AAR_MSG_BYTES];
	uint8_t got[AAR_MSG_BYTES];
	unsigned i, round, seen = 0U;
	char log[32];

	reset(1U);
	for (round = 0U; round < 20U; round++) {
		for (i = 0U; i < 40U; i++) {
			memset(msg, (int)(round * 40U + i), sizeof(msg));
			CHECK(zzapp_svc_msg_send(msg) == AAR_OK);
		}
		(void)mock_client_serve(&CL(0));
		while (mock_client_get_msg(&CL(0), got) == 1) {
			memset(msg, (int)seen, sizeof(msg));
			CHECK(memcmp(got, msg, sizeof(got)) == 0);
			seen++;
		}
	}
	CHECK(seen == 800U);
	/* 800 messages through a 64-deep queue: the free-running indices wrapped
	 * more than twelve times and nothing was lost or reordered. */
	CHECK(CB(0, AAR_CB_SQ_TAIL) == 800U);
	CHECK(CB(0, AAR_CB_SQ_HEAD) == 800U);
	snprintf(log, sizeof(log), "line %d", 7);
	zzapp_svc_log(log, (uint32_t)strlen(log));
	(void)mock_client_serve(&CL(0));
	CHECK(CL(0).logs_seen == 1U && strcmp(CL(0).last_log, "line 7") == 0);
}

/* A full queue: a message is refused with BUSY (the client has stopped
 * draining) and goes through after the client drains; a log record is
 * dropped and counted; nothing already queued is disturbed. */
static void test_full_queue_refuses_and_recovers(void)
{
	uint8_t msg[AAR_MSG_BYTES];
	uint8_t got[AAR_MSG_BYTES];
	unsigned sent = 0U, seen = 0U;

	reset(1U);
	memset(msg, 0x33, sizeof(msg));
	while (zzapp_svc_msg_send(msg) == AAR_OK)
		sent++;
	/* One entry stays free for a completion. */
	CHECK(sent == AAR_MSG_CAPACITY);
	CHECK(zzapp_svc_msg_send(msg) == AAR_E_BUSY);
	zzapp_svc_log("dropped", 7U);
	zzapp_svc_log("dropped", 7U);
	CHECK(CB(0, AAR_CB_LOG_DROPPED) >= 1U);
	(void)mock_client_serve(&CL(0));
	while (mock_client_get_msg(&CL(0), got) == 1) {
		CHECK(got[0] == 0x33);
		seen++;
	}
	CHECK(seen == sent);
	CHECK(zzapp_svc_msg_send(msg) == AAR_OK);
}

/* ---- Client to app ----------------------------------------------------------- */

/* Events and messages the client posts arrive in order, for more than the
 * queue holds: the app polls between bursts. */
static void test_client_to_app_in_order_across_wraps(void)
{
	aar_event ev[16];
	uint8_t msg[AAR_MSG_BYTES];
	unsigned i, round, events = 0U, msgs = 0U;
	uint32_t n;

	reset(1U);
	for (round = 0U; round < 25U; round++) {
		for (i = 0U; i < 20U; i++) {
			aar_event e;

			memset(&e, 0, sizeof(e));
			e.type = AAR_EV_KEY_DOWN;
			e.time_ms = round * 20U + i;
			CHECK(mock_client_post_event(&CL(0), &e) == AAR_OK);
		}
		for (i = 0U; i < 10U; i++) {
			memset(msg, (int)((round * 10U + i) & 0xff), sizeof(msg));
			CHECK(mock_client_post_msg(&CL(0), msg) == AAR_OK);
		}
		do {
			unsigned k;

			CHECK(zzapp_svc_event_poll(ev, 16U, &n) == AAR_OK);
			for (k = 0U; k < n; k++) {
				CHECK(ev[k].time_ms == events);
				events++;
			}
		} while (n != 0U);
		while (zzapp_svc_msg_poll(msg) == 1) {
			CHECK(msg[0] == (uint8_t)(msgs & 0xff));
			msgs++;
		}
	}
	CHECK(events == 500U);
	CHECK(msgs == 250U);
	CHECK(CB(0, AAR_CB_MSG_CONSUMED) == 250U);
}

/* A completion is not blocked behind the client's credited messages: with
 * the inbox as full as the credits allow, a file request posted by the app
 * still completes, and the messages stay readable. */
static void test_completion_is_not_blocked_by_credited_messages(void)
{
	uint8_t msg[AAR_MSG_BYTES];
	unsigned posted = 0U, taken = 0U;
	int fd;

	reset(1U);
	memset(msg, 0x44, sizeof(msg));
	while (mock_client_post_msg(&CL(0), msg) == AAR_OK)
		posted++;
	CHECK(posted == AAR_MSG_CAPACITY);
	/* The request completes though the completion queue holds a full set of
	 * unread messages. */
	fd = zzapp_svc_file_open("sys:test", AAR_FILE_READ);
	CHECK(fd >= 0);
	while (zzapp_svc_msg_poll(msg) == 1)
		taken++;
	CHECK(taken == posted);
	CHECK(CB(0, AAR_CB_MSG_CONSUMED) == posted);
	/* The credits came back: the client may post again. */
	CHECK(mock_client_post_msg(&CL(0), msg) == AAR_OK);
}

/* ---- Handles ----------------------------------------------------------------- */

static uint16_t op_with(uint16_t op, uint32_t handle)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t len = 0U;

	zzapp_build_handle(req, handle);
	zzapp_services_test_set_core(0);
	return zzapp_mailbox_dispatch(op, req, sizeof(req), reply, &len, 1, 9U, 0U);
}

/* Every operation that names a claim is refused for a handle that is not the
 * live one, and changes nothing: the claim's state, queues and record stay. */
static void test_every_op_checks_the_handle(void)
{
	static const uint16_t ops[] = {
		ZZAPP_OP_LOAD_BEGIN, ZZAPP_OP_LOAD_CHUNK, ZZAPP_OP_LOAD_END,
		ZZAPP_OP_START, ZZAPP_OP_STOP, ZZAPP_OP_RELEASE, ZZAPP_OP_KICK,
	};
	zzapp_rec_t *rec;
	uint32_t handle, index, gen;
	unsigned i, bad;
	uint32_t wrong[4];

	reset(2U);
	rec = APP(0).rec;
	handle = APP(0).handle;
	index = aar_handle_index(handle);
	gen = aar_handle_generation(handle);
	wrong[0] = 0U;                                         /* none */
	wrong[1] = aar_handle_make(index, gen + 1U);           /* stale generation */
	wrong[2] = aar_handle_make(aar_handle_index(APP(1).handle), gen + 7U);
	wrong[3] = aar_handle_make(63U, 1U);                   /* a free index */
	for (i = 0U; i < sizeof(ops) / sizeof(ops[0]); i++) {
		for (bad = 0U; bad < 4U; bad++) {
			uint32_t tail = CB(0, AAR_CB_SQ_TAIL);

			CHECK(op_with(ops[i], wrong[bad]) == SDK_STATUS_BAD_HANDLE);
			CHECK(zzapp_rec_by_handle(handle) == rec);
			CHECK(zzapp_rec_by_handle(APP(1).handle) == APP(1).rec);
			CHECK(CB(0, AAR_CB_STATE) == AAR_STATE_IDLE);
			CHECK(CB(0, AAR_CB_SQ_TAIL) == tail);
		}
	}
	/* The right handle is accepted (a kick is the op with no side effect). */
	CHECK(op_with(ZZAPP_OP_KICK, handle) == SDK_STATUS_OK);
}

/* ---- Teardown ---------------------------------------------------------------- */

static zzapp_rec_t *releasing;
static unsigned releases_seen;

/* At the start of every block release the record no longer points at the
 * block: detach comes before free, so a replay frees nothing twice. */
static void detach_before_free(void *ptr, uint32_t card)
{
	zzapp_xport_buf_t buf;
	unsigned kind;

	(void)card;
	for (kind = 0U; kind < ZZAPP_XPORT_BLOCK_COUNT; kind++) {
		if (zzapp_xport_block_get(releasing, (zzapp_xport_block_kind_t)kind,
					  &buf) == 1)
			CHECK(buf.ptr != ptr);
	}
	releases_seen++;
}

static void test_teardown_detaches_before_it_frees(void)
{
	zzapp_xport_buf_t area;
	zzapp_rec_t *rec;

	reset(1U);
	rec = APP(0).rec;
	/* The app uses the service, data and load blocks too. */
	CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_SCRATCH, &area) == AAR_OK);
	CHECK(zzapp_xport_data_reserve(rec, 8192U, &area) == AAR_OK);
	CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_LOAD, 0x12000U, &area) == AAR_OK);
	CHECK(mock_heap.live == 4U);
	releasing = rec;
	releases_seen = 0U;
	mock_heap_on_release = detach_before_free;
	CHECK(zzapp_xport_release(rec) == 0);
	mock_heap_on_release = NULL;
	CHECK(releases_seen == 4U);
	CHECK(mock_heap.live == 0U && mock_heap.bad_frees == 0U);
	CHECK(!zzapp_xport_live(rec));
	/* A replay frees nothing. */
	CHECK(zzapp_xport_release(rec) == 0);
	CHECK(mock_heap.frees == 4U && mock_heap.bad_frees == 0U);
}

/* ---- Many apps --------------------------------------------------------------- */

/* Far more apps than the four windows of the old protocol, each with its own
 * queues: what one sends and receives is its own, and releasing them all
 * returns the heap. */
static void test_many_apps_keep_their_queues(void)
{
	uint8_t msg[AAR_MSG_BYTES];
	uint8_t got[AAR_MSG_BYTES];
	unsigned n, k;
	const unsigned apps = 72U;

	reset(apps);
	for (n = 0U; n < apps; n++) {
		svc_select(n);
		memset(msg, (int)n, sizeof(msg));
		for (k = 0U; k < 5U; k++)
			CHECK(zzapp_svc_msg_send(msg) == AAR_OK);
		memset(msg, (int)(0x80U + n), sizeof(msg));
		CHECK(mock_client_post_msg(&CL(n), msg) == AAR_OK);
	}
	mock_client_serve_all();
	for (n = 0U; n < apps; n++) {
		unsigned seen = 0U;

		svc_select(n);
		while (mock_client_get_msg(&CL(n), got) == 1) {
			CHECK(got[0] == (uint8_t)n);
			seen++;
		}
		CHECK(seen == 5U);
		CHECK(zzapp_svc_msg_poll(msg) == 1 && msg[0] == (uint8_t)(0x80U + n));
		CHECK(zzapp_svc_msg_poll(msg) == 0);
	}
	CHECK(mock_heap.live == apps);
	svc_apps_reset();
	CHECK(mock_heap.live == 0U && mock_heap.bad_frees == 0U);
}

int main(void)
{
	svc_env_init(80U, 0U, NULL);
	test_app_to_client_in_order_across_wraps();
	test_full_queue_refuses_and_recovers();
	test_client_to_app_in_order_across_wraps();
	test_completion_is_not_blocked_by_credited_messages();
	test_every_op_checks_the_handle();
	test_teardown_detaches_before_it_frees();
	test_many_apps_keep_their_queues();
	svc_apps_reset();
	printf("queue_runtime_test: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
