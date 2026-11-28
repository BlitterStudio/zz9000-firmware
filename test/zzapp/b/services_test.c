/*
 * The services over the AAR transport: file, console, log, event, message,
 * display and audio calls as the queue entries and control block words a
 * client sees, the lifecycle of a claim (load, lease, quarantine, release)
 * as the mailbox and the control block show it, and the record-lifetime
 * rules of the passes that walk the records.
 *
 * The Amiga is mock_client: it drains the submission queue from the yield
 * hook of a parked app and answers through the completion queue.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#define _Static_assert(cond, msg) typedef char zzapp_sa_##__LINE__[(cond) ? 1 : -1]
#endif

#define ZZAPP_SERVICES_STANDALONE_TEST
#include "zzapp_services.c"

#include "svc_env.h"
#include <stdio.h>
#include <string.h>

#define READ_BYTES (300u * 1024u)

static int g_fails;
/* The host-visible heap QUERY reports: a 64 MiB heap of which the mock heap's
 * live blocks are taken. */
#define HEAP_MODEL_BYTES (64U << 20)

uint32_t zzapp_test_hostheap_free(void)
{
	return HEAP_MODEL_BYTES - mock_heap.live_bytes;
}

uint32_t zzapp_test_hostheap_largest(void)
{
	return HEAP_MODEL_BYTES - mock_heap.live_bytes;
}

static uint64_t g_now;

static void fail(const char *what)
{
	fprintf(stderr, "fail: %s\n", what);
	g_fails++;
}

#define EXPECT(cond, what) do { if (!(cond)) fail(what); } while (0)

static uint64_t test_time(void)
{
	return g_now;
}

static int loader_chunk_succeeds(uint32_t file_off, const void *src,
				 uint32_t len)
{
	(void)file_off;
	(void)src;
	(void)len;
	return AAR_OK;
}

static void loader_begin_succeeds(void)
{
}

static int start_posted_deferred(unsigned n)
{
	if (DEF(n).state != DEFER_POSTED)
		return 0;
	DEF(n).state = DEFER_RUNNING;
	return 1;
}

/* A probe client for the mailbox operations that take no handle. */
static mock_client g_probe;

static void probe_init(void)
{
	mock_client_init(&g_probe);
	svc_client_wire(&g_probe);
}

static uint32_t query_word(uint32_t off)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };

	if (mock_client_query(&g_probe, reply) != SDK_STATUS_OK)
		return 0xdeadbeefU;
	return zzapp_get_be32(reply + off);
}

/* A fresh world with app 0 in the state the old implicit window record had
 * (IDLE, with a pair and an address space), selected on core 1. */
static zzapp_rec_t *world(void)
{
	zzapp_rec_t *rec;

	svc_apps_reset();
	probe_init();
	rec = svc_app_new(0U);
	zzapp_services_test_set_core(1);
	zzapp_services_test_set_time(test_time);
	zzapp_services_test_set_yield(svc_yield);
	g_now = 0U;
	return rec;
}

/* The same, but app 0 is a CLAIM through the mailbox (no address space). */
static zzapp_rec_t *world_claimed(void)
{
	zzapp_rec_t *rec;

	svc_apps_reset();
	probe_init();
	zzapp_services_test_set_core(0);
	zzapp_services_test_set_time(test_time);
	zzapp_services_test_set_yield(svc_yield);
	g_now = 0U;
	rec = svc_app_claim(0U);
	if (!rec)
		fail("claim");
	return rec;
}

/* Runs the queued deferred operation of app `n` on the system thread. */
static void load_begin_run(unsigned n, uint32_t caps, uint8_t reply[48],
			   uint16_t *reply_len)
{
	uint16_t status;

	zzapp_services_test_set_core(0);
	status = mock_client_load_begin(&CL(n), caps, reply, reply_len);
	if (status != SDK_STATUS_QUEUED)
		fail("load begin queued");
	zzapp_services_test_set_core(1);
	zzapp_services_select_pid(PID_OF(n));
	if (!start_posted_deferred(n))
		fail("load begin posted");
	run_deferred(service_rec());
}

static void harvest_ack(void)
{
	if (!zzapp_mailbox_harvest(NULL, NULL, NULL, NULL, NULL, NULL))
		fail("harvest completion");
	zzapp_mailbox_harvest_ack();
}

/* Claim app 0, load-begin it with the client capabilities `caps` and select
 * it on core 1: the state of an app a client has loaded, with a space. */
static void world_loaded(uint32_t caps)
{
	world_claimed();
	give_space(0U);
	if (svc_load_begin(0U, caps, NULL, NULL) != SDK_STATUS_OK)
		fail("load begin with client capabilities");
	zzapp_services_test_set_core(1);
}

/* ---- Z2 and sizes ----------------------------------------------------------- */

static void test_z2_query(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t n = 0U;
	uint16_t status;

	svc_apps_reset();
	zzapp_services_test_set_core(0);
	memset(reply, 0x5a, sizeof(reply));
	status = zzapp_mailbox_dispatch(ZZAPP_OP_QUERY, NULL, 0U, reply, &n, 0,
					1U, 0U);
	EXPECT(status == SDK_STATUS_UNSUPPORTED, "Z2 query status");
	EXPECT(n == ZZAPP_MAILBOX_PAYLOAD_BYTES, "Z2 query length");
	EXPECT(zzapp_get_be32(reply + ZZAPP_QUERY_CAPS) == 0U, "Z2 query caps");
	status = zzapp_mailbox_dispatch(ZZAPP_OP_CLAIM, NULL, 0U, reply, &n, 0,
					2U, 0U);
	EXPECT(status == SDK_STATUS_UNSUPPORTED, "Z2 claim status");
	status = zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, NULL, 0U, reply, &n,
					0, 3U, 0U);
	EXPECT(status == SDK_STATUS_UNSUPPORTED, "Z2 load status");
	zzapp_services_test_set_core(1);
}

/* ---- Files ------------------------------------------------------------------- */

static void test_chunked_read(void)
{
	static uint8_t buf[READ_BYTES];
	const mock_client_hist_t *h;
	uint32_t got = 0U;
	uint32_t i;
	int fd;
	int rc;

	world();
	fd = zzapp_svc_file_open("id1/pak0.pak", AAR_FILE_READ);
	EXPECT(fd >= AAR_FD_BASE, "open");
	h = mock_client_last(&CL(0), AAR_OP_FILE_OPEN);
	EXPECT(h && strcmp(h->path, "id1/pak0.pak") == 0, "open path in the scratch area");
	EXPECT(h && h->req.arg[AAR_A_FILE_OPEN_FLAGS] == AAR_FILE_READ, "open flags");
	rc = zzapp_svc_file_read(fd, buf, READ_BYTES, &got);
	EXPECT(rc == AAR_OK, "chunked read status");
	EXPECT(got == READ_BYTES, "chunked read length");
	for (i = 0; i < got; i++) {
		if (buf[i] != (uint8_t)i) {
			fail("chunked read order");
			break;
		}
	}
	EXPECT(CL(0).produced == READ_BYTES, "producer cursor");
	/* Chunks stop at the data block: 256 KiB, then the rest. */
	EXPECT(mock_client_count(&CL(0), AAR_OP_FILE_READ) == 2U, "chunk count");
	h = mock_client_hist_at(&CL(0), CL(0).hist_count - 1U);
	EXPECT(h && h->req.op == AAR_OP_FILE_READ &&
	       h->req.buf_len <= ZZAPP_XPORT_DATA_MAX, "chunk fits the data block");
	/* The client's error comes back as the call's result. */
	CL(0).forced[AAR_OP_FILE_READ] = 1U;
	CL(0).forced_result[AAR_OP_FILE_READ] = AAR_E_IO;
	rc = zzapp_svc_file_read(fd, buf, 64U, &got);
	EXPECT(rc == AAR_E_IO, "client error reaches the app");
	EXPECT(CB(0, AAR_CB_SQ_FULL) == 0U, "no SQ pressure");
}

static uint64_t file_now;
static int file_phase;

static uint64_t file_time(void)
{
	return file_now;
}

static void file_yield(void)
{
	if (file_phase == 0) {
		file_now += ZZAPP_FILE_TIMEOUT_US;
		return;
	}
	svc_yield();
}

/* A request that timed out is abandoned: its late completion matches nothing,
 * is dropped and counted, and the next request gets its own answer. The id of
 * a request is never reused, so the stale completion cannot be taken for it. */
static void test_late_file_reply_ids(void)
{
	int fd;
	const zzapp_xport_stats_t *st;
	uint32_t first_id;

	world();
	CL(0).frozen = 1;
	file_now = 0U;
	file_phase = 0;
	zzapp_services_test_set_time(file_time);
	zzapp_services_test_set_yield(file_yield);
	fd = zzapp_svc_file_open("pak0.pak", AAR_FILE_READ);
	EXPECT(fd == AAR_E_TIMEDOUT, "first open timeout");
	EXPECT(mock_client_sq_used(&CL(0)) == 1, "the timed-out request stays in the SQ");
	EXPECT(zzapp_xport_req_state(APP(0).rec, ZZAPP_XPORT_REQ_BLOCKING) ==
	       ZZAPP_XPORT_REQ_FREE, "timeout abandoned the request");
	first_id = zzapp_xport_req_id(APP(0).rec, ZZAPP_XPORT_REQ_BLOCKING);
	/* The abandoned open still names the path in the scratch area: nothing
	 * rewrites it while the client has not answered, so a second open waits. */
	EXPECT(zzapp_xport_buffer_held(APP(0).rec,
		ZZAPP_XPORT_NAMES_AREA(ZZAPP_XPORT_AREA_SCRATCH)),
	       "the timed-out open does not hold its path");
	CL(0).frozen = 0;
	file_phase = 1;
	file_now = 0U;
	fd = zzapp_svc_file_open("pak1.pak", AAR_FILE_READ);
	EXPECT(fd == AAR_E_BUSY, "a second open reused the path of the first");
	{
		zzapp_xport_buf_t blk;

		EXPECT(zzapp_xport_block_get(APP(0).rec, ZZAPP_XPORT_BLOCK_SERVICE, &blk) == 1 &&
		       memcmp(blk.ptr, "pak0.pak", 9U) == 0,
		       "the path of the abandoned open was rewritten");
	}
	/* The Amiga wakes up and answers everything in order. */
	svc_yield();
	fd = zzapp_svc_file_open("pak1.pak", AAR_FILE_READ);
	EXPECT(fd == AAR_FD_BASE + 1, "late reply applied or timed-out fd reused");
	st = zzapp_xport_stats(APP(0).rec);
	EXPECT(st->late_dropped == 1U, "late completion dropped and counted");
	EXPECT(st->unknown_dropped == 0U, "late completion is not unknown");
	EXPECT(zzapp_xport_req_id(APP(0).rec, ZZAPP_XPORT_REQ_BLOCKING) != first_id,
	       "ids are not reused");
}

/* The scheduler times a file wait out (the wake reason is a timeout): the
 * request is abandoned and the next one is answered normally. */
static void scheduler_file_timeout(void)
{
	service_rec()->thread.wake_reason = ZZAPP_WAKE_TIMEOUT;
}

static void test_scheduler_file_timeout_abandons_request(void)
{
	int fd;

	world();
	CL(0).frozen = 1;
	g_now = 0U;
	zzapp_services_test_set_yield(scheduler_file_timeout);
	fd = zzapp_svc_file_open("late.pak", AAR_FILE_READ);
	EXPECT(fd == AAR_E_TIMEDOUT, "scheduler file timeout");
	EXPECT(zzapp_xport_req_state(APP(0).rec, ZZAPP_XPORT_REQ_BLOCKING) ==
	       ZZAPP_XPORT_REQ_FREE, "scheduler timeout abandoned the request");
	EXPECT(!zzapp_services_wait_registered(PID_OF(0U)),
	       "timeout left the wait registered");
	service_rec()->thread.wake_reason = ZZAPP_WAKE_NONE;
	CL(0).frozen = 0;
	zzapp_services_test_set_yield(svc_yield);
	/* The abandoned open still holds its path until the client answers. */
	fd = zzapp_svc_file_open("next.pak", AAR_FILE_READ);
	EXPECT(fd == AAR_E_BUSY, "an open before the answer of the timed-out one");
	svc_yield();
	fd = zzapp_svc_file_open("next.pak", AAR_FILE_READ);
	EXPECT(fd == AAR_FD_BASE + 1, "next open after the scheduler timeout");
}

/* Spec 7.1, written out on its own: at least one of READ and WRITE, CREATE
 * and TRUNCATE need WRITE, EXCL needs CREATE, no other bit. */
static int open_flags_valid(uint32_t flags)
{
	int rd = (flags & AAR_FILE_READ) != 0U;
	int wr = (flags & AAR_FILE_WRITE) != 0U;
	int cr = (flags & AAR_FILE_CREATE) != 0U;
	int tr = (flags & AAR_FILE_TRUNCATE) != 0U;
	int ex = (flags & AAR_FILE_EXCL) != 0U;

	if ((flags & ~(AAR_FILE_READ | AAR_FILE_WRITE | AAR_FILE_CREATE |
		       AAR_FILE_TRUNCATE | AAR_FILE_EXCL)) != 0U)
		return 0;
	if (!rd && !wr)
		return 0;
	if ((cr || tr) && !wr)
		return 0;
	if (ex && !cr)
		return 0;
	return 1;
}

/* Every valid combination reaches the client in arg[FLAGS] unchanged; every
 * invalid one is AAR_E_INVAL and posts nothing: no entry in the SQ, and the
 * scratch area is untouched. */
