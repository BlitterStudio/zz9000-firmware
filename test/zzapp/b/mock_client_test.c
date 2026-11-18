/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The model of the Amiga client (mock_client) against the firmware side of
 * the transport (zzapp_xport.c), nothing else: the services are not linked.
 * This is the test of the instrument the services suites use: claim and
 * attach from the descriptors, every op type answered through the
 * completion queue, forced, delayed, held and dropped answers, completions
 * that wait behind a full CQ and keep their order, credited messages and
 * the doorbell.
 */
#include <stdio.h>
#include <string.h>

#include "card_pool.h"
#include "host_card_pool.h"
#include "memorymap.h"
#include "mock_client.h"
#include "xil_cache.h"
#include "zzapp_xport.h"

struct xil_cache_mock_state g_xil_cache_mock;
int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1U; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
	if (*word)
		return 0;
	*word = 1U;
	return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0U; }

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

static uint32_t bsp[4096];

static void setup(void)
{
	zzapp_rec_layout_t lay;
	uint32_t addr;
	uint32_t i;
	zzapp_xport_env_t env;

	host_card_pool_reset(128U << 20);
	zzapp_rec_layout_for(16U, 78U << 20, &lay);
	addr = card_pool_alloc_aligned(&card_pool, lay.bytes, ZZAPP_REC_ARENA_ALIGN,
		CARD_POOL_OWNER(CARD_POOL_CLASS_APP_ARENA, 0U));
	CHECK(addr != 0U);
	CHECK(zzapp_rec_arena_init((void *)(uintptr_t)addr, &lay) == 0);
	for (i = 0U; i < 0x400U; i++)
		bsp[i] = (i << 20) | 0x15de6U;
	zzapp_as_init(bsp);
	mock_heap_reset();
	env = mock_heap_env();
	zzapp_xport_init(&env);
	mock_client_registry_clear();
}

static zzapp_rec_t *claim(mock_client *c)
{
	zzapp_rec_t *rec = NULL;
	uint32_t handle = 0U;
	zzapp_xport_buf_t pair;

	CHECK(zzapp_rec_admit(&handle, &rec) == 0U);
	CHECK(zzapp_xport_admit(rec) == AAR_REASON_NONE);
	mock_client_init(c);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_PAIR, &pair) == 1);
	CHECK(mock_client_attach(c, pair.token) == AAR_OK);
	CHECK(c->handle == handle);
	return rec;
}

/* Post a request of the blocking entry and let the client answer it. */
static int call(zzapp_rec_t *rec, mock_client *c, aar_request *req,
		aar_completion *out)
{
	uint32_t id = 0U;
	int rc;

	rc = zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, req, 0U, &id);
	if (rc != AAR_OK)
		return rc;
	(void)mock_client_serve(c);
	(void)zzapp_xport_pump(rec);
	rc = zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, out);
	return rc == 1 ? AAR_OK : rc == 0 ? AAR_E_BUSY : rc;
}

static void test_attach_and_claim_reply(void)
{
	mock_client c;
	zzapp_rec_t *rec;
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	zzapp_xport_buf_t pair;
	mock_client d;

	setup();
	rec = claim(&c);
	CHECK(c.attached && c.cb != NULL && c.sq.depth == 64U && c.cq.depth == 64U);
	CHECK(mock_client_cb_get(&c, AAR_CB_STATE) == AAR_STATE_CLAIMED);
	/* The same from a CLAIM reply. */
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_PAIR, &pair) == 1);
	zzapp_put_be32(reply + ZZAPP_CLAIM_HANDLE, rec->hdr.handle);
	zzapp_put_be32(reply + ZZAPP_CLAIM_CB_TOKEN, pair.token);
	zzapp_put_be32(reply + ZZAPP_CLAIM_CB_BYTES, AAR_PAIR_BYTES);
	mock_client_init(&d);
	CHECK(mock_client_attach_reply(&d, reply, ZZAPP_CLAIM_REPLY_BYTES) == AAR_OK);
	CHECK(d.handle == rec->hdr.handle);
	/* A reply that names no block, or a short block, does not attach. */
	zzapp_put_be32(reply + ZZAPP_CLAIM_CB_TOKEN, pair.token + 0x100000U);
	CHECK(mock_client_attach_reply(&d, reply, ZZAPP_CLAIM_REPLY_BYTES) == AAR_E_IO);
	zzapp_put_be32(reply + ZZAPP_CLAIM_CB_TOKEN, pair.token);
	zzapp_put_be32(reply + ZZAPP_CLAIM_CB_BYTES, 0x100U);
	CHECK(mock_client_attach_reply(&d, reply, ZZAPP_CLAIM_REPLY_BYTES) == AAR_E_IO);
	/* No mailbox is wired: a call is unsupported, not a crash. */
	CHECK(mock_client_release(&c) == SDK_STATUS_UNSUPPORTED);
	/* Limits, lease and stop are words of the block. */
	mock_client_set_limits(&c, 8U, 20U, 3U);
	CHECK(zzapp_xport_cb_read_client(rec, AAR_CB_PRIVATE_MIB) == 8U &&
	      zzapp_xport_cb_read_client(rec, AAR_CB_SHARED_MIB) == 20U &&
	      zzapp_xport_cb_read_client(rec, AAR_CB_WEIGHT) == 3U);
	mock_client_renew_lease(&c);
	mock_client_renew_lease(&c);
	CHECK(zzapp_xport_cb_read_client(rec, AAR_CB_LEASE_COUNTER) == 2U);
	mock_client_request_stop(&c);
	CHECK(zzapp_xport_cb_read_client(rec, AAR_CB_STOP_REQUEST) == 1U);
}