static void test_file_open_flags(void)
{
	static const uint32_t unknown[] = {
		32U, 64U, 0x80U, 0x100U, 0x8000U, 0x80000000U, 0xffffffffU,
		0xffffffe3U, 32U | AAR_FILE_READ, 32U | AAR_FILE_READ | AAR_FILE_WRITE,
		0x80000000U | AAR_FILE_READ,
	};
	uint32_t flags;
	uint32_t valid = 0U;
	uint32_t invalid = 0U;
	unsigned i;

	world();
	for (i = 0U; i < 64U + sizeof(unknown) / sizeof(unknown[0]); i++) {
		uint32_t opens = mock_client_count(&CL(0), AAR_OP_FILE_OPEN);
		uint32_t posted = CL(0).hist_count;
		int fd;

		flags = i < 64U ? i : unknown[i - 64U];
		fd = zzapp_svc_file_open("flags.bin", flags);
		if (open_flags_valid(flags)) {
			valid++;
			if (fd < AAR_FD_BASE) {
				fail("valid open flags refused");
				continue;
			}
			if (mock_client_count(&CL(0), AAR_OP_FILE_OPEN) != opens + 1U)
				fail("valid open flags were not posted");
			if (mock_client_last(&CL(0), AAR_OP_FILE_OPEN)->req.arg[AAR_A_FILE_OPEN_FLAGS] != flags)
				fail("open flags changed on the wire");
			if (zzapp_svc_file_close(fd) != AAR_OK)
				fail("close after open");
		} else {
			invalid++;
			if (fd != AAR_E_INVAL)
				fail("invalid open flags were not AAR_E_INVAL");
			if (mock_client_count(&CL(0), AAR_OP_FILE_OPEN) != opens ||
			    CL(0).hist_count != posted)
				fail("invalid open flags reached the client");
			if (mock_client_sq_used(&CL(0)) != 0)
				fail("invalid open flags left an entry in the SQ");
		}
	}
	/* READ alone (1), and the writers: READ or not (2) times TRUNCATE or
	 * not (2) times none, CREATE, CREATE|EXCL (3). */
	EXPECT(valid == 13U &&
	       invalid == 64U - 13U + (uint32_t)(sizeof(unknown) / sizeof(unknown[0])),
	       "count of valid flag combinations");
}

/* ---- Events, console ------------------------------------------------------------- */

static void test_events_drop_on_full(void)
{
	aar_event ev[8];
	aar_event in;
	uint32_t n = 0U;
	uint32_t i;
	uint32_t got = 0U;
	int rc;

	world();
	memset(&in, 0, sizeof(in));
	in.type = AAR_EV_KEY_DOWN;
	/* The CQ holds 64 entries: the 65th event does not fit, and the client
	 * counts the drop in its own control block word. */
	for (i = 0; i < ZZAPP_XPORT_DEPTH; i++) {
		in.code = (uint16_t)i;
		if (mock_client_post_event(&CL(0), &in) != AAR_OK)
			fail("event post");
	}
	in.code = 999U;
	EXPECT(mock_client_post_event(&CL(0), &in) == AAR_E_BUSY, "event on a full CQ");
	EXPECT(CB(0, AAR_CB_EVENTS_DROPPED) == 1U, "event drop count");
	EXPECT(CB(0, AAR_CB_CQ_FULL) == 1U, "cq full count");
	while (got < ZZAPP_XPORT_DEPTH) {
		rc = zzapp_svc_event_poll(ev, 8U, &n);
		if (rc != AAR_OK || n == 0U) {
			fail("event poll");
			return;
		}
		for (i = 0; i < n; i++) {
			if (ev[i].code != (uint16_t)(got + i) ||
			    ev[i].type != AAR_EV_KEY_DOWN) {
				fail("event order");
				return;
			}
		}
		got += n;
	}
	rc = zzapp_svc_event_poll(ev, 8U, &n);
	EXPECT(rc == AAR_OK && n == 0U, "events drained");
	EXPECT(CL(0).cq.depth == 64U, "queue depth");
}

static void test_console_eof_ends_wait(void)
{
	uint8_t buf[4];
	uint32_t n = 99U;

	world();
	CL(0).console_in_eof = 1;
	EXPECT(zzapp_svc_console_read(buf, sizeof(buf), &n) == AAR_OK && n == 0U,
	       "console eof read");
	EXPECT(mock_client_last(&CL(0), AAR_OP_CONSOLE_READ) &&
	       mock_client_last(&CL(0), AAR_OP_CONSOLE_READ)->req.buf_len == sizeof(buf),
	       "console read names the room");
	EXPECT(!zzapp_services_wait_registered(PID_OF(0U)),
	       "console eof left the wait registered");
}

static void test_console_roundtrip(void)
{
	uint8_t buf[16];
	uint32_t n = 0U;

	world();
	memcpy(CL(0).console_in, "hello\n", 6U);
	CL(0).console_in_len = 6U;
	EXPECT(zzapp_svc_console_read(buf, sizeof(buf), &n) == AAR_OK && n == 6U &&
	       memcmp(buf, "hello\n", 6U) == 0, "console read bytes");
	EXPECT(zzapp_svc_console_write("out", 3U, &n) == AAR_OK && n == 3U,
	       "console write");
	/* The write left through the console-out ring: one request, the bytes in
	 * the service block. */
	mock_client_serve(&CL(0));
	svc_yield();
	EXPECT(CL(0).console_out_len == 3U &&
	       memcmp(CL(0).console_out, "out", 3U) == 0, "console bytes at the client");
	EXPECT(mock_client_count(&CL(0), AAR_OP_CONSOLE_WRITE) == 1U,
	       "one console write request");
}

/* The console output ring takes writes without a client round trip, and a
 * client that takes a few bytes at a time still gets all of them in order. */
static void test_console_partial_writes_continue(void)
{
	uint8_t big[5000];
	uint32_t n = 0U;
	uint32_t i;
	unsigned spins;

	world();
	for (i = 0U; i < sizeof(big); i++)
		big[i] = (uint8_t)(i * 7U);
	CL(0).console_out_chunk_cap = 100U;
	EXPECT(zzapp_svc_console_write(big, sizeof(big), &n) == AAR_OK &&
	       n == sizeof(big), "console write of 5000 bytes");
	for (spins = 0U; spins < 200U && CL(0).console_out_len < sizeof(big); spins++)
		svc_yield();
	EXPECT(CL(0).console_out_len == sizeof(big) &&
	       memcmp(CL(0).console_out, big, sizeof(big)) == 0,
	       "console bytes in order through partial writes");
}

/* ---- Audio -------------------------------------------------------------------------- */

static void test_audio_busy(void)
{
	aar_audio audio;
	int rc;

	world();
	zzapp_services_test_set_slot_busy(2U, 1);
	zzapp_services_test_set_slot_busy(1U, 1);
	rc = zzapp_svc_audio_open(22050U, 2U, &audio);
	EXPECT(rc == AAR_E_BUSY, "audio busy");
	rc = zzapp_svc_audio_open(11025U, 2U, &audio);
	EXPECT(rc == AAR_E_INVAL, "audio 11025");
	zzapp_services_test_set_slot_busy(2U, 0);
	zzapp_services_test_set_slot_busy(1U, 0);
	rc = zzapp_svc_audio_open(22050U, 2U, &audio);
	EXPECT(rc == AAR_OK && audio.rate == 22050U && audio.channels == 2U &&
	       audio.frames == 8192U, "audio grant");
	EXPECT(zzapp_svc_audio_close() == AAR_OK, "audio close");
}

static void test_audio_teardown_handoff(void)
{
	aar_audio audio;
	int rc;

	world();
	rc = zzapp_svc_audio_open(22050U, 2U, &audio);
	EXPECT(rc == AAR_OK, "audio open before end");
	zzapp_services_on_app_end();
	EXPECT(zzapp_services_test_audio_detach_count() == 0U, "app end reclaimed audio");
	EXPECT(zzapp_services_test_audio_request() == AUDIO_REQUEST_CLOSE,
	       "app end did not hand off close");
	EXPECT(zzapp_svc_audio_open(22050U, 2U, &audio) == AAR_E_BUSY,
	       "open reused audio before handoff");
	zzapp_services_poll();
	EXPECT(zzapp_services_test_audio_request() == AUDIO_REQUEST_CLOSE_QUIESCED,
	       "core 1 did not quiesce audio close");
	EXPECT(zzapp_svc_audio_open(22050U, 2U, &audio) == AAR_E_BUSY,
	       "open during audio quiesce");
	zzapp_services_core0_poll();
	EXPECT(zzapp_services_test_audio_detach_count() == 1U, "core 0 did not reclaim audio");
	EXPECT(zzapp_services_test_audio_request() == AUDIO_REQUEST_NONE,
	       "audio handoff still pending");
	EXPECT(zzapp_svc_audio_open(22050U, 2U, &audio) == AAR_OK, "open after audio reclaim");
	EXPECT(zzapp_svc_audio_close() == AAR_OK, "close after reclaim");
}

static void test_reset_preserves_audio_teardown(void)
{
	aar_audio audio;

	world();
	EXPECT(zzapp_svc_audio_open(22050U, 2U, &audio) == AAR_OK, "audio open before reset");
	zzapp_services_on_reset();
	EXPECT(zzapp_services_test_audio_request() == AUDIO_REQUEST_CLOSE,
	       "reset discarded audio close request");
	zzapp_services_poll();
	EXPECT(zzapp_services_test_audio_request() == AUDIO_REQUEST_CLOSE_QUIESCED,
	       "reset did not quiesce audio close");
	zzapp_services_core0_poll();
	EXPECT(zzapp_services_test_audio_detach_count() == 1U &&
	       zzapp_services_test_audio_request() == AUDIO_REQUEST_NONE,
	       "reset did not complete audio detach");
}

static void test_audio_write_cursor_wrap(void)
{
	/* The app's write cursor is 32-bit and wraps after ~54 h at 22050 Hz.
	 * Produced must stay >= staged across the wrap, or the fabric producer
	 * stalls. */
	uint64_t staged = 0xFFFFFFFCull * 4u; /* staged up to frame 2^32 - 4 */

	EXPECT(audio_produced_frames(0xFFFFFFFEu, staged) == 0xFFFFFFFEull,
	       "cursor before the wrap");
	EXPECT(audio_produced_frames(0x00000003u, staged) == 0x100000003ull,
	       "cursor across the wrap");
	staged = 0x100000001ull * 4u; /* staging itself crossed the wrap */
	EXPECT(audio_produced_frames(0x00000005u, staged) == 0x100000005ull,
	       "cursor after staging crossed the wrap");
	EXPECT(audio_produced_frames(0x00000001u, staged) == 0x100000001ull,
	       "cursor equal to staged");
}

/* The AHI route: the runtime allocates the ring and names it in the request;
 * the client echoes it. */
static void test_audio_ahi_route_names_the_ring(void)
{
	aar_audio audio;
	const mock_client_hist_t *h;
	zzapp_xport_buf_t ring;

	world_loaded(ZZAPP_CLIENT_AUDIO_AHI);
	EXPECT(zzapp_svc_audio_open(22050U, 2U, &audio) == AAR_OK, "ahi audio open");
	h = mock_client_last(&CL(0), AAR_OP_AUDIO_OPEN);
	if (!h) {
		/* The platform route was taken: no AHI request is posted. */
		return;
	}
	EXPECT(h->req.arg[AAR_A_AUDIO_OPEN_ROUTE] == AAR_AUDIO_ROUTE_AHI, "ahi route");
	/* The request names the ring the runtime allocated: its token, and the
	 * ring's own length (header and samples), which the block rounds up. */
	EXPECT(zzapp_xport_block_get(APP(0).rec, ZZAPP_XPORT_BLOCK_AUDIO, &ring) == 1 &&
	       h->req.buf == ring.token &&
	       h->req.buf_len == AAR_AUDIO_RING_HDR_BYTES + ZZAPP_AUDIO_FRAMES * 4U &&
	       h->req.buf_len <= ring.bytes,
	       "request names the runtime's ring");
	EXPECT(zzapp_svc_audio_close() == AAR_OK, "ahi audio close");
	EXPECT(zzapp_xport_block_get(APP(0).rec, ZZAPP_XPORT_BLOCK_AUDIO, &ring) == 0,
	       "ring returned at close");
}

/* The audio ring of a record: the transport's AUDIO block (the app's own
 * ring, which the card reads in place), NULL while none is open. */
static void *audio_ring_ptr(zzapp_rec_t *rec)
{
	zzapp_xport_buf_t ring;

	return zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, &ring) == 1 ?
	       ring.ptr : NULL;
}

/* ---- Two apps: state is per record ----------------------------------------------- */

static void test_per_app_file_console_and_audio_state(void)
{
	aar_audio first;
	aar_audio second;
	int first_fd;
	int second_fd;
	uint32_t written;
	unsigned spins;

	svc_apps_reset();
	probe_init();
	svc_app_new(0U);
	svc_app_new(1U);
	zzapp_services_test_set_core(1);
	zzapp_services_test_set_yield(svc_yield);
	svc_select(0U);
	first_fd = zzapp_svc_file_open("slot0", AAR_FILE_READ);
	svc_select(1U);
	second_fd = zzapp_svc_file_open("slot1", AAR_FILE_READ);
	EXPECT(first_fd == AAR_FD_BASE && second_fd == AAR_FD_BASE, "per-app file handles");
	EXPECT(strcmp(mock_client_last(&CL(0), AAR_OP_FILE_OPEN)->path, "slot0") == 0 &&
	       strcmp(mock_client_last(&CL(1), AAR_OP_FILE_OPEN)->path, "slot1") == 0,
	       "each app's request is in its own queue");
	svc_select(0U);
	EXPECT(zzapp_svc_console_write("a", 1U, &written) == AAR_OK && written == 1U,
	       "app zero console write");
	svc_select(1U);
	EXPECT(zzapp_svc_console_write("b", 1U, &written) == AAR_OK && written == 1U,
	       "app one console write");
	for (spins = 0U; spins < 20U; spins++)
		svc_yield();
	EXPECT(CL(0).console_out_len == 1U && CL(0).console_out[0] == 'a', "app zero console data");
	EXPECT(CL(1).console_out_len == 1U && CL(1).console_out[0] == 'b', "app one console data");
	svc_select(0U);
	EXPECT(zzapp_svc_audio_open(22050U, 2U, &first) == AAR_OK, "first app audio open");
	svc_select(1U);
	EXPECT(zzapp_svc_audio_open(22050U, 2U, &second) == AAR_OK &&
	       audio_ring_ptr(APP(0).rec) != NULL && audio_ring_ptr(APP(1).rec) != NULL &&
	       audio_ring_ptr(APP(0).rec) != audio_ring_ptr(APP(1).rec), "per-app audio state");
	svc_select(0U);
	EXPECT(zzapp_svc_audio_close() == AAR_OK, "first app audio close");
	svc_select(1U);
	EXPECT(zzapp_svc_audio_close() == AAR_OK, "second app audio close");
}

/* On core 1 the services act for the running app, whatever core 0 selected. */
static void test_running_app_ignores_mailbox_selection(void)
{
	svc_apps_reset();
	probe_init();
	svc_app_new(0U);
	svc_app_new(1U);
	zzapp_services_test_set_core(0);
	svc_select(0U);
	EXPECT(zzapp_services_current_pid() == PID_OF(0U), "core 0 selection");
	zzapp_services_test_set_core(1);
	service_test_current_thread = (int)PID_OF(1U);
	EXPECT(zzapp_services_current_pid() == PID_OF(1U),
	       "the running app is not the mailbox selection");
	service_test_current_thread = -1;
}

/* core 0 never wakes an app: only core 1's pass pumps the completion queue. */
static unsigned g_wake_phase;

static void wake_yield(void)
{
	if (g_wake_phase == 0U) {
		g_wake_phase = 1U;
		/* The app is parked on its request. Core 0's pass leaves it. */
		EXPECT(zzapp_services_wait_registered(PID_OF(0U)), "file wait registered");
		zzapp_services_test_set_core(0);
		zzapp_services_core0_poll();
		EXPECT(service_test_wake_count == 0U, "core zero called wake");
		zzapp_services_test_set_core(1);
		/* The Amiga answers while the app is parked: core 1's pass
		 * pumps the completion and wakes the waiter. */
		(void)mock_client_serve(&CL(0));
		zzapp_services_poll();
		return;
	}
	zzapp_services_poll();
}

static void test_core1_wakes_ready_waiter(void)
{
	int fd;

	world();
	service_test_wake_count = 0U;
	g_wake_phase = 0U;
	zzapp_services_test_set_yield(wake_yield);
	fd = zzapp_svc_file_open("wake", AAR_FILE_READ);
	EXPECT(fd >= AAR_FD_BASE, "open that parked");
	EXPECT(service_test_wake_count >= 1U, "core one did not wake the waiter");
	EXPECT(!zzapp_services_wait_registered(PID_OF(0U)), "wait ended");
}

/* ---- Display ---------------------------------------------------------------------------- */

/* 320x200 RGB565 is 128000 bytes: the buffers sit 0x40000 apart, so the
 * address between the first two (0x00230000) is outside every buffer. */
#define FRONT 0x00200000U
#define BACK  0x00240000U
#define SPARE 0x00280000U

static void client_display(mock_client *c, uint32_t front, uint32_t back,
			   uint32_t spare, uint32_t caps)
{
	c->display_front = SVC_TOKEN(front);
	c->display_back = SVC_TOKEN(back);
	c->display_spare = spare ? SVC_TOKEN(spare) : 0U;
	c->display_dispcaps = caps;
}

static int open_display(unsigned n, uint32_t w, uint32_t h, uint32_t fmt,
			uint32_t flags, aar_surface *surface)
{
	memset(surface, 0, sizeof(*surface));
	svc_select(n);
	return zzapp_svc_display_open(w, h, fmt, flags, surface);
}

/* A client without scanout acknowledgement: the completion of the flip
 * request is the acknowledgement. */
static void test_display_refresh_rotation_and_layout(void)
{
	aar_surface surface;

	world_loaded(0U);
	client_display(&CL(0), FRONT, BACK, 0U, 0U);
	CL(0).flip_flags = 0U;
	EXPECT(open_display(0U, 320U, 200U, AAR_FMT_RGB565, 0U, &surface) == AAR_OK,
	       "display open");
	EXPECT(mock_client_last(&CL(0), AAR_OP_DISPLAY_OPEN) != NULL, "display open request");
	EXPECT(CB(0, AAR_CB_FLAGS) & AAR_CBF_DISPLAY, "control block shows the display");
	memset(&surface, 0, sizeof(surface));
	surface.id = 1U;
	EXPECT(zzapp_svc_display_refresh(&surface) == AAR_OK &&
	       PHYS(surface.front) == FRONT && PHYS(surface.back) == BACK &&
	       surface.layout == AAR_LAYOUT_RGB565_BE, "display initial surface");
	/* A flip the client holds is pending: the surface does not rotate. */
	CL(0).delay[AAR_OP_DISPLAY_FLIP] = 2U;
	EXPECT(zzapp_svc_display_flip_async(1U) == AAR_OK, "display flip");
	(void)mock_client_serve(&CL(0));
	EXPECT(mock_client_last(&CL(0), AAR_OP_DISPLAY_FLIP) != NULL, "flip request posted");
	EXPECT(zzapp_svc_display_refresh(&surface) == AAR_E_BUSY, "display pending refresh");
	/* The completion arrives with no SUBMITTED flag: the flip is complete
	 * without waiting for the card's vertical blank. */
	CL(0).delay[AAR_OP_DISPLAY_FLIP] = 0U;
	svc_yield();
	svc_yield();
	EXPECT(zzapp_svc_display_refresh(&surface) == AAR_OK &&
	       PHYS(surface.front) == BACK && PHYS(surface.back) == FRONT,
	       "display refreshed rotation");
}

static void test_display_scanout_ack(void)
{
	aar_surface surface;

	world_loaded(ZZAPP_CLIENT_SCANOUT_ACK);
	client_display(&CL(0), FRONT, BACK, SPARE, 0U);
	CL(0).flip_flags = ZZAPP_FLIPF_SUBMITTED;
	EXPECT(open_display(0U, 320U, 200U, AAR_FMT_RGB565, 0U, &surface) == AAR_OK,
	       "display scanout-ack open");
	surface.id = 1U;
	EXPECT(zzapp_svc_display_flip_async(surface.id) == AAR_OK, "display scanout-ack queue");
	svc_yield();
	/* Submitted by the client, shown when the card scans out the new buffer. */
	zzapp_services_display_vblank(FRONT);
	EXPECT(zzapp_display_flip_idle() == 0, "display scanout-ack wrong address");
	zzapp_services_display_vblank(BACK);
	EXPECT(zzapp_display_flip_idle() == 1, "display scanout-ack vblank");
	EXPECT(zzapp_svc_display_refresh(&surface) == AAR_OK &&
	       PHYS(surface.front) == BACK && PHYS(surface.back) == SPARE,
	       "display scanout-ack recycle");
}

static uint64_t g_flip_us;
static unsigned g_flip_yields;
static unsigned g_flip_unregistered_yields;

static uint64_t flip_time(void)
{
	return g_flip_us;
}

/* Model a parked app: it resumes only after the service pass wakes its
 * registered wait, first at the 50 ms diagnostic deadline and then when the
 * card acknowledges. */
static void flip_yield(void)
{
	if (!zzapp_services_wait_registered(service_pid()))
		g_flip_unregistered_yields++;
	g_flip_yields++;
	if (current_runtime()->wait_deadline != 0U) {
		g_flip_us = current_runtime()->wait_deadline;
	} else {
		g_flip_us = 80000U;
		zzapp_services_display_vblank(BACK);
	}
	(void)mock_client_serve(&CL(0));
	zzapp_services_poll();
}

static void test_display_wait_flip_idle(void)
{
	aar_surface surface;

	world_loaded(ZZAPP_CLIENT_SCANOUT_ACK);
	client_display(&CL(0), FRONT, BACK, SPARE, 0U);
	CL(0).flip_flags = ZZAPP_FLIPF_SUBMITTED;
	EXPECT(open_display(0U, 320U, 200U, AAR_FMT_RGB565, 0U, &surface) == AAR_OK,
	       "flip wait display open");
	g_flip_us = 0U;
	g_flip_yields = 0U;
	g_flip_unregistered_yields = 0U;
	flip_wait_reports = 0U;
	zzapp_services_test_set_time(flip_time);
	zzapp_services_test_set_yield(flip_yield);
	/* Nothing queued: idle at once, no wait. */
	EXPECT(zzapp_display_wait_flip_idle() == AAR_OK && g_flip_yields == 0U,
	       "flip wait while idle");
	EXPECT(zzapp_svc_display_flip_async(1U) == AAR_OK, "flip wait queue");
	/* The first pass wakes at the 50 ms diagnostic deadline. The second
	 * remains parked until VBlank acknowledges the queued flip at 80 ms. */
	EXPECT(zzapp_display_wait_flip_idle() == AAR_OK, "flip wait result");
	EXPECT(g_flip_yields >= 2U && g_flip_unregistered_yields == 0U,
	       "flip wait registers with the service pass");
	EXPECT(!zzapp_services_wait_registered(service_pid()), "flip wait ended");
	EXPECT(flip_wait_reports == 1U, "slow flip reported once");
}

/* A CLUT8 palette travels as an AAR_OP_DISPLAY_PALETTE request: the colours
 * big-endian 0x00RRGGBB in the scratch area. */
static void test_clut8_palette_publication(void)
{
	aar_surface surface;
	uint32_t rgb[] = { 0x11223344U, 0x00a0b0c0U };
	const mock_client_hist_t *h;

	world_loaded(ZZAPP_CLIENT_CLUT8);
	client_display(&CL(0), FRONT, BACK, 0U, AAR_DISPCAP_CLUT8);
	EXPECT(open_display(0U, 320U, 200U, AAR_FMT_CLUT8, 0U, &surface) == AAR_OK,
	       "CLUT8 display open");
	EXPECT(zzapp_svc_display_set_palette(1U, 4U, 2U, rgb) == AAR_OK, "CLUT8 palette call");
	svc_yield();
	h = mock_client_last(&CL(0), AAR_OP_DISPLAY_PALETTE);
	EXPECT(h && h->req.arg[AAR_A_DISPLAY_PALETTE_FIRST] == 4U &&
	       h->req.arg[AAR_A_DISPLAY_PALETTE_COUNT] == 2U, "palette request words");
	EXPECT(CL(0).palette[4] == 0x223344U && CL(0).palette[5] == 0xa0b0c0U,
	       "palette colours read through the scratch area");
}

/* ---- Palette updates: aar_display_set_palette does not block ----------------------------- */

/* The client's palette as the tests model it: every call writes the colours
 * here too, and what the client ends up holding must equal it. */
static uint32_t pal_model[256];
static uint8_t pal_written[256], pal_seen[256];
static unsigned pal_yields;

struct pal_req {
	uint32_t id, first, count, display;
	const uint8_t *buf;
	uint8_t snap[1024];
};
static struct pal_req pal_log[32];
static unsigned pal_reqs, pal_inflight, pal_max_inflight;
static int pal_hold;

/* A yield inside a palette call means the app parked: after a while the stop
 * request ends the wait, so a regression fails instead of hanging. */
static void pal_yield(void)
{
	pal_yields++;
	if (pal_yields == 20U)
		mock_client_request_stop(&CL(0));
	svc_yield();
}

/* The client: it reads the colours of the buffer the request names when it
 * takes the request, and either answers at once or holds the completion. */
static mock_verdict_t pal_on_request(mock_client *c, const aar_request *req,
				     aar_completion *comp)
{
	struct pal_req *p;
	const uint8_t *buf;
	uint32_t i;

	(void)comp;
	if (req->op != AAR_OP_DISPLAY_PALETTE)
		return MOCK_DEFAULT;
	buf = (const uint8_t *)mock_client_buf(req);
	p = &pal_log[pal_reqs % 32U];
	p->id = req->id;
	p->first = req->arg[AAR_A_DISPLAY_PALETTE_FIRST];
	p->count = req->arg[AAR_A_DISPLAY_PALETTE_COUNT];
	p->display = req->arg[AAR_A_DISPLAY_ID];
	p->buf = buf;
	pal_reqs++;
	pal_inflight++;
	if (pal_inflight > pal_max_inflight)
		pal_max_inflight = pal_inflight;
	if (!buf || p->count == 0U || p->count > 256U || p->first >= 256U ||
	    p->count > 256U - p->first || req->buf_len < p->count * 4U) {
		fail("palette request names its colours");
		pal_inflight--;
		return MOCK_COMPLETE;
	}
	memcpy(p->snap, buf, p->count * 4U);
	for (i = 0U; i < p->count; i++)
		c->palette[p->first + i] = zzapp_get_be32(buf + i * 4U);
	if (pal_hold)
		return MOCK_HOLD;
	pal_inflight--;
	return MOCK_COMPLETE;
}

static const struct pal_req *pal_req_at(unsigned n)
{
	return &pal_log[n % 32U];
}

/* The held request `n` is answered; core 1's next pass takes it. */
static void pal_complete(unsigned n)
{
	pal_inflight--;
	EXPECT(mock_client_complete(&CL(0), pal_req_at(n)->id, AAR_OK, NULL, 0U) ==
	       AAR_OK, "held palette request completes");
}

/* The Amiga serves, core 1 posts what is pending, the Amiga serves again. */
static void pal_settle(void)
{
	unsigned i;

	for (i = 0U; i < 4U; i++)
		svc_yield();
}

static int pal_set(uint32_t first, uint32_t count, uint32_t tag)
{
	uint32_t rgb[256];
	uint32_t i;

	for (i = 0U; i < count; i++) {
		/* The top byte is not a colour: it must not reach the client. */
		rgb[i] = 0xff000000U | ((tag & 0xffU) << 16) |
			 (((first + i) & 0xffU) << 8) | (i & 0xffU);
		if (first + i < 256U) {
			pal_model[first + i] = rgb[i] & 0x00ffffffU;
			pal_written[first + i] = 1U;
		}
	}
	return zzapp_svc_display_set_palette(1U, first, count, rgb);
}