static int kicked_handle;
static uint32_t kicked_flags;
static int test_kick(uint32_t handle, uint32_t flags)
{
	kicked_handle = (int)handle;
	kicked_flags = flags;
	return zzapp_xport_kick(handle, flags);
}

static uint16_t mbox_echo(uint16_t op, const uint8_t *req, uint16_t len,
			  uint8_t *reply, uint16_t *rlen)
{
	(void)req;
	(void)len;
	zzapp_put_be32(reply, op);
	*rlen = 4U;
	return SDK_STATUS_OK;
}

static void test_mailbox_helpers_and_doorbell(void)
{
	mock_client c;
	zzapp_rec_t *rec;
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t rlen = 0U;

	setup();
	rec = claim(&c);
	c.mailbox = mbox_echo;
	c.kick = test_kick;
	CHECK(mock_client_call(&c, ZZAPP_OP_STOP, NULL, 0U, reply, &rlen) == SDK_STATUS_OK &&
	      zzapp_get_be32(reply) == ZZAPP_OP_STOP && rlen == 4U);
	CHECK(mock_client_kick_op(&c, ZZAPP_KICK_CQ_POSTED) == SDK_STATUS_OK &&
	      c.kicks == 1U && c.kick_flags_or == ZZAPP_KICK_CQ_POSTED);
	/* The doorbell hook is the firmware's own entry point. */
	CHECK(c.kick(c.handle, ZZAPP_KICK_SQ_SPACE) == AAR_OK &&
	      kicked_handle == (int)c.handle && kicked_flags == ZZAPP_KICK_SQ_SPACE);
	CHECK(zzapp_xport_kick_take(rec) == ZZAPP_KICK_SQ_SPACE);
	CHECK(c.kick(c.handle + 0x10000U, ZZAPP_KICK_CQ_POSTED) == AAR_E_STALE);
}

static void test_every_op_is_answered(void)
{
	mock_client c;
	zzapp_rec_t *rec;
	aar_request req;
	aar_completion comp;
	zzapp_xport_buf_t scratch, data, console;
	const mock_client_hist_t *h;
	uint8_t *p;
	uint32_t i;

	setup();
	rec = claim(&c);
	CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_SCRATCH, &scratch) == AAR_OK);
	CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_CONSOLE_IN, &console) == AAR_OK);
	CHECK(zzapp_xport_data_reserve(rec, 0x4000U, &data) == AAR_OK);

	/* OPEN: the path is read from the scratch area through the token. */
	memset(&req, 0, sizeof(req));
	strcpy(scratch.ptr, "id1/pak0.pak");
	req.op = AAR_OP_FILE_OPEN;
	req.arg[AAR_A_FILE_OPEN_FD] = AAR_FD_BASE;
	req.arg[AAR_A_FILE_OPEN_FLAGS] = AAR_FILE_READ;
	req.buf = scratch.token;
	req.buf_off = scratch.off;
	req.buf_len = 13U;
	CHECK(call(rec, &c, &req, &comp) == AAR_OK && comp.result == AAR_OK);
	h = mock_client_last(&c, AAR_OP_FILE_OPEN);
	CHECK(h && strcmp(h->path, "id1/pak0.pak") == 0 && h->completed);
	CHECK(h->req.arg[AAR_A_FILE_OPEN_FLAGS] == AAR_FILE_READ);

	/* READ: the pattern lands in the data block, COUNT is what fit. */
	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_FILE_READ;
	req.arg[AAR_A_FILE_FD] = AAR_FD_BASE;
	req.arg[AAR_A_FILE_LEN] = 0x3000U;
	req.buf = data.token;
	req.buf_off = data.off;
	req.buf_len = 0x3000U;
	c.read_chunk_cap = 0x1000U;
	CHECK(call(rec, &c, &req, &comp) == AAR_OK && comp.out[AAR_R_FILE_COUNT] == 0x1000U);
	p = data.ptr;
	for (i = 0U; i < 0x1000U; i++)
		if (p[i] != (uint8_t)i) {
			CHECK(0 && "read pattern");
			break;
		}
	c.read_chunk_cap = 0U;
	c.read_remaining = 10U;
	CHECK(call(rec, &c, &req, &comp) == AAR_OK && comp.out[AAR_R_FILE_COUNT] == 10U);
	CHECK(call(rec, &c, &req, &comp) == AAR_OK && comp.out[AAR_R_FILE_COUNT] == 0U);

	/* WRITE collects the bytes; SEEK echoes the offset; STAT answers. */
	memset(&req, 0, sizeof(req));
	memcpy(data.ptr, "written", 7U);
	req.op = AAR_OP_FILE_WRITE;
	req.arg[AAR_A_FILE_FD] = AAR_FD_BASE;
	req.arg[AAR_A_FILE_LEN] = 7U;
	req.buf = data.token;
	req.buf_off = data.off;
	req.buf_len = 7U;
	CHECK(call(rec, &c, &req, &comp) == AAR_OK && comp.out[AAR_R_FILE_COUNT] == 7U);
	CHECK(c.written_len == 7U && memcmp(c.written, "written", 7U) == 0);
	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_FILE_SEEK;
	req.arg[AAR_A_FILE_OFF_HI] = 1U;
	req.arg[AAR_A_FILE_OFF_LO] = 2U;
	CHECK(call(rec, &c, &req, &comp) == AAR_OK && comp.out[AAR_R_FILE_POS_HI] == 1U &&
	      comp.out[AAR_R_FILE_POS_LO] == 2U);
	c.stat_size = 1234U;
	c.stat_mode = 7U;
	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_FILE_STAT;
	req.buf = scratch.token;
	req.buf_off = scratch.off;
	req.buf_len = 13U;
	CHECK(call(rec, &c, &req, &comp) == AAR_OK && comp.out[AAR_R_STAT_SIZE_LO] == 1234U &&
	      comp.out[AAR_R_STAT_MODE] == 7U);

	/* A list: two names, then AAR_E_NOENT, names in the scratch area. */
	c.names[0] = "alpha";
	c.names[1] = "beta";
	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_LIST_OPEN;
	req.buf = scratch.token;
	req.buf_off = scratch.off;
	req.buf_len = 13U;
	CHECK(call(rec, &c, &req, &comp) == AAR_OK);
	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_LIST_NEXT;
	req.buf = scratch.token;
	req.buf_off = scratch.off;
	req.buf_len = 64U;
	CHECK(call(rec, &c, &req, &comp) == AAR_OK && strcmp(scratch.ptr, "alpha") == 0);
	CHECK(call(rec, &c, &req, &comp) == AAR_OK && strcmp(scratch.ptr, "beta") == 0);
	CHECK(call(rec, &c, &req, &comp) == AAR_OK && comp.result == AAR_E_NOENT);

	/* Console: write goes to the client, read returns its input, EOF is 0,
	 * and a read with nothing and no EOF is held. */
	memset(&req, 0, sizeof(req));
	memcpy(console.ptr, "typed", 5U);
	c.console_in_len = 0U;
	req.op = AAR_OP_CONSOLE_READ;
	req.buf = console.token;
	req.buf_off = console.off;
	req.buf_len = 8U;
	CHECK(call(rec, &c, &req, &comp) == AAR_E_BUSY);   /* held: no completion yet */
	CHECK(mock_client_complete(&c, zzapp_xport_req_id(rec, ZZAPP_XPORT_REQ_BLOCKING),
				   AAR_OK, NULL, 0U) == AAR_OK);
	(void)mock_client_serve(&c);
	(void)zzapp_xport_pump(rec);
	CHECK(zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, &comp) == 1);
	c.console_in_eof = 1;
	CHECK(call(rec, &c, &req, &comp) == AAR_OK && comp.out[AAR_R_CONSOLE_COUNT] == 0U);

	/* Display: tokens and caps come back; palette words are collected. */
	c.display_front = 0x1000U;
	c.display_back = 0x2000U;
	c.display_spare = 0x3000U;
	c.display_dispcaps = AAR_DISPCAP_CLUT8;
	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_DISPLAY_OPEN;
	req.arg[AAR_A_DISPLAY_OPEN_W] = 320U;
	req.arg[AAR_A_DISPLAY_OPEN_H] = 200U;
	req.arg[AAR_A_DISPLAY_OPEN_FORMAT] = AAR_FMT_CLUT8;
	CHECK(call(rec, &c, &req, &comp) == AAR_OK &&
	      comp.out[AAR_R_DISPLAY_OPEN_PITCH] == 320U &&
	      comp.out[AAR_R_DISPLAY_OPEN_FRONT] == 0x1000U &&
	      comp.out[AAR_R_DISPLAY_OPEN_BACK] == 0x2000U &&
	      comp.out[AAR_R_DISPLAY_OPEN_SPARE] == 0x3000U &&
	      comp.out[AAR_R_DISPLAY_OPEN_LAYOUT] == AAR_LAYOUT_CLUT8 &&
	      comp.flags == AAR_DISPCAP_CLUT8);
	zzapp_put_be32((uint8_t *)scratch.ptr, 0x00112233U);
	zzapp_put_be32((uint8_t *)scratch.ptr + 4U, 0x00445566U);
	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_DISPLAY_PALETTE;
	req.arg[AAR_A_DISPLAY_PALETTE_FIRST] = 10U;
	req.arg[AAR_A_DISPLAY_PALETTE_COUNT] = 2U;
	req.buf = scratch.token;
	req.buf_off = scratch.off;
	req.buf_len = 8U;
	CHECK(call(rec, &c, &req, &comp) == AAR_OK && c.palette[10] == 0x112233U &&
	      c.palette[11] == 0x445566U);
	c.flip_flags = ZZAPP_FLIPF_SUBMITTED;
	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_DISPLAY_FLIP;
	CHECK(call(rec, &c, &req, &comp) == AAR_OK && comp.flags == ZZAPP_FLIPF_SUBMITTED);

	/* Audio: the ring the request names is echoed. */
	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_AUDIO_OPEN;
	req.arg[AAR_A_AUDIO_OPEN_RATE] = 22050U;
	req.arg[AAR_A_AUDIO_OPEN_CHANNELS] = 2U;
	req.arg[AAR_A_AUDIO_OPEN_ROUTE] = AAR_AUDIO_ROUTE_AHI;
	req.buf = 0x5000U;
	req.buf_len = AAR_AUDIO_RING_HDR_BYTES + 8192U * 4U;
	CHECK(call(rec, &c, &req, &comp) == AAR_OK &&
	      comp.out[AAR_R_AUDIO_OPEN_RING] == 0x5000U &&
	      comp.out[AAR_R_AUDIO_OPEN_FRAMES] == 8192U &&
	      comp.out[AAR_R_AUDIO_OPEN_RATE] == 22050U);
	CHECK(mock_client_count(&c, AAR_OP_FILE_READ) == 3U);
	CHECK(zzapp_xport_stats(rec)->unknown_dropped == 0U);
}