static void pal_client_matches_model(const char *what)
{
	uint32_t i;

	for (i = 0U; i < 256U; i++) {
		if (CL(0).palette[i] != pal_model[i]) {
			fail(what);
			fprintf(stderr, "  entry %u: client %06x want %06x\n",
				(unsigned)i, (unsigned)CL(0).palette[i],
				(unsigned)pal_model[i]);
			return;
		}
	}
}

/* A CLUT8 display for app 0 with a palette the client holds already. */
static void pal_world(int hold)
{
	aar_surface surface;
	uint32_t i;

	world_loaded(ZZAPP_CLIENT_CLUT8);
	client_display(&CL(0), FRONT, BACK, 0U, AAR_DISPCAP_CLUT8);
	EXPECT(open_display(0U, 320U, 200U, AAR_FMT_CLUT8, 0U, &surface) == AAR_OK,
	       "palette display open");
	for (i = 0U; i < 256U; i++)
		CL(0).palette[i] = pal_model[i] = 0x00bb0000U | i;
	memset(pal_written, 0, sizeof(pal_written));
	memset(pal_seen, 0, sizeof(pal_seen));
	pal_yields = 0U;
	pal_reqs = pal_inflight = pal_max_inflight = 0U;
	pal_hold = hold;
	CL(0).on_request = pal_on_request;
	zzapp_services_test_set_yield(pal_yield);
}

static void pal_done(void)
{
	CL(0).on_request = NULL;
	zzapp_services_test_set_yield(svc_yield);
}

/* The contract of aar_display_set_palette: it does not block. A client that
 * answers nothing must not park the app, however many updates it makes. */
static void test_palette_never_parks_with_a_silent_client(void)
{
	unsigned i;
	int rc = AAR_OK;

	pal_world(0);
	CL(0).frozen = 1;
	for (i = 0U; i < 100U && rc == AAR_OK; i++)
		rc = pal_set(0U, 256U, i);
	EXPECT(rc == AAR_OK, "palette call with a silent client returns OK");
	EXPECT(pal_yields == 0U, "palette call with a silent client does not park");
	EXPECT(!zzapp_services_wait_registered(PID_OF(0U)),
	       "palette call leaves no wait registered");
	CL(0).frozen = 0;
	pal_settle();
	pal_client_matches_model("silent client: the latest palette arrives");
	pal_done();
}

/* One request in flight at a time. The buffer it names is not written until
 * the client has answered, later updates collect (latest wins, an entry that
 * nobody wrote is never sent) and go out one run at a time. */
static void test_palette_coalesces_behind_one_request_in_flight(void)
{
	const struct pal_req *first;
	unsigned n, k;
	uint8_t snap[1024];

	pal_world(1);
	EXPECT(pal_set(0U, 256U, 1U) == AAR_OK, "full palette");
	pal_settle();
	EXPECT(pal_reqs == 1U, "the first update goes out at once");
	first = pal_req_at(0U);
	EXPECT(first->first == 0U && first->count == 256U, "first request range");
	memcpy(snap, first->buf, sizeof(snap));
	memset(pal_written, 0, sizeof(pal_written));
	/* Overlapping and disjoint updates while the first is unanswered. */
	EXPECT(pal_set(10U, 3U, 2U) == AAR_OK, "update 10..12");
	EXPECT(pal_set(11U, 1U, 3U) == AAR_OK, "overlapping update of 11");
	EXPECT(pal_set(200U, 2U, 4U) == AAR_OK, "update 200..201");
	EXPECT(pal_set(10U, 2U, 5U) == AAR_OK, "overlapping update 10..11");
	EXPECT(pal_set(255U, 1U, 6U) == AAR_OK, "update of the last entry");
	EXPECT(pal_set(0U, 1U, 7U) == AAR_OK, "update of the first entry");
	pal_settle();
	EXPECT(pal_reqs == 1U && pal_inflight == 1U,
	       "nothing else goes out while the request is unanswered");
	EXPECT(memcmp(first->buf, snap, sizeof(snap)) == 0,
	       "the buffer of a request in flight is not written");
	/* Each answer lets the next run out; the runs are what was written. */
	for (n = 0U; n < 8U && pal_inflight != 0U; n++) {
		pal_complete(n);
		pal_settle();
		EXPECT(pal_max_inflight == 1U, "one request in flight at most");
	}
	EXPECT(pal_inflight == 0U, "every request was answered");
	for (n = 1U; n < pal_reqs; n++) {
		const struct pal_req *r = pal_req_at(n);

		for (k = r->first; k < r->first + r->count; k++)
			pal_seen[k]++;
	}
	for (k = 0U; k < 256U; k++)
		if (pal_seen[k] != pal_written[k]) {
			fail("every written entry is sent once, no other");
			fprintf(stderr, "  entry %u: sent %u times, written %u\n",
				k, pal_seen[k], pal_written[k]);
			break;
		}
	pal_client_matches_model("overlap and holes: the latest colours arrive");
	pal_done();
}

/* Latest wins when the same entries are written many times behind one
 * request in flight, and the colours are the last ones written. */
static void test_palette_latest_wins(void)
{
	unsigned n;
	unsigned i;

	pal_world(1);
	EXPECT(pal_set(0U, 256U, 1U) == AAR_OK, "initial palette");
	pal_settle();
	for (i = 0U; i < 50U; i++)
		EXPECT(pal_set((i * 7U) & 0x7fU, 100U, 10U + i) == AAR_OK,
		       "overlapping update");
	for (n = 0U; n < 12U && pal_inflight != 0U; n++) {
		pal_complete(n);
		pal_settle();
	}
	EXPECT(pal_inflight == 0U && pal_max_inflight == 1U,
	       "one request in flight at most, all answered");
	pal_client_matches_model("latest colours win");
	pal_done();
}

/* A full SQ is not an error for the app and loses nothing: the update waits
 * and goes out when the client has drained. */
static void test_palette_survives_a_full_sq(void)
{
	zzapp_rec_t *rec;
	uint8_t msg[AAR_MSG_BYTES] = { 0 };
	aar_request req;
	uint32_t id;

	pal_world(0);
	rec = APP(0U).rec;
	CL(0).frozen = 1;
	while (zzapp_xport_post_msg(rec, msg) == AAR_OK)
		;
	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_FILE_CLOSE;
	EXPECT(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &req, 0U, &id) ==
	       AAR_OK, "the last SQ entry");
	EXPECT(zzapp_xport_sq_space(rec) == 0, "the SQ is full");
	EXPECT(pal_set(3U, 5U, 1U) == AAR_OK, "palette call with a full SQ");
	EXPECT(pal_set(4U, 5U, 2U) == AAR_OK, "second palette call with a full SQ");
	EXPECT(pal_yields == 0U, "a full SQ does not park the app");
	CL(0).frozen = 0;
	pal_settle();
	EXPECT(pal_reqs >= 1U, "the update goes out once the client drained");
	pal_client_matches_model("full SQ: the latest palette arrives");
	pal_done();
}

/* The buffer of a request in flight stays while the screen is closed and
 * opened again; an update made for the old screen is not sent to the new
 * one, and one made for the new screen is. */
static void test_palette_across_close_and_reopen(void)
{
	aar_surface surface;
	const struct pal_req *held;
	uint8_t snap[16];

	pal_world(1);
	EXPECT(pal_set(0U, 4U, 1U) == AAR_OK, "palette before close");
	pal_settle();
	EXPECT(pal_reqs == 1U, "request in flight");
	held = pal_req_at(0U);
	memcpy(snap, held->buf, sizeof(snap));
	EXPECT(pal_set(5U, 2U, 2U) == AAR_OK, "update pending for the old screen");
	EXPECT(zzapp_svc_display_close(1U) == AAR_OK, "display close");
	EXPECT(open_display(0U, 320U, 200U, AAR_FMT_CLUT8, 0U, &surface) == AAR_OK,
	       "display reopen");
	EXPECT(pal_set(100U, 2U, 3U) == AAR_OK, "update for the new screen");
	pal_settle();
	EXPECT(pal_reqs == 1U, "no request goes out while the old one is unanswered");
	EXPECT(memcmp(held->buf, snap, sizeof(snap)) == 0,
	       "the buffer in flight survives close and reopen");
	pal_complete(0U);
	pal_settle();
	EXPECT(pal_reqs == 2U, "the update of the new screen goes out");
	if (pal_reqs >= 2U) {
		const struct pal_req *r = pal_req_at(1U);

		EXPECT(r->first == 100U && r->count == 2U,
		       "only the new screen's colours are sent");
		pal_complete(1U);
	}
	pal_settle();
	EXPECT(pal_reqs == 2U, "nothing more is sent");
	/* The update that belonged to the closed screen is gone. */
	pal_model[5] = 0x00bb0005U;
	pal_model[6] = 0x00bb0006U;
	pal_client_matches_model("the old screen's update never reaches the new one");
	pal_done();
}

/* An update kept for the client's display is not dropped by a flip, which
 * leaves the display as it is: the pending colours are sent when it can. */
static void test_palette_pending_survives_a_flip(void)
{
	pal_world(1);
	EXPECT(pal_set(0U, 8U, 1U) == AAR_OK, "first palette");
	pal_settle();
	EXPECT(pal_set(20U, 4U, 2U) == AAR_OK, "pending palette");
	(void)zzapp_svc_display_flip_async(1U);
	pal_settle();
	pal_complete(0U);
	pal_settle();
	EXPECT(pal_reqs == 2U, "the pending palette goes out after the flip");
	if (pal_reqs >= 2U)
		pal_complete(1U);
	pal_settle();
	pal_client_matches_model("pending palette survives a flip");
	pal_done();
}

/* Two claims: each client sees its own colours, and one claim's unanswered
 * request does not hold up the other. */
static void test_palette_of_two_claims_is_independent(void)
{
	aar_surface surface;
	uint32_t rgb0[2] = { 0x00112233U, 0x00445566U };
	uint32_t rgb1[2] = { 0x00aabbccU, 0x00ddeeffU };
	uint32_t i;

	pal_world(1);
	zzapp_services_test_set_core(0);
	EXPECT(svc_app_claim(1U) != NULL, "second claim");
	give_space(1U);
	EXPECT(svc_load_begin(1U, ZZAPP_CLIENT_CLUT8, NULL, NULL) == SDK_STATUS_OK,
	       "second load begin");
	zzapp_services_test_set_core(1);
	client_display(&CL(1), 0x00300000U, 0x00340000U, 0U, AAR_DISPCAP_CLUT8);
	EXPECT(open_display(1U, 320U, 200U, AAR_FMT_CLUT8, 0U, &surface) == AAR_OK,
	       "second display open");
	for (i = 0U; i < 256U; i++)
		CL(1).palette[i] = 0x00cc0000U | i;
	svc_select(0U);
	EXPECT(zzapp_svc_display_set_palette(1U, 0U, 2U, rgb0) == AAR_OK,
	       "first claim palette");
	svc_yield();
	EXPECT(pal_reqs == 1U, "first claim request in flight, held");
	svc_select(1U);
	EXPECT(zzapp_svc_display_set_palette(1U, 8U, 2U, rgb1) == AAR_OK,
	       "second claim palette while the first is unanswered");
	pal_settle();
	EXPECT(CL(1).palette[8] == 0x00aabbccU && CL(1).palette[9] == 0x00ddeeffU,
	       "second claim's client has its colours");
	EXPECT(CL(0).palette[8] != 0x00aabbccU && CL(0).palette[9] != 0x00ddeeffU,
	       "first claim's client does not");
	EXPECT(CL(1).palette[0] == 0x00cc0000U, "second claim's other entries stay");
	svc_select(0U);
	pal_complete(0U);
	pal_settle();
	pal_done();
}

/* The claim ends with an update in flight and one waiting: nothing is posted
 * after the end, the request is given up, its late answer is dropped by id,
 * and the buffer stays untouched. */
static void test_palette_at_the_end_of_the_app(void)
{
	zzapp_rec_t *rec;
	const struct pal_req *held;
	uint8_t snap[64];
	unsigned reqs;

	pal_world(1);
	rec = APP(0U).rec;
	EXPECT(pal_set(0U, 16U, 1U) == AAR_OK, "palette before the end");
	pal_settle();
	EXPECT(pal_reqs == 1U, "request in flight");
	held = pal_req_at(0U);
	memcpy(snap, held->buf, sizeof(snap));
	EXPECT(pal_set(40U, 4U, 2U) == AAR_OK, "palette waiting at the end");
	zzapp_services_on_app_end();
	reqs = pal_reqs;
	pal_settle();
	EXPECT(pal_reqs == reqs, "no palette request after the app ended");
	EXPECT(memcmp(held->buf, snap, sizeof(snap)) == 0,
	       "the buffer stays untouched after the end");
	pal_complete(0U);
	pal_settle();
	(void)zzapp_xport_pump(rec);
	EXPECT(zzapp_xport_stats(rec)->late_dropped >= 1U,
	       "the late answer is dropped by id");
	pal_done();
}

/* The checks of the call are unchanged: a bad range, a missing table, a
 * display that is not CLUT8, a wrong surface id, and the partial updates at
 * both ends of the table. */
static void test_palette_validation_and_partial_updates(void)
{
	uint32_t rgb[2] = { 0x00010203U, 0x00040506U };
	aar_surface surface;

	pal_world(0);
	EXPECT(zzapp_svc_display_set_palette(1U, 0U, 0U, rgb) == AAR_E_INVAL,
	       "count 0");
	EXPECT(zzapp_svc_display_set_palette(1U, 256U, 1U, rgb) == AAR_E_INVAL,
	       "first 256");
	EXPECT(zzapp_svc_display_set_palette(1U, 255U, 2U, rgb) == AAR_E_INVAL,
	       "range past 256");
	EXPECT(zzapp_svc_display_set_palette(1U, 0U, 257U, rgb) == AAR_E_INVAL,
	       "count above 256");
	EXPECT(zzapp_svc_display_set_palette(1U, 0U, 1U, NULL) == AAR_E_INVAL,
	       "no colours");
	EXPECT(zzapp_svc_display_set_palette(2U, 0U, 1U, rgb) == AAR_E_INVAL,
	       "wrong surface id");
	pal_settle();
	EXPECT(pal_reqs == 0U, "refused calls send nothing");
	EXPECT(pal_set(0U, 1U, 9U) == AAR_OK && pal_set(255U, 1U, 9U) == AAR_OK,
	       "entries 0 and 255 alone");
	pal_settle();
	pal_client_matches_model("partial updates at both ends");
	EXPECT(zzapp_svc_display_close(1U) == AAR_OK, "display close");
	EXPECT(zzapp_svc_display_set_palette(1U, 0U, 1U, rgb) == AAR_E_INVAL,
	       "no display");
	client_display(&CL(0), FRONT, BACK, 0U, 0U);
	EXPECT(open_display(0U, 320U, 200U, AAR_FMT_RGB565, 0U, &surface) == AAR_OK,
	       "RGB565 display open");
	EXPECT(zzapp_svc_display_set_palette(1U, 0U, 1U, rgb) == AAR_E_INVAL,
	       "display is not CLUT8");
	pal_done();
}

/* Card-owned LATEST flips never reach the client. */
static void test_display_latest_flip_posts_nothing(void)
{
	aar_surface surface;
	uint32_t scanout;
	uint32_t flips;

	world_loaded(ZZAPP_CLIENT_LATEST);
	client_display(&CL(0), FRONT, BACK, SPARE, AAR_DISPCAP_LATEST);
	EXPECT(open_display(0U, 320U, 200U, AAR_FMT_RGB565, AAR_DISPLAY_LATEST, &surface) == AAR_OK,
	       "display latest open");
	EXPECT(zzapp_display_latest() == 1, "latest mode active");
	EXPECT(zzapp_services_display_mailbox_begin(0x00230000U, &scanout) == 0,
	       "display mailbox rejects buffer gap");
	flips = mock_client_count(&CL(0), AAR_OP_DISPLAY_FLIP);
	surface.id = 1U;
	EXPECT(zzapp_svc_display_flip_async(surface.id) == AAR_OK &&
	       zzapp_svc_display_refresh(&surface) == AAR_OK &&
	       PHYS(surface.back) == SPARE, "display mailbox first queue");
	EXPECT(zzapp_svc_display_flip_async(surface.id) == AAR_OK &&
	       zzapp_svc_display_refresh(&surface) == AAR_OK &&
	       PHYS(surface.back) == BACK && zzapp_svc_display_latest_dropped() == 1U,
	       "display mailbox replace");
	EXPECT(zzapp_services_display_mailbox_begin(FRONT, &scanout) && scanout == SPARE,
	       "display mailbox select latest");
	zzapp_services_display_mailbox_end(1);
	EXPECT(zzapp_svc_display_refresh(&surface) == AAR_OK &&
	       PHYS(surface.front) == SPARE && PHYS(surface.back) == BACK,
	       "display mailbox latch reuse");
	EXPECT(zzapp_svc_display_flip_async(surface.id) == AAR_OK, "display mailbox second queue");
	EXPECT(zzapp_services_display_mailbox_begin(0x00300000U, &scanout) == 0,
	       "display mailbox hidden screen");
	EXPECT(zzapp_svc_display_refresh(&surface) == AAR_OK && PHYS(surface.back) == FRONT,
	       "display mailbox hidden ownership");
	EXPECT(mock_client_count(&CL(0), AAR_OP_DISPLAY_FLIP) == flips,
	       "no flip request reached the client");
	display_mailbox_lock_acquire();
	zzapp_services_reset_core1_lock();
	if (!zzapp_services_display_mailbox_begin(FRONT, &scanout))
		fail("display mailbox restart unlock");
	else
		zzapp_services_display_mailbox_end(0);

	/* A client that did not ask for LATEST gets the plain path. */
	world_loaded(0U);
	client_display(&CL(0), FRONT, BACK, SPARE, 0U);
	EXPECT(open_display(0U, 320U, 200U, AAR_FMT_RGB565, 0U, &surface) == AAR_OK &&
	       zzapp_display_latest() == 0, "display mailbox fallback");
}

static void test_direct_mailbox_lease(void)
{
	const uint32_t bytes = 320U * 200U * 2U;
	uint32_t pages[3], surface_bytes, scanout, back, spare;
	aar_surface surface;
	uint32_t closes;

	world_loaded(ZZAPP_CLIENT_LATEST);
	client_display(&CL(0), FRONT, BACK, SPARE, AAR_DISPCAP_LATEST);
	EXPECT(open_display(0U, 320U, 200U, AAR_FMT_RGB565, AAR_DISPLAY_LATEST,
			    &surface) == AAR_OK, "open direct mailbox");
	closes = mock_client_count(&CL(0), AAR_OP_DISPLAY_CLOSE);
	EXPECT(zzapp_display_mailbox_buffers(pages, &surface_bytes) == AAR_OK &&
	       pages[0] == FRONT && pages[1] == BACK && pages[2] == SPARE &&
	       surface_bytes == bytes, "mailbox buffer snapshot");
	EXPECT(zzapp_display_mailbox_lease(1) == AAR_OK, "acquire direct lease");
	EXPECT(zzapp_svc_display_flip_async(surface.id) == AAR_E_BUSY &&
	       zzapp_svc_display_close(surface.id) == AAR_E_BUSY &&
	       mock_client_count(&CL(0), AAR_OP_DISPLAY_CLOSE) == closes,
	       "regular flip and close cannot take leased pages");
	zzapp_test_present_flush_count = 0U;
	EXPECT(zzapp_display_mailbox_present(FRONT, ZZAPP_DISPLAY_PRESENT_NO_DIRTY) ==
	       AAR_E_INVAL, "shown buffer is not owned");
	EXPECT(zzapp_display_mailbox_present(SPARE, ZZAPP_DISPLAY_PRESENT_NO_DIRTY) ==
	       AAR_OK && zzapp_test_present_flush_count == 0U,
	       "present the spare without a clean");
	EXPECT(zzapp_display_mailbox_owned(&back, &spare) == AAR_OK &&
	       back == BACK && spare == 0U, "spare submitted, back still owned");
	EXPECT(zzapp_display_mailbox_present(SPARE, 0U) == AAR_E_INVAL &&
	       zzapp_test_present_flush_count == 0U, "pending buffer is not owned");
	EXPECT(zzapp_display_mailbox_present(BACK, 0U) == AAR_OK &&
	       zzapp_test_present_flush_count == 1U &&
	       zzapp_test_present_flushes[0].card == BACK &&
	       zzapp_test_present_flushes[0].bytes == bytes,
	       "CPU fallback cleans exactly the submitted frame");
	EXPECT(zzapp_display_mailbox_owned(&back, &spare) == AAR_OK &&
	       back == SPARE && spare == 0U, "replacement handed back to app");
	EXPECT(zzapp_services_display_mailbox_begin(FRONT, &scanout) && scanout == BACK,
	       "vblank selects the newest completed frame");
	zzapp_services_display_mailbox_end(1);
	EXPECT(zzapp_display_mailbox_owned(&back, &spare) == AAR_OK &&
	       back == SPARE && spare == FRONT, "vblank releases previous shown page");
	zzapp_test_display_invalidate_count = 0U;
	EXPECT(zzapp_display_invalidate(BACK, bytes) == AAR_OK &&
	       zzapp_display_invalidate(BACK - 1U, bytes + 1U) == AAR_E_INVAL &&
	       zzapp_test_display_invalidate_count == 1U &&
	       zzapp_test_display_invalidate_last_address == BACK &&
	       zzapp_test_display_invalidate_last_bytes == bytes,
	       "only a granted range can be invalidated");

	zzapp_test_display_dma_busy = 1;
	EXPECT(zzapp_display_mailbox_lease(0) == AAR_E_BUSY &&
	       !zzapp_display_mailbox_retire_ready(), "still writing, retain lease");
	zzapp_services_on_app_end();
	EXPECT(zzapp_display_latest() && service_proc()->as.alias_count == 3U,
	       "teardown preserves live DMA targets");
	zzapp_test_display_dma_busy = 0;
	EXPECT(zzapp_display_mailbox_lease(0) == AAR_OK &&
	       zzapp_display_mailbox_retire_ready(), "release after engine idle");
	EXPECT(zzapp_svc_display_close(surface.id) == AAR_OK && !zzapp_display_latest(),
	       "close after draining direct GL");
}

static void test_display_mailbox_uses_scanout_owner(void)
{
	uint32_t scanout;

	svc_apps_reset();
	probe_init();
	svc_app_new(0U);
	svc_app_new(1U);
	RT(1).display_mailbox_active = 1;
	/* The vblank walks the records with a bound display. */
	slot_state_lock_acquire();
	set_add(&set_display, PID_OF(1U));
	slot_state_lock_release();
	RT(1).display_mailbox_shown = 0x00200000U;
	RT(1).display_mailbox_pending = 0x00210000U;
	RT(1).display_mailbox_buffers[0] = 0x00200000U;
	RT(1).display_mailbox_buffers[1] = 0x00210000U;
	RT(1).display_mailbox_buffers[2] = 0x00220000U;
	RT(1).display_mailbox_bytes = 0x10000U;
	zzapp_services_test_set_core(0);
	svc_select(0U);
	EXPECT(zzapp_services_display_mailbox_begin(0x00200000U, &scanout) &&
	       scanout == 0x00210000U, "display mailbox scanout owner");
	zzapp_services_display_mailbox_end(1);
	EXPECT(RT(1).display_mailbox_shown == 0x00210000U &&
	       RT(1).display_mailbox_pending == 0U, "display mailbox owner completion");
	zzapp_services_test_set_core(1);
}

/* Every grant is an alias extent of the app's space; a revoke removes exactly it. */
static void test_returned_buffers_are_user_granted(void)
{
	aar_surface surface;
	aar_audio audio;
	uint32_t revokes;

	world_loaded(0U);
	client_display(&CL(0), FRONT, BACK, 0U, 0U);
	EXPECT(open_display(0U, 320U, 200U, AAR_FMT_RGB565, 0U, &surface) == AAR_OK,
	       "surface open for grant");
	EXPECT(service_proc()->as.alias_count == 2U, "surface buffers were not granted");
	EXPECT(open_display(0U, 320U, 200U, AAR_FMT_RGB565, 0U, &surface) == AAR_E_BUSY,
	       "second surface open was accepted");
	EXPECT(service_proc()->as.alias_count == 2U, "second surface open created aliases");
	EXPECT(zzapp_svc_display_close(1U) == AAR_OK, "surface close for revoke");
	EXPECT(service_proc()->as.alias_count == 0U, "surface close left a display alias");
	revokes = service_proc()->as.alias_count;
	EXPECT(zzapp_svc_audio_open(22050U, 2U, &audio) == AAR_OK, "audio open for grant");
	EXPECT(service_proc()->as.alias_count == revokes + 1U, "audio buffer was not granted");
	EXPECT(zzapp_svc_audio_close() == AAR_OK, "audio close for revoke");
	EXPECT(service_proc()->as.alias_count == revokes, "audio buffer was not revoked");
}

/* The client scribbles on every platform-written word of its control block:
 * the app's display keeps the buffers the client named in its completion. */
static void test_display_state_ignores_control_block_tampering(void)
{
	aar_surface surface;
	uint32_t front;
	uint32_t back;
	uint32_t off;

	world_loaded(0U);
	client_display(&CL(0), FRONT, BACK, 0U, 0U);
	EXPECT(open_display(0U, 320U, 200U, AAR_FMT_RGB565, 0U, &surface) == AAR_OK,
	       "display open for private state");
	for (off = AAR_CB_STATE; off <= AAR_CB_MSG_CONSUMED; off += 4U)
		mock_client_cb_put(&CL(0), off, 0x01000000U);
	mock_client_cb_put(&CL(0), AAR_CB_SHARED_USED_LO, 0x01010000U);
	EXPECT(zzapp_display_bound(NULL, NULL, NULL, NULL, &front, &back) == AAR_OK &&
	       front == FRONT && back == BACK, "display trusted the client's addresses");
}

/* ---- Lifecycle ---------------------------------------------------------------------------- */

static unsigned g_begin_calls;

static void loader_begin_counts(void)
{
	g_begin_calls++;
}

/* One word of the LIST reply for the record at `index`. */
static uint32_t list_word(uint32_t index, uint32_t off)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };

	if (mock_client_list(&g_probe, index, reply) != SDK_STATUS_OK)
		return 0xdeadbeefU;
	return zzapp_get_be32(reply + off);
}

static void test_claim_publishes_the_pair(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t rl = 0U;
	uint32_t free_before;
	uint32_t heap_before;

	svc_apps_reset();
	probe_init();
	zzapp_services_test_set_core(0);
	free_before = query_word(ZZAPP_QUERY_RECORDS_FREE);
	heap_before = query_word(ZZAPP_QUERY_HOSTHEAP_FREE);
	EXPECT(mock_client_call(&g_probe, ZZAPP_OP_CLAIM, NULL, 0U, reply, &rl) ==
	       SDK_STATUS_OK && rl == ZZAPP_CLAIM_REPLY_BYTES, "claim");
	EXPECT(mock_client_attach_reply(&g_probe, reply, rl) == AAR_OK,
	       "the claim reply names a control block that checks");
	EXPECT(zzapp_get_be32(reply + ZZAPP_CLAIM_CB_BYTES) == AAR_PAIR_BYTES,
	       "contract size of the block");
	EXPECT(mock_client_cb_get(&g_probe, AAR_CB_STATE) == AAR_STATE_CLAIMED,
	       "claimed state in the control block");
	EXPECT(mock_client_cb_get(&g_probe, AAR_CB_HANDLE) == g_probe.handle, "handle in the block");
	EXPECT(query_word(ZZAPP_QUERY_RECORDS_FREE) == free_before - 1U, "a record is taken");
	EXPECT(query_word(ZZAPP_QUERY_HOSTHEAP_FREE) < heap_before, "the pair is taken from the heap");
	EXPECT(mock_client_release(&g_probe) == SDK_STATUS_OK, "release");
	EXPECT(query_word(ZZAPP_QUERY_RECORDS_FREE) == free_before, "release returns the record");
	EXPECT(query_word(ZZAPP_QUERY_HOSTHEAP_FREE) == heap_before, "release returns the pair");
	EXPECT(mock_client_release(&g_probe) == SDK_STATUS_BAD_HANDLE, "a second release is stale");
}