static mock_verdict_t drop_all(mock_client *cl, const aar_request *r,
			       aar_completion *cp)
{
	(void)cl;
	(void)r;
	(void)cp;
	return MOCK_DROP;
}

static void test_forced_delayed_held_dropped(void)
{
	mock_client c;
	zzapp_rec_t *rec;
	aar_request req;
	aar_completion comp;
	uint32_t id;
	unsigned n;

	setup();
	rec = claim(&c);
	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_FILE_CLOSE;
	req.arg[AAR_A_FILE_FD] = AAR_FD_BASE;

	c.forced[AAR_OP_FILE_CLOSE] = 1U;
	c.forced_result[AAR_OP_FILE_CLOSE] = AAR_E_IO;
	CHECK(call(rec, &c, &req, &comp) == AAR_OK && comp.result == AAR_E_IO);
	c.forced[AAR_OP_FILE_CLOSE] = 0U;

	/* A delay of two serves: not complete at the first, complete at the third. */
	c.delay[AAR_OP_FILE_CLOSE] = 2U;
	CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &req, 0U, &id) == AAR_OK);
	for (n = 0U; n < 2U; n++) {
		(void)mock_client_serve(&c);
		(void)zzapp_xport_pump(rec);
		CHECK(zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, &comp) == 0);
	}
	(void)mock_client_serve(&c);
	(void)zzapp_xport_pump(rec);
	CHECK(zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, &comp) == 1 &&
	      comp.id == id);
	c.delay[AAR_OP_FILE_CLOSE] = 0U;

	/* A frozen client answers nothing until it thaws. */
	c.frozen = 1;
	CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &req, 0U, &id) == AAR_OK);
	CHECK(mock_client_serve(&c) == 0 && mock_client_sq_used(&c) == 1);
	c.frozen = 0;
	CHECK(mock_client_serve(&c) == 1);
	(void)zzapp_xport_pump(rec);
	CHECK(zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, &comp) == 1);

	/* A dropped request never completes. */
	c.on_request = drop_all;
	CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &req, 0U, &id) == AAR_OK);
	(void)mock_client_serve(&c);
	(void)zzapp_xport_pump(rec);
	CHECK(zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, &comp) == 0);
	CHECK(mock_client_sq_used(&c) == 0 && !mock_client_last(&c, AAR_OP_FILE_CLOSE)->completed);
	c.on_request = NULL;
}