static void test_load_begin_reply_and_limits(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t rl = 0U;
	void *buf;

	world_claimed();
	give_space(0U);
	mock_client_set_limits(&CL(0), 8U, 20U, 3U);
	service_test_loader_begin_hook = loader_begin_succeeds;
	load_begin_run(0U, ZZAPP_CLIENT_KNOWN, reply, &rl);
	service_test_loader_begin_hook = NULL;
	{
		uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES];
		uint16_t status = 0U, plen = 0U;

		EXPECT(zzapp_mailbox_harvest(NULL, NULL, NULL, &status, payload, &plen) &&
		       status == SDK_STATUS_OK && plen == ZZAPP_LOAD_BEGIN_REPLY_BYTES,
		       "load begin reply is 16 bytes");
		zzapp_mailbox_harvest_ack();
		EXPECT(zzapp_get_be32(payload + ZZAPP_LOAD_REPLY_SIZE) == ZZAPP_LOAD_BUFFER_BYTES,
		       "load buffer size");
		EXPECT(zzapp_get_be32(payload + ZZAPP_LOAD_REPLY_CHUNK_MAX) == ZZAPP_CHUNK_MAX,
		       "chunk maximum");
		EXPECT(zzapp_get_be32(payload + ZZAPP_LOAD_REPLY_ARGV_OFF) == ZZAPP_LOAD_ARGV_OFF,
		       "argv offset");
		buf = mock_heap_map(zzapp_get_be32(payload + ZZAPP_LOAD_REPLY_TOKEN),
				    ZZAPP_LOAD_BUFFER_BYTES);
		EXPECT(buf != NULL, "load buffer token maps");
	}
	EXPECT(CBSTATE(0) == AAR_STATE_LOADING, "loading state in the control block");
	EXPECT(zzapp_services_memory_limit() == 8U * 1024U * 1024U, "private limit from the control block");
	EXPECT(CB(0, AAR_CB_SHARED_USED_LO) >= ZZAPP_LOAD_BUFFER_BYTES + 0x4000U,
	       "the load buffer is counted as shared");
}

/* A LOAD_CHUNK reads the load buffer the client wrote. */
static int g_chunk_ok;
static char g_chunk_data[8];

static int chunk_records(uint32_t file_off, const void *src, uint32_t len)
{
	g_chunk_ok = file_off == 0U && len == 5U;
	memcpy(g_chunk_data, src, len);
	return AAR_OK;
}

static void test_chunk_reads_the_load_buffer(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t rl = 0U;
	uint16_t status = 0U, plen = 0U;
	uint8_t *buf;

	world_claimed();
	give_space(0U);
	service_test_loader_begin_hook = loader_begin_succeeds;
	load_begin_run(0U, 0U, reply, &rl);
	service_test_loader_begin_hook = NULL;
	EXPECT(zzapp_mailbox_harvest(NULL, NULL, NULL, &status, payload, &plen), "begin done");
	zzapp_mailbox_harvest_ack();
	buf = mock_heap_map(zzapp_get_be32(payload + ZZAPP_LOAD_REPLY_TOKEN), ZZAPP_LOAD_BUFFER_BYTES);
	if (!buf) {
		fail("load buffer");
		return;
	}
	memcpy(buf, "hello", 5U);
	zzapp_build_load_chunk_request(reply, CL(0).handle, 0U, 5U);
	service_test_loader_chunk_hook = chunk_records;
	g_chunk_ok = 0;
	zzapp_services_test_set_core(0);
	EXPECT(mock_client_call(&CL(0), ZZAPP_OP_LOAD_CHUNK, reply, ZZAPP_MAILBOX_PAYLOAD_BYTES,
				NULL, NULL) == SDK_STATUS_QUEUED, "chunk queued");
	zzapp_services_test_set_core(1);
	zzapp_services_select_pid(PID_OF(0U));
	if (!start_posted_deferred(0U))
		fail("chunk posted");
	run_deferred(service_rec());
	service_test_loader_chunk_hook = NULL;
	EXPECT(g_chunk_ok && memcmp(g_chunk_data, "hello", 5U) == 0, "chunk bytes from the load buffer");
	zzapp_mailbox_harvest_ack();
}

/* The client's limits are read at LOAD_BEGIN only. */
static void test_deferred_waits_for_system_thread(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t rl = 0U;

	world_claimed();
	zzapp_services_test_set_core(0);
	EXPECT(mock_client_load_begin(&CL(0), 0U, reply, &rl) == SDK_STATUS_QUEUED,
	       "deferred request queued");
	zzapp_services_test_set_core(1);
	service_test_current_thread = (int)PID_OF(0U);
	zzapp_services_poll();
	EXPECT(__atomic_load_n(&DEF(0).state, __ATOMIC_ACQUIRE) == DEFER_POSTED,
	       "app thread ran deferred request");
	service_test_current_thread = -1;
	zzapp_services_poll();
	EXPECT(__atomic_load_n(&DEF(0).state, __ATOMIC_ACQUIRE) == DEFER_DONE,
	       "system thread did not run deferred request");
	(void)zzapp_loader_abort();
}

static void test_deferred_requests_queue_independently(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t rl = 0U;

	svc_apps_reset();
	probe_init();
	zzapp_services_test_set_core(0);
	svc_app_claim(0U);
	svc_app_claim(1U);
	EXPECT(mock_client_load_begin(&CL(0), 0U, reply, &rl) == SDK_STATUS_QUEUED,
	       "first deferred request");
	EXPECT(mock_client_load_begin(&CL(1), 0U, reply, &rl) == SDK_STATUS_QUEUED,
	       "second deferred request");
	EXPECT(mock_client_load_begin(&CL(1), 0U, reply, &rl) == SDK_STATUS_BUSY,
	       "a second op on the same record is busy");
}

/* A request that names `handle` after its claim was revoked. */
static uint16_t stale_chunk(uint32_t handle, uint32_t id)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t rep[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t rl = 0U;

	zzapp_build_load_chunk_request(req, handle, 0U, 1U);
	return zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_CHUNK, req, sizeof(req), rep,
				      &rl, 1, id, 0U);
}

/* A STOP that names `handle` after its claim was revoked: the one stale
 * request, with RELEASE, that lets a quarantined shell go. */
static uint16_t stale_stop(uint32_t handle)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t rep[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t rl = 0U;

	zzapp_build_handle(req, handle);
	return zzapp_mailbox_dispatch(ZZAPP_OP_STOP, req, sizeof(req), rep, &rl, 1,
				      0U, 0U);
}

static void test_failed_starts_release_admission(void)
{
	uint32_t free_before;
	uint32_t heap_before;
	uint32_t tables_before;
	unsigned i;

	svc_apps_reset();
	probe_init();
	zzapp_services_test_set_core(0);
	free_before = query_word(ZZAPP_QUERY_RECORDS_FREE);
	heap_before = query_word(ZZAPP_QUERY_HOSTHEAP_FREE);
	tables_before = query_word(ZZAPP_QUERY_TABLES_FREE);
	/* More cycles than there are records: nothing leaks. */
	for (i = 0U; i < free_before + 8U; i++) {
		if (!svc_app_claim(0U)) {
			fail("claim before failed start");
			break;
		}
		zzapp_services_on_app_end();
		(void)zzapp_rec_retire(zzapp_services_current_pid());
		EXPECT(mock_client_release(&CL(0)) == SDK_STATUS_OK, "release after a failed start");
		memset(&g_app[0], 0, sizeof(g_app[0]));
		mock_client_registry_clear();
	}
	EXPECT(query_word(ZZAPP_QUERY_RECORDS_FREE) == free_before, "failed starts leaked a record");
	EXPECT(query_word(ZZAPP_QUERY_HOSTHEAP_FREE) == heap_before, "failed starts leaked heap");
	EXPECT(query_word(ZZAPP_QUERY_TABLES_FREE) == tables_before, "failed starts leaked tables");
}

/* ---- Lease, quarantine ------------------------------------------------------------------------- */

static unsigned g_abort_base;

static void expire_running_load(void)
{
	zzapp_services_expire_claims(ZZAPP_LOAD_LEASE_US);
}

static uint16_t g_stop_status;
static uint32_t g_stop_state;
static uint16_t g_claim_status;

/* The client stops the claim while core 1 is completing its load. */
static void stop_completing_load(void)
{
	g_stop_status = mock_client_stop(&CL(0));
	zzapp_services_core0_poll();
	g_stop_state = CBSTATE(0);
}

static void test_stop_cancels_completing_load(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t rl = 0U;
	uint32_t free_before;

	world_claimed();
	zzapp_services_test_set_core(0);
	free_before = query_word(ZZAPP_QUERY_RECORDS_FREE);
	EXPECT(mock_client_load_begin(&CL(0), 0U, reply, &rl) == SDK_STATUS_QUEUED &&
	       start_posted_deferred(0U), "queue load before completing stop");
	service_test_loader_begin_hook = loader_begin_succeeds;
	service_test_deferred_completing_hook = stop_completing_load;
	g_abort_base = service_test_loader_abort_count;
	zzapp_services_test_set_core(1);
	zzapp_services_select_pid(PID_OF(0U));
	run_deferred(service_rec());
	service_test_deferred_completing_hook = NULL;
	service_test_loader_begin_hook = NULL;
	EXPECT(g_stop_status == SDK_STATUS_OK, "stop did not take completing load");
	EXPECT(g_stop_state == AAR_STATE_STOPPING, "audio poll released completing load");
	EXPECT(CBSTATE(0) == AAR_STATE_IDLE &&
	       __atomic_load_n(&DEF(0).state, __ATOMIC_ACQUIRE) == DEFER_IDLE &&
	       service_test_loader_abort_count == g_abort_base + 1U,
	       "completing stop was published after cancellation");
	/* The shell is the client's to release: nothing else can take its record. */
	g_claim_status = mock_client_release(&CL(0));
	EXPECT(g_claim_status == SDK_STATUS_OK &&
	       query_word(ZZAPP_QUERY_RECORDS_FREE) == free_before + 1U,
	       "release after completing stop");
}

/* A claim that sat idle past its 30 s lease loses the handle: the client's next
 * request is stale, and a request that names the old handle releases the
 * shell. */
static void test_claim_expiry_revokes_the_handle(void)
{
	uint32_t old_handle;
	uint32_t free_before;

	world_claimed();
	zzapp_services_test_set_core(0);
	free_before = query_word(ZZAPP_QUERY_RECORDS_FREE);
	old_handle = CL(0).handle;
	g_now = ZZAPP_CLAIM_LEASE_US - 1U;
	zzapp_services_expire_claims(g_now);
	EXPECT(zzapp_rec_by_handle(old_handle) != NULL, "claim expired early");
	g_now = ZZAPP_CLAIM_LEASE_US;
	zzapp_services_expire_claims(g_now);
	EXPECT(CB(0, AAR_CB_STATE) == AAR_STATE_IDLE &&
	       CB(0, AAR_CB_END_REASON) == AAR_END_LEASE, "lease loss in the control block");
	/* The shell is held under a new handle: the old one is stale. */
	EXPECT(zzapp_xport_resolve(old_handle, NULL) == NULL, "old handle resolves to nothing");
	EXPECT(query_word(ZZAPP_QUERY_RECORDS_FREE) == free_before, "the shell holds its record");
	/* The stale contact is refused and releases the shell. */
	EXPECT(mock_client_stop(&CL(0)) == SDK_STATUS_BAD_HANDLE, "stale contact");
	EXPECT(query_word(ZZAPP_QUERY_RECORDS_FREE) == free_before + 1U,
	       "the stale contact did not release the shell");
}

static void test_claim_lease_renews_with_the_counter(void)
{
	world_claimed();
	zzapp_services_test_set_core(0);
	g_now = ZZAPP_CLAIM_LEASE_US;
	mock_client_renew_lease(&CL(0));
	zzapp_services_expire_claims(g_now);
	EXPECT(CBSTATE(0) == AAR_STATE_CLAIMED, "lease counter did not renew the claim");
	g_now += ZZAPP_CLAIM_LEASE_US - 1U;
	zzapp_services_expire_claims(g_now);
	EXPECT(CBSTATE(0) == AAR_STATE_CLAIMED, "renewed claim expired early");
	g_now += 1U;
	zzapp_services_expire_claims(g_now);
	EXPECT(CBSTATE(0) == AAR_STATE_IDLE, "renewed claim did not expire");
}

static void start_loading(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len = 0U;

	world_claimed();
	give_space(0U);
	service_test_loader_begin_hook = loader_begin_succeeds;
	load_begin_run(0U, 0U, reply, &reply_len);
	harvest_ack();
	service_test_loader_begin_hook = NULL;
	EXPECT(CBSTATE(0) == AAR_STATE_LOADING, "enter loading deadline");
	g_abort_base = service_test_loader_abort_count;
}