/* A completion that finds the CQ full waits and keeps its place behind the
 * ones before it; nothing is lost. */
static void test_completions_wait_behind_a_full_cq(void)
{
	mock_client c;
	zzapp_rec_t *rec;
	aar_request req;
	aar_event ev;
	aar_completion comp;
	uint32_t id;
	unsigned i;

	setup();
	rec = claim(&c);
	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_FILE_CLOSE;
	memset(&ev, 0, sizeof(ev));
	ev.type = AAR_EV_KEY_DOWN;
	for (i = 0U; i < ZZAPP_XPORT_DEPTH; i++)
		CHECK(mock_client_post_event(&c, &ev) == AAR_OK);
	CHECK(mock_client_cq_room(&c) == 0U);
	CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &req, 0U, &id) == AAR_OK);
	CHECK(mock_client_serve(&c) == 1 && mock_client_pending(&c) == 1U && c.cq_full_seen == 1U);
	/* The app side drains the events; the next serve gets the completion in. */
	CHECK(zzapp_xport_pump(rec) == (int)ZZAPP_XPORT_DEPTH);
	CHECK(zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, &comp) == 0);
	(void)mock_client_serve(&c);
	CHECK(mock_client_pending(&c) == 0U);
	CHECK(zzapp_xport_pump(rec) == 1);
	CHECK(zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, &comp) == 1 &&
	      comp.id == id);
	CHECK(zzapp_xport_event_pending(rec) == ZZAPP_XPORT_EVENT_ENTRIES);
}

static void test_messages_and_log(void)
{
	mock_client c;
	zzapp_rec_t *rec;
	uint8_t msg[AAR_MSG_BYTES];
	uint8_t got[AAR_MSG_BYTES];
	unsigned i;

	setup();
	rec = claim(&c);
	/* App to client: messages and logs are drained, not requests. */
	memset(msg, 0x31, sizeof(msg));
	CHECK(zzapp_xport_post_msg(rec, msg) == AAR_OK);
	CHECK(zzapp_xport_post_log(rec, "hello log", 9U) == AAR_OK);
	CHECK(mock_client_serve(&c) == 2);
	CHECK(mock_client_get_msg(&c, got) == 1 && memcmp(got, msg, sizeof(msg)) == 0);
	CHECK(mock_client_get_msg(&c, got) == 0);
	CHECK(c.logs_seen == 1U && c.last_log_len == 9U && strcmp(c.last_log, "hello log") == 0);
	CHECK(c.hist_count == 0U);

	/* Client to app: the credit is the control block's MSG_CONSUMED. */
	for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
		memset(msg, (int)i, sizeof(msg));
		CHECK(mock_client_post_msg(&c, msg) == AAR_OK);
	}
	CHECK(mock_client_post_msg(&c, msg) == AAR_E_BUSY);
	CHECK(zzapp_xport_pump(rec) == (int)AAR_MSG_CAPACITY);
	CHECK(mock_client_post_msg(&c, msg) == AAR_E_BUSY);   /* nothing consumed yet */
	CHECK(zzapp_xport_take_msg(rec, got) == 1 && got[0] == 0U);
	CHECK(mock_client_cb_get(&c, AAR_CB_MSG_CONSUMED) == 1U);
	CHECK(mock_client_post_msg(&c, msg) == AAR_OK);
	/* The raw post ignores the credit; the queue's reserve still holds, so a
	 * completion always finds room: 62 more fit beside the one already there. */
	for (i = 0U; mock_client_post_msg_raw(&c, msg) == AAR_OK; i++)
		;
	CHECK(i == AAR_MSG_CAPACITY - 1U);
	CHECK(mock_client_cq_room(&c) == AAR_MSG_RESERVE);
}

int main(void)
{
	test_attach_and_claim_reply();
	test_mailbox_helpers_and_doorbell();
	test_every_op_is_answered();
	test_forced_delayed_held_dropped();
	test_completions_wait_behind_a_full_cq();
	test_messages_and_log();
	printf("mock_client_test: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