static void test_abandoned_loading_is_quarantined(void)
{
	uint32_t old_handle;
	uint32_t free_before;
	uint32_t current;

	start_loading();
	zzapp_services_test_set_core(0);
	free_before = query_word(ZZAPP_QUERY_RECORDS_FREE);
	old_handle = CL(0).handle;
	g_now = ZZAPP_LOAD_LEASE_US;
	zzapp_services_expire_claims(g_now);
	EXPECT(CBSTATE(0) == AAR_STATE_STOPPING &&
	       service_test_loader_abort_count == g_abort_base, "expired load was not quarantined");
	/* LIST reports the shell under its current handle. */
	EXPECT(list_word(PID_OF(0U), ZZAPP_LIST_STATE) == AAR_STATE_STOPPING, "list state");
	current = list_word(PID_OF(0U), ZZAPP_LIST_HANDLE);
	EXPECT(current != 0U && current != old_handle, "the shell is listed under a new handle");
	{
		uint8_t r[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };

		(void)mock_client_list(&g_probe, PID_OF(0U), r);
		EXPECT(r[ZZAPP_LIST_REASONS + 1U] == ZZAPP_END_QUARANTINED, "status reports quarantine");
	}
	EXPECT(stale_chunk(old_handle, 4U) == SDK_STATUS_BAD_HANDLE,
	       "stale launcher contact did not receive the terminal error");
	EXPECT(query_word(ZZAPP_QUERY_RECORDS_FREE) == free_before,
	       "stale contact released a live quarantined shell");
	/* The cancelled loader finishes; the stale contact then releases. */
	(void)zzapp_loader_abort();
	(void)zzapp_rec_retire(PID_OF(0U));
	EXPECT(stale_chunk(old_handle, 5U) == SDK_STATUS_BAD_HANDLE, "stale contact after teardown");
	EXPECT(query_word(ZZAPP_QUERY_RECORDS_FREE) == free_before,
	       "a stale deferred call released the shell");
	EXPECT(stale_stop(old_handle) == SDK_STATUS_BAD_HANDLE, "stale stop after teardown");
	EXPECT(query_word(ZZAPP_QUERY_RECORDS_FREE) == free_before + 1U,
	       "stale stop did not release after loader teardown");
}

static void test_explicit_release_of_a_quarantined_shell_and_reset(void)
{
	uint32_t free_start;
	uint32_t current;

	start_loading();
	zzapp_services_test_set_core(0);
	free_start = query_word(ZZAPP_QUERY_RECORDS_FREE) + 1U;
	g_now = ZZAPP_LOAD_LEASE_US;
	zzapp_services_expire_claims(g_now);
	(void)zzapp_loader_abort();
	(void)zzapp_rec_retire(PID_OF(0U));
	current = list_word(PID_OF(0U), ZZAPP_LIST_HANDLE);
	EXPECT(current != 0U, "quarantined shell is listed");
	CL(0).handle = current;
	EXPECT(mock_client_release(&CL(0)) == SDK_STATUS_OK, "explicit release did not free quarantine");
	EXPECT(mock_client_release(&CL(0)) == SDK_STATUS_BAD_HANDLE,
	       "explicit release accepted a released handle");
	EXPECT(query_word(ZZAPP_QUERY_RECORDS_FREE) == free_start, "explicit release did not restore admission");

	start_loading();
	g_now = ZZAPP_LOAD_LEASE_US;
	zzapp_services_expire_claims(g_now);
	(void)zzapp_loader_abort();
	(void)zzapp_rec_retire(PID_OF(0U));
	zzapp_services_on_reset();
	EXPECT(list_word(PID_OF(0U), ZZAPP_LIST_HANDLE) == 0U,
	       "reset did not clear quarantine");
}

static void test_loading_chunk_renews_deadline(void)
{
	uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };

	start_loading();
	g_now = ZZAPP_LOAD_LEASE_US - 1U;
	zzapp_build_load_chunk_request(payload, CL(0).handle, 0U, 1U);
	zzapp_services_test_set_core(0);
	service_test_loader_chunk_hook = loader_chunk_succeeds;
	EXPECT(mock_client_call(&CL(0), ZZAPP_OP_LOAD_CHUNK, payload, ZZAPP_MAILBOX_PAYLOAD_BYTES,
				NULL, NULL) == SDK_STATUS_QUEUED && start_posted_deferred(0U),
	       "queue slow loading chunk");
	zzapp_services_test_set_core(1);
	zzapp_services_select_pid(PID_OF(0U));
	run_deferred(service_rec());
	service_test_loader_chunk_hook = NULL;
	harvest_ack();
	g_now += ZZAPP_LOAD_LEASE_US - 1U;
	zzapp_services_expire_claims(g_now);
	EXPECT(CBSTATE(0) == AAR_STATE_LOADING && service_test_loader_abort_count == g_abort_base,
	       "slow loading chunk did not renew deadline");
	g_now++;
	zzapp_services_expire_claims(g_now);
	EXPECT(CBSTATE(0) == AAR_STATE_STOPPING && service_test_loader_abort_count == g_abort_base,
	       "renewed loading deadline was not quarantined");
}

static void test_loading_lease_counter_renews_deadline(void)
{
	start_loading();
	g_now = ZZAPP_LOAD_LEASE_US;
	mock_client_renew_lease(&CL(0));
	zzapp_services_expire_claims(g_now);
	EXPECT(CBSTATE(0) == AAR_STATE_LOADING, "lease counter did not renew loading deadline");
	g_now += ZZAPP_LOAD_LEASE_US - 1U;
	zzapp_services_expire_claims(g_now);
	EXPECT(CBSTATE(0) == AAR_STATE_LOADING, "active slow load was reclaimed");
	g_now++;
	zzapp_services_expire_claims(g_now);
	EXPECT(CBSTATE(0) == AAR_STATE_STOPPING, "renewed loading deadline did not expire");
}

static void test_claim_expiry_cancels_running_load(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t rl = 0U;
	uint32_t free_before;

	world_claimed();
	zzapp_services_test_set_core(0);
	free_before = query_word(ZZAPP_QUERY_RECORDS_FREE);
	EXPECT(mock_client_load_begin(&CL(0), 0U, reply, &rl) == SDK_STATUS_QUEUED &&
	       start_posted_deferred(0U), "queue load before expiry");
	g_abort_base = service_test_loader_abort_count;
	service_test_deferred_completion_hook = expire_running_load;
	service_test_loader_begin_hook = loader_begin_succeeds;
	zzapp_services_test_set_core(1);
	zzapp_services_select_pid(PID_OF(0U));
	run_deferred(service_rec());
	service_test_deferred_completion_hook = NULL;
	service_test_loader_begin_hook = NULL;
	/* The cancelled load ends through the retire path: the shell reads
	 * IDLE with the end reason in the control block (it read STOPPING while
	 * a window held the state), after the loader dropped its arena. */
	EXPECT(CBSTATE(0) == AAR_STATE_IDLE &&
	       CB(0, AAR_CB_END_REASON) == AAR_END_QUARANTINED &&
	       __atomic_load_n(&DEF(0).state, __ATOMIC_ACQUIRE) == DEFER_IDLE &&
	       service_test_loader_abort_count == g_abort_base + 1U,
	       "expired running load was not quarantined");
	/* No replacement can take the shell's record until it is released. */
	EXPECT(query_word(ZZAPP_QUERY_RECORDS_FREE) == free_before, "the shell still holds its record");
	zzapp_build_load_chunk_request(reply, CL(0).handle, 0U, 1U);
	EXPECT(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_CHUNK, reply, ZZAPP_MAILBOX_PAYLOAD_BYTES, reply, &rl, 1,
				      3U, 0U) == SDK_STATUS_BAD_HANDLE,
	       "stale running loader did not release quarantine");
	/* A new claim is admitted and its load is not affected by the expiry. */
	EXPECT(svc_app_claim(1U) != NULL, "claim after running expiry");
	zzapp_services_test_set_core(0);
	EXPECT(mock_client_load_begin(&CL(1), 0U, reply, &rl) == SDK_STATUS_QUEUED &&
	       CBSTATE(1) == AAR_STATE_CLAIMED && __atomic_load_n(&DEF(1).state, __ATOMIC_ACQUIRE) == DEFER_POSTED,
	       "replacement load was affected by expiry");
}

static void test_preload_cancellation_keeps_the_loader_consistent(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t rl = 0U;

	world_claimed();
	zzapp_services_test_set_core(0);
	EXPECT(mock_client_load_begin(&CL(0), 0U, reply, &rl) == SDK_STATUS_QUEUED &&
	       start_posted_deferred(0U), "queue load before pre-load cancellation");
	g_abort_base = service_test_loader_abort_count;
	service_test_loader_set_generation_hook = expire_running_load;
	zzapp_services_test_set_core(1);
	zzapp_services_select_pid(PID_OF(0U));
	run_deferred(service_rec());
	service_test_loader_set_generation_hook = NULL;
	EXPECT(CBSTATE(0) == AAR_STATE_IDLE &&
	       CB(0, AAR_CB_END_REASON) == AAR_END_QUARANTINED &&
	       __atomic_load_n(&DEF(0).state, __ATOMIC_ACQUIRE) == DEFER_IDLE &&
	       service_test_loader_abort_count == g_abort_base + 1U,
	       "pre-load cancellation did not quarantine lifecycle state");
}

static unsigned g_race_result;
static uint16_t g_race_status;

static void harvest_between_reply_and_done(void)
{
	g_race_result = (unsigned)zzapp_mailbox_harvest(NULL, NULL, NULL, &g_race_status, NULL, NULL);
}

static void test_harvest_racing_completion_reads_reply(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t rl = 0U;
	uint16_t status;

	world_claimed();
	zzapp_services_test_set_core(0);
	EXPECT(mock_client_load_begin(&CL(0), 0U, reply, &rl) == SDK_STATUS_QUEUED &&
	       start_posted_deferred(0U), "queue load before harvest race");
	service_test_deferred_reply_hook = harvest_between_reply_and_done;
	zzapp_services_test_set_core(1);
	zzapp_services_select_pid(PID_OF(0U));
	run_deferred(service_rec());
	service_test_deferred_reply_hook = NULL;
	EXPECT(g_race_result == 0U, "harvest posted completion before DONE");
	EXPECT(zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, NULL) &&
	       status == SDK_STATUS_UNSUPPORTED, "harvest posted stale completion status");
	zzapp_mailbox_harvest_ack();
}

static uint64_t replacement_wait_time(void)
{
	return 1U;
}

static void test_new_claim_does_not_inherit_wait(void)
{
	zzapp_rec_t *rec;

	world_claimed();
	current_runtime()->wait_kind = ZZAPP_WAIT_FILE;
	current_runtime()->wait_value = 0U;
	current_runtime()->wait_deadline = 1U;
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(zzapp_services_current_pid());
	EXPECT(mock_client_release(&CL(0)) == SDK_STATUS_OK, "release of the parked claim");
	rec = svc_app_claim(1U);
	EXPECT(rec != NULL, "claim after parked teardown");
	if (!rec)
		return;
	TH(1).state = ZZAPP_THREAD_SLEEPING;
	zzapp_services_test_set_time(replacement_wait_time);
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	zzapp_services_test_set_time(NULL);
	EXPECT(TH(1).state == ZZAPP_THREAD_SLEEPING, "old file wait woke replacement sleep");
}

/* STOP of a claim that never loaded ends it; the shell is released by the
 * client, not by the next claim. */
static void test_stop_ends_a_claimed_record(void)
{
	uint32_t free_before;

	world_claimed();
	free_before = query_word(ZZAPP_QUERY_RECORDS_FREE);
	EXPECT(mock_client_stop(&CL(0)) == SDK_STATUS_OK, "stop claimed record");
	EXPECT(mock_client_release(&CL(0)) == SDK_STATUS_OK, "release after stop");
	EXPECT(query_word(ZZAPP_QUERY_RECORDS_FREE) == free_before + 1U, "claimed record was not released");
}

static void test_stop_cancels_queued_claim_load(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t rl = 0U;

	world_claimed();
	EXPECT(mock_client_load_begin(&CL(0), 0U, reply, &rl) == SDK_STATUS_QUEUED, "queue claimed load");
	EXPECT(mock_client_stop(&CL(0)) == SDK_STATUS_OK, "stop queued claimed load");
	g_begin_calls = 0U;
	service_test_loader_begin_hook = loader_begin_counts;
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	zzapp_services_poll();
	service_test_loader_begin_hook = NULL;
	EXPECT(g_begin_calls == 0U, "a cancelled load entered the loader");
	EXPECT(CBSTATE(0) != AAR_STATE_LOADING, "a cancelled load published LOADING");
	(void)mock_client_release(&CL(0));
}

/* The argv table sits in the load buffer; START carries only argc; the list
 * name is the file part of argv[0]. */
static void test_list_name_uses_start_argv(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t rl = 0U;
	uint16_t plen = 0U, status = 0U;
	uint8_t *buf;
	uint8_t list[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };

	world_claimed();
	give_space(0U);
	service_test_loader_begin_hook = loader_begin_succeeds;
	load_begin_run(0U, 0U, reply, &rl);
	service_test_loader_begin_hook = NULL;
	EXPECT(zzapp_mailbox_harvest(NULL, NULL, NULL, &status, payload, &plen), "begin done");
	zzapp_mailbox_harvest_ack();
	buf = mock_heap_map(zzapp_get_be32(payload + ZZAPP_LOAD_REPLY_TOKEN), ZZAPP_LOAD_BUFFER_BYTES);
	if (!buf) {
		fail("load buffer for argv");
		return;
	}
	zzapp_put_be32(buf + ZZAPP_LOAD_ARGV_OFF + ZZAPP_ARGV_ARGC_OFF, 1U);
	zzapp_put_be32(buf + ZZAPP_LOAD_ARGV_OFF + ZZAPP_ARGV_OFFS_OFF,
		       ZZAPP_LOAD_ARGV_OFF + ZZAPP_ARGV_STR_OFF);
	memcpy(buf + ZZAPP_LOAD_ARGV_OFF + ZZAPP_ARGV_STR_OFF, "PROGDIR:bin/demo",
	       sizeof("PROGDIR:bin/demo"));
	test_force_state(0U, AAR_STATE_LOADING);
	zzapp_services_test_set_core(0);
	EXPECT(mock_client_start(&CL(0), 1U) == SDK_STATUS_QUEUED, "start queued for list name");
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	EXPECT(zzapp_services_take_start(NULL), "start pending for list name");
	EXPECT(zzapp_services_start_entered(zzapp_services_generation()) == AAR_OK,
	       "start completion for list name");
	zzapp_services_test_set_core(0);
	EXPECT(mock_client_list(&g_probe, PID_OF(0U), list) == SDK_STATUS_OK &&
	       memcmp(list + ZZAPP_LIST_NAME, "demo", sizeof("demo")) == 0, "list name from argv");
}

/* ---- Audio and the passes that walk records --------------------------------------------------- */

static void test_audio_teardown_quiesces_every_app(void)
{
	aar_audio audio;

	svc_apps_reset();
	probe_init();
	svc_app_new(0U);
	svc_app_new(1U);
	zzapp_services_test_set_core(1);
	svc_select(0U);
	EXPECT(zzapp_svc_audio_open(22050U, 2U, &audio) == AAR_OK, "audio open for cross-app teardown");
	zzapp_services_on_app_end();
	svc_select(1U);
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	zzapp_services_test_set_core(0);
	zzapp_services_core0_poll();
	svc_select(0U);
	EXPECT(zzapp_services_test_audio_request() == AUDIO_REQUEST_NONE &&
	       zzapp_services_test_audio_detach_count() == 1U,
	       "audio teardown stranded by another app");
}

static void test_audio_handoff_blocks_release(void)
{
	aar_audio audio;
	void *block;
	uint32_t handed;
	uint32_t free_before;

	svc_apps_reset();
	probe_init();
	zzapp_services_test_set_core(0);
	svc_app_claim(0U);
	give_space(0U);
	free_before = query_word(ZZAPP_QUERY_RECORDS_FREE);
	zzapp_services_test_set_core(1);
	svc_select(0U);
	EXPECT(zzapp_svc_audio_open(22050U, 2U, &audio) == AAR_OK, "audio open before blocked release");
	block = audio_ring_ptr(service_rec());
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(zzapp_services_current_pid());
	handed = zzapp_services_test_audio_request();
	zzapp_services_test_set_core(0);
	EXPECT(mock_client_release(&CL(0)) == SDK_STATUS_OK, "release during the handoff is accepted");
	/* A release during the handoff is accepted, but the record and its pair
	 * stay until core 0 has detached the audio. */
	EXPECT(query_word(ZZAPP_QUERY_RECORDS_FREE) == free_before,
	       "the record was freed during the audio handoff");
	svc_select(0U);
	EXPECT(audio_ring_ptr(service_rec()) == block &&
	       zzapp_services_test_audio_request() == handed &&
	       zzapp_services_test_audio_detach_count() == 0U, "release zeroed live audio during handoff");
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	zzapp_services_test_set_core(0);
	zzapp_services_core0_poll();
	EXPECT(query_word(ZZAPP_QUERY_RECORDS_FREE) == free_before + 1U,
	       "record not freed after the handoff");
}

static void test_cancelled_pending_open_releases_record(void)
{
	zzapp_rec_t *r0;
	uint32_t free_before;

	svc_apps_reset();
	probe_init();
	zzapp_services_test_set_core(0);
	svc_app_claim(0U);
	free_before = query_word(ZZAPP_QUERY_RECORDS_FREE);
	svc_select(0U);
	r0 = service_rec();
	zzapp_services_test_set_audio_request(AUDIO_REQUEST_OPEN);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(zzapp_services_current_pid());
	EXPECT(CBSTATE(0) != AAR_STATE_IDLE, "idle while open was still pending teardown");
	zzapp_services_core0_poll();
	EXPECT(runtime_of(r0)->audio_teardown == 0U && runtime_of(r0)->audio_request == AUDIO_REQUEST_NONE &&
	       CBSTATE(0) == AAR_STATE_IDLE, "cancelled open left the record unreleased");
	EXPECT(mock_client_release(&CL(0)) == SDK_STATUS_OK, "release after cancelled open");
	EXPECT(query_word(ZZAPP_QUERY_RECORDS_FREE) == free_before + 1U, "record after cancelled open release");
}

static void test_reclaim_halted_audio_reclaims_every_app(void)
{
	aar_audio audio[2];
	zzapp_rec_t *rec0;
	zzapp_rec_t *rec1;
	uint32_t pid0;

	svc_apps_reset();
	probe_init();
	zzapp_services_test_set_core(0);
	svc_app_claim(0U);
	give_space(0U);
	svc_app_claim(1U);
	give_space(1U);
	svc_select(0U);
	rec0 = service_rec();
	pid0 = zzapp_services_current_pid();
	zzapp_services_test_set_core(1);
	svc_select(0U);
	EXPECT(zzapp_svc_audio_open(22050U, 2U, &audio[0]) == AAR_OK, "producer 0 open");
	svc_select(1U);
	rec1 = service_rec();
	EXPECT(zzapp_svc_audio_open(22050U, 2U, &audio[1]) == AAR_OK, "producer 1 open");
	svc_select(0U);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(zzapp_services_current_pid());
	svc_select(1U);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(zzapp_services_current_pid());
	EXPECT(CBSTATE(0) != AAR_STATE_IDLE && CBSTATE(1) != AAR_STATE_IDLE,
	       "producer idle before halted reclaim");
	/* Core 1 is halted: no pass quiesces CLOSE. One call reclaims both and
	 * leaves app 0 selected. */
	zzapp_services_test_set_core(0);
	svc_select(0U);
	zzapp_services_reclaim_halted_audio();
	EXPECT(zzapp_services_current_pid() == pid0, "halted reclaim did not preserve the selection");
	EXPECT(CBSTATE(0) == AAR_STATE_IDLE && CBSTATE(1) == AAR_STATE_IDLE,
	       "halted reclaim left a producer unreleased");
	EXPECT(audio_ring_ptr(rec0) == NULL && audio_ring_ptr(rec1) == NULL,
	       "halted reclaim left an audio block allocated");
	EXPECT(runtime_of(rec0)->audio_detach_count == 1U && runtime_of(rec1)->audio_detach_count == 1U,
	       "halted reclaim did not detach both producers");
	EXPECT(mock_client_release(&CL(0)) == SDK_STATUS_OK && mock_client_release(&CL(1)) == SDK_STATUS_OK,
	       "release after halted reclaim");
}

static unsigned loader_end_passes;
static unsigned loader_end_complete_after;

static int loader_end_in_passes(void)
{
	loader_end_passes++;
	return loader_end_passes < loader_end_complete_after ?
		ZZAPP_LOADER_IN_PROGRESS : AAR_OK;
}

/* A LOAD_END that the system thread runs in bounded batches. */
static void queue_loading_end(void)
{
	world_claimed();
	give_space(0U);
	test_force_state(0U, AAR_STATE_LOADING);
	service_loader_state = AAR_STATE_LOADING;
	zzapp_services_test_set_core(0);
	EXPECT(mock_client_call(&CL(0), ZZAPP_OP_LOAD_END, NULL, 0U, NULL, NULL) ==
	       SDK_STATUS_QUEUED, "queue load end");
	zzapp_services_test_set_core(1);
	loader_end_passes = 0U;
	service_test_loader_end_hook = loader_end_in_passes;
}

static void test_load_end_resumes_and_cancels(void)
{
	unsigned pass;

	queue_loading_end();
	loader_end_complete_after = 48U;
	for (pass = 0U; pass < loader_end_complete_after; pass++) {
		zzapp_services_poll();
		if (pass + 1U < loader_end_complete_after &&
		    DEF(0).state != DEFER_RUNNING)
			fail("load end did not remain running between zero batches");
	}
	EXPECT(loader_end_passes == loader_end_complete_after && DEF(0).state == DEFER_DONE,
	       "load end did not finish after bounded zero batches");
	service_test_loader_end_hook = NULL;

	queue_loading_end();
	loader_end_complete_after = UINT32_MAX;
	zzapp_services_poll();
	EXPECT(DEF(0).state == DEFER_RUNNING && loader_end_passes == 1U,
	       "load end did not begin bounded zero batch");
	g_abort_base = service_test_loader_abort_count;
	DEF(0).cancelled = 1U;
	zzapp_services_poll();
	EXPECT(DEF(0).state == DEFER_IDLE && service_test_loader_abort_count == g_abort_base + 1U,
	       "cancelled load end did not abort");
	service_test_loader_end_hook = NULL;
}

/* ---- The other core retires the record a pass just looked up ------------------------------------ */

static uint32_t g_lookup_pid;
static unsigned g_lookup_blocked;
static unsigned g_lookup_retired;

static void retire_at_lookup(uint32_t pid)
{
	zzapp_rec_t *rec;

	if (pid != g_lookup_pid)
		return;
	if (__atomic_test_and_set(&g_slot_state_test_lock, __ATOMIC_ACQUIRE)) {
		g_lookup_blocked++;
		return;
	}
	__atomic_clear(&g_slot_state_test_lock, __ATOMIC_RELEASE);
	rec = zzapp_rec_by_pid(pid);
	if (!rec || zzapp_rec_state(pid) == ZZAPP_REC_FREE)
		return;
	g_lookup_retired++;
	(void)zzapp_rec_retire(pid);
}

static void arm_lookup_race(unsigned n)
{
	g_lookup_pid = PID_OF(n);
	g_lookup_blocked = 0U;
	g_lookup_retired = 0U;
	service_test_lookup_hook = retire_at_lookup;
}

/* The retirement a pass held off goes through once the pass is over. */
static void expect_held_off(const char *what, unsigned n)
{
	service_test_lookup_hook = NULL;
	if (g_lookup_blocked != 1U || g_lookup_retired != 0U) {
		fail(what);
		return;
	}
	(void)zzapp_rec_retire(PID_OF(n));
	EXPECT(zzapp_rec_state(PID_OF(n)) != ZZAPP_REC_LIVE, "retirement after the held-off pass");
}

static void test_vblank_pass_holds_record_lifetime(void)
{
	aar_surface surface;

	world_loaded(ZZAPP_CLIENT_SCANOUT_ACK);
	client_display(&CL(0), FRONT, BACK, SPARE, 0U);
	CL(0).flip_flags = ZZAPP_FLIPF_SUBMITTED;
	EXPECT(open_display(0U, 320U, 200U, AAR_FMT_RGB565, 0U, &surface) == AAR_OK &&
	       zzapp_svc_display_flip_async(1U) == AAR_OK, "vblank lifetime display setup");
	svc_yield();
	/* The app's flip wait pumps the completion, which is what lets the
	 * card acknowledge the submitted flip. */
	EXPECT(zzapp_display_flip_idle() == 0, "flip submitted, not yet shown");
	arm_lookup_race(0U);
	zzapp_services_display_vblank(BACK);
	EXPECT(zzapp_display_flip_idle() == 1, "vblank pass lost its record to the retirement");
	expect_held_off("vblank pass left its record unprotected", 0U);
}

/* The passes walk the sets of records with something to do: a record is put
 * in the one the pass reads, as its parked wait or its audio request would. */
static void test_audio_polls_hold_record_lifetime(void)
{
	world();
	zzapp_services_test_set_core(1);
	slot_state_lock_acquire();
	set_add(&set_svc, PID_OF(0U));
	slot_state_lock_release();
	arm_lookup_race(0U);
	zzapp_services_poll();
	expect_held_off("core 1 pass left its record unprotected", 0U);

	world();
	zzapp_services_test_set_core(0);
	slot_state_lock_acquire();
	set_add(&set_core0, PID_OF(0U));
	slot_state_lock_release();
	arm_lookup_race(0U);
	zzapp_services_core0_poll();
	expect_held_off("core 0 audio poll left its record unprotected", 0U);
}

int main(void)
{
	svc_env_init(SVC_MAX_APPS, 0U, NULL);
	test_z2_query();
	test_chunked_read();
	test_events_drop_on_full();
	test_audio_busy();
	test_late_file_reply_ids();
	test_scheduler_file_timeout_abandons_request();
	test_file_open_flags();
	test_console_eof_ends_wait();
	test_console_roundtrip();
	test_console_partial_writes_continue();
	test_audio_teardown_handoff();
	test_reset_preserves_audio_teardown();
	test_audio_write_cursor_wrap();
	test_audio_ahi_route_names_the_ring();
	test_display_refresh_rotation_and_layout();
	test_display_scanout_ack();
	test_display_wait_flip_idle();
	test_clut8_palette_publication();
	test_palette_never_parks_with_a_silent_client();
	test_palette_coalesces_behind_one_request_in_flight();
	test_palette_latest_wins();
	test_palette_survives_a_full_sq();
	test_palette_across_close_and_reopen();
	test_palette_pending_survives_a_flip();
	test_palette_of_two_claims_is_independent();
	test_palette_at_the_end_of_the_app();
	test_palette_validation_and_partial_updates();
	test_display_latest_flip_posts_nothing();
	test_direct_mailbox_lease();
	test_display_mailbox_uses_scanout_owner();
	test_returned_buffers_are_user_granted();
	test_display_state_ignores_control_block_tampering();
	test_per_app_file_console_and_audio_state();
	test_running_app_ignores_mailbox_selection();
	test_core1_wakes_ready_waiter();
	test_claim_publishes_the_pair();
	test_load_begin_reply_and_limits();
	test_chunk_reads_the_load_buffer();
	test_deferred_waits_for_system_thread();
	test_deferred_requests_queue_independently();
	test_failed_starts_release_admission();
	test_claim_expiry_revokes_the_handle();
	test_claim_lease_renews_with_the_counter();
	test_abandoned_loading_is_quarantined();
	test_explicit_release_of_a_quarantined_shell_and_reset();
	test_loading_chunk_renews_deadline();
	test_loading_lease_counter_renews_deadline();
	test_claim_expiry_cancels_running_load();
	test_stop_cancels_completing_load();
	test_preload_cancellation_keeps_the_loader_consistent();
	test_harvest_racing_completion_reads_reply();
	test_new_claim_does_not_inherit_wait();
	test_stop_ends_a_claimed_record();
	test_stop_cancels_queued_claim_load();
	test_list_name_uses_start_argv();
	test_audio_teardown_quiesces_every_app();
	test_audio_handoff_blocks_release();
	test_cancelled_pending_open_releases_record();
	test_reclaim_halted_audio_reclaims_every_app();
	test_vblank_pass_holds_record_lifetime();
	test_audio_polls_hold_record_lifetime();
	test_load_end_resumes_and_cancels();
	if (g_fails != 0) {
		fprintf(stderr, "%d failure(s)\n", g_fails);
		return 1;
	}
	puts("zzapp services: ok");
	return 0;
}
