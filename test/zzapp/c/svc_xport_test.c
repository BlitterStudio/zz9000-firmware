/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The app services over the AAR transport (zzapp_services.c), on the host:
 * claim and release, the deferred load ops, files, console, log, events and
 * messages, display and palette, audio, the system pass, the doorbell, the
 * quarantine and reset. The services run on the stub kernel and loader of
 * ZZAPP_SERVICES_STANDALONE_TEST over the record arena and an injected
 * host-visible heap. The client is the reference model of aar/transport.h,
 * bound from the control block descriptors, answering from the yield hook the
 * services call in every wait loop.
 */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#define _Static_assert(cond, msg) typedef char zzapp_sa_##__LINE__[(cond) ? 1 : -1]
#endif

#define ZZAPP_SERVICES_STANDALONE_TEST
#include "zzapp_services.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card_pool.h"
#include "host_card_pool.h"
#include "memorymap.h"
#include "xil_cache.h"

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

/* The host's single retire path (zzapp_host.c) is not linked: the tables go
 * back and the wire layer holds or frees the shell. */
extern unsigned svc_test_latch_calls;
#define latch_calls svc_test_latch_calls
static unsigned revoke_visible_calls;
void zzapp_host_revoke_visible(void)
{
	revoke_visible_calls++;
}

int zzapp_rec_retire(uint32_t pid)
{
	zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

	if (!rec || zzapp_rec_state(pid) == ZZAPP_REC_FREE)
		return -1;
	(void)zzapp_rec_set_state(rec, ZZAPP_REC_RETIRING);
	zzapp_rec_retire_tables(rec);
	return zzapp_services_retire_wire(rec);
}

/* ---- Arena and heap --------------------------------------------------------- */

#define HEAP_78M (78U << 20)
#define DDR_WB 0x15de6U
static uint32_t bsp[4096];

static void setup_arena(uint32_t apps)
{
	zzapp_rec_layout_t lay;
	uint32_t addr;
	uint32_t i;

	host_card_pool_reset(128U << 20);
	zzapp_rec_layout_for(apps, HEAP_78M, &lay);
	addr = card_pool_alloc_aligned(&card_pool, lay.bytes, ZZAPP_REC_ARENA_ALIGN,
		CARD_POOL_OWNER(CARD_POOL_CLASS_APP_ARENA, 0U));
	CHECK(addr != 0U);
	CHECK(zzapp_rec_arena_init((void *)(uintptr_t)addr, &lay) == 0);
	for (i = 0U; i < 4096U; i++)
		bsp[i] = (i << 20) | DDR_WB;
	zzapp_as_init(bsp);
}

#define HEAP_BLOCKS 128U

typedef struct {
	void *ptr;
	uint32_t card;
	uint32_t bytes;
} heap_block_t;

static struct {
	heap_block_t block[HEAP_BLOCKS];
	uint32_t next_card;
	uint32_t allocs, frees, live;
	int32_t fail_after;	/* allocations that still succeed; -1 never fail */
} heap;

static void heap_reset(void)
{
	uint32_t i;

	for (i = 0U; i < HEAP_BLOCKS; i++)
		free(heap.block[i].ptr);
	memset(&heap, 0, sizeof(heap));
	heap.next_card = 0x04000000U;
	heap.fail_after = -1;
}

static int heap_alloc(void *ctx, uint32_t bytes, uint32_t align, uint32_t lo,
		      uint32_t hi, void **ptr, uint32_t *card)
{
	uint32_t i;
	void *p;

	(void)ctx;
	(void)lo;
	(void)hi;
	if (heap.fail_after == 0)
		return -1;
	if (heap.fail_after > 0)
		heap.fail_after--;
	for (i = 0U; i < HEAP_BLOCKS && heap.block[i].ptr; i++)
		;
	if (i == HEAP_BLOCKS)
		return -1;
	p = aligned_alloc(align, bytes);
	if (!p)
		return -1;
	memset(p, 0xA5, bytes);
	heap.next_card = (heap.next_card + align - 1U) & ~(align - 1U);
	heap.block[i].ptr = p;
	heap.block[i].card = heap.next_card;
	heap.block[i].bytes = bytes;
	heap.next_card += bytes + 0x2000U;
	heap.allocs++;
	heap.live++;
	*ptr = p;
	*card = heap.block[i].card;
	return 0;
}

static int heap_release(void *ctx, void *ptr, uint32_t card)
{
	uint32_t i;

	(void)ctx;
	for (i = 0U; i < HEAP_BLOCKS; i++) {
		if (heap.block[i].ptr == ptr && heap.block[i].card == card) {
			free(ptr);
			memset(&heap.block[i], 0, sizeof(heap.block[i]));
			heap.live--;
			heap.frees++;
			return 0;
		}
	}
	return -1;
}

/* What the 68k does with a token: board base (0 here) plus token. */
static void *map_token(uint32_t token)
{
	uint32_t i;

	for (i = 0U; i < HEAP_BLOCKS; i++)
		if (heap.block[i].ptr && token >= heap.block[i].card &&
		    token < heap.block[i].card + heap.block[i].bytes)
			return (uint8_t *)heap.block[i].ptr +
			       (token - heap.block[i].card);
	return NULL;
}

uint32_t zzapp_test_hostheap_free(void) { return 0x100000U; }
uint32_t zzapp_test_hostheap_largest(void) { return 0x80000U; }

/* ---- The client ---------------------------------------------------------------- */

typedef struct {
	volatile uint8_t *cb;
	aar_queue sq, cq;
	uint32_t msgs_posted;
} client_t;

static client_t *cur_client;
static zzapp_rec_t *cur_rec;

static uint32_t word(client_t *c, uint32_t off) { return aar_get_be32(c->cb + off); }

static int client_attach(client_t *c, uint32_t cb_token)
{
	volatile uint8_t *cb = map_token(cb_token);
	uint32_t t;

	memset(c, 0, sizeof(*c));
	if (!cb || aar_cb_check(cb) != AAR_OK)
		return -1;
	c->cb = cb;
	t = aar_get_be32(cb + AAR_CB_SQ_DESC + AAR_QD_TOKEN);
	if (aar_cb_queue_bind(&c->sq, (volatile void *)cb,
			      (uint8_t *)map_token(t) +
			      aar_get_be32(cb + AAR_CB_SQ_DESC + AAR_QD_OFFSET),
			      AAR_QUEUE_SQ) != AAR_OK)
		return -1;
	t = aar_get_be32(cb + AAR_CB_CQ_DESC + AAR_QD_TOKEN);
	if (aar_cb_queue_bind(&c->cq, (volatile void *)cb,
			      (uint8_t *)map_token(t) +
			      aar_get_be32(cb + AAR_CB_CQ_DESC + AAR_QD_OFFSET),
			      AAR_QUEUE_CQ) != AAR_OK)
		return -1;
	return 0;
}

/* What the scripted client does. */
static struct {
	int ignore;			/* answer nothing */
	int32_t result;			/* result of file, list and display ops */
	uint32_t file_size;		/* bytes a file offers to read */
	uint32_t file_pos;
	uint32_t console_accept;	/* bytes a console write consumes; 0: all */
	const char *console_in;
	uint32_t console_in_len, console_in_pos;
	uint32_t flip_flags;		/* completion flags of a flip */
	uint32_t pending_vblank;	/* scanout address the next yield shows */
	uint32_t open_front, open_back, open_spare, open_pitch, open_layout;
	uint32_t ring_delta;		/* added to the ring token an audio open echoes */
	uint32_t clock_step;		/* microseconds per yield */
	unsigned stop_at_yield;		/* set the host stop on this yield; 0: never */
	unsigned yields;
	unsigned nreq[0x50];
	aar_request last[0x50];
	char path[300];
	uint8_t written[1u << 20];
	uint32_t written_len;
	uint8_t console_out[1u << 17];
	uint32_t console_out_len;
	unsigned console_writes;
	unsigned console_outstanding_max;
	char logs[64][AAR_LOG_MAX + 1];
	uint32_t log_len[64];
	unsigned log_count;
	uint8_t msgs[16][AAR_MSG_BYTES];
	unsigned msg_count;
	uint32_t palette[256];
	char names[4][24];
	unsigned name_next;
	unsigned wake_on_yield;
} cl;

static uint64_t fake_now;
static uint64_t fake_time(void) { return fake_now; }

static void client_reset(void)
{
	memset(&cl, 0, sizeof(cl));
	cl.open_front = 0x00400000U;
	cl.open_back = 0x00500000U;
	cl.open_pitch = 1280U;
	cl.open_layout = AAR_LAYOUT_RGB565_BE;
	cl.clock_step = 1000U;
}

static void client_post(client_t *c, aar_completion *d)
{
	CHECK(aar_queue_post_completion(&c->cq, d, NULL, NULL) == AAR_OK);
}

static void client_handle(client_t *c, const aar_request *r)
{
	aar_completion d;
	uint8_t *buf = r->buf ? (uint8_t *)map_token(r->buf) + r->buf_off : NULL;
	uint32_t i;

	if (r->op < 0x50U) {
		cl.nreq[r->op]++;
		cl.last[r->op] = *r;
	}
	memset(&d, 0, sizeof(d));
	d.op = r->op;
	d.id = r->id;
	d.result = cl.result;
	switch (r->op) {
	case AAR_OP_LOG:
		if (cl.log_count < 64U && buf) {
			cl.log_len[cl.log_count] = r->buf_len;
			memcpy(cl.logs[cl.log_count], buf, r->buf_len);
			cl.logs[cl.log_count][r->buf_len] = '\0';
		}
		cl.log_count++;
		return;
	case AAR_OP_MSG: {
		uint8_t m[AAR_MSG_BYTES];

		/* The request struct does not carry msg bytes; they are read from
		 * the entry by the caller (client_service). */
		(void)m;
		return;
	}
	case AAR_OP_FILE_OPEN:
	case AAR_OP_LIST_OPEN:
	case AAR_OP_FILE_STAT:
		if (buf)
			strncpy(cl.path, (const char *)buf, sizeof(cl.path) - 1U);
		cl.file_pos = 0U;
		d.out[AAR_R_STAT_SIZE_LO] = cl.file_size;
		d.out[AAR_R_STAT_MTIME_LO] = 1234U;
		d.out[AAR_R_STAT_MODE] = AAR_MODE_FILE;
		break;
	case AAR_OP_FILE_READ: {
		uint32_t left = cl.file_size - cl.file_pos;
		uint32_t n = r->arg[AAR_A_FILE_LEN] < left ? r->arg[AAR_A_FILE_LEN] : left;

		for (i = 0U; i < n; i++)
			buf[i] = (uint8_t)((cl.file_pos + i) * 7U + 3U);
		cl.file_pos += n;
		d.out[AAR_R_FILE_COUNT] = n;
		break;
	}
	case AAR_OP_FILE_WRITE: {
		uint32_t n = r->arg[AAR_A_FILE_LEN];

		if (cl.written_len + n <= sizeof(cl.written))
			memcpy(cl.written + cl.written_len, buf, n);
		cl.written_len += n;
		d.out[AAR_R_FILE_COUNT] = n;
		break;
	}
	case AAR_OP_FILE_SEEK:
		d.out[AAR_R_FILE_POS_HI] = r->arg[AAR_A_FILE_OFF_HI];
		d.out[AAR_R_FILE_POS_LO] = r->arg[AAR_A_FILE_OFF_LO] + 1U;
		break;
	case AAR_OP_LIST_NEXT:
		if (cl.name_next < 3U) {
			strcpy((char *)buf, cl.names[cl.name_next++]);
			d.out[AAR_R_LIST_MODE] = AAR_MODE_FILE;
		} else {
			d.result = AAR_E_NOENT;
		}
		break;
	case AAR_OP_CONSOLE_WRITE: {
		uint32_t n = r->buf_len;

		cl.console_writes++;
		if (cl.console_accept != 0U && cl.console_accept < n)
			n = cl.console_accept;
		if (cl.console_out_len + n <= sizeof(cl.console_out))
			memcpy(cl.console_out + cl.console_out_len, buf, n);
		cl.console_out_len += n;
		d.out[AAR_R_CONSOLE_COUNT] = n;
		d.result = AAR_OK;
		break;
	}
	case AAR_OP_CONSOLE_READ: {
		uint32_t left = cl.console_in_len - cl.console_in_pos;
		uint32_t n = r->buf_len < left ? r->buf_len : left;

		memcpy(buf, cl.console_in + cl.console_in_pos, n);
		cl.console_in_pos += n;
		d.out[AAR_R_CONSOLE_COUNT] = n;
		d.result = AAR_OK;
		break;
	}
	case AAR_OP_DISPLAY_OPEN:
		d.out[AAR_R_DISPLAY_OPEN_ID] = 7U;
		d.out[AAR_R_DISPLAY_OPEN_PITCH] = cl.open_pitch;
		d.out[AAR_R_DISPLAY_OPEN_LAYOUT] = cl.open_layout;
		d.out[AAR_R_DISPLAY_OPEN_FRONT] = cl.open_front;
		d.out[AAR_R_DISPLAY_OPEN_BACK] = cl.open_back;
		d.out[AAR_R_DISPLAY_OPEN_SPARE] = cl.open_spare;
		break;
	case AAR_OP_DISPLAY_FLIP:
		d.flags = (uint16_t)cl.flip_flags;
		if ((cl.flip_flags & ZZAPP_FLIPF_SUBMITTED) != 0U)
			cl.pending_vblank = 1U;	/* resolved by the yield hook */
		break;
	case AAR_OP_DISPLAY_PALETTE:
		for (i = 0U; i < r->arg[AAR_A_DISPLAY_PALETTE_COUNT] && i < 256U; i++)
			cl.palette[r->arg[AAR_A_DISPLAY_PALETTE_FIRST] + i] =
				zzapp_get_be32(buf + i * 4U);
		break;
	case AAR_OP_AUDIO_OPEN:
		d.out[AAR_R_AUDIO_OPEN_RATE] = r->arg[AAR_A_AUDIO_OPEN_RATE];
		d.out[AAR_R_AUDIO_OPEN_CHANNELS] = r->arg[AAR_A_AUDIO_OPEN_CHANNELS];
		d.out[AAR_R_AUDIO_OPEN_RING] = r->buf + cl.ring_delta;
		d.out[AAR_R_AUDIO_OPEN_RING_BYTES] = r->buf_len;
		d.out[AAR_R_AUDIO_OPEN_FRAMES] = 8192U;
		break;
	default:
		break;
	}
	if (!cl.ignore)
		client_post(c, &d);
}

/* Drain the SQ the way the library's service loop does. */
static void client_service(client_t *c)
{
	const volatile uint8_t *slot;

	while (c && aar_queue_peek(&c->sq, NULL, &slot) == 1) {
		aar_request r;
		uint16_t op = aar_get_be16(slot + AAR_ENTRY_OP);

		if (op == AAR_OP_MSG) {
			if (cl.msg_count < 16U)
				aar_entry_get_msg(slot, cl.msgs[cl.msg_count]);
			cl.msg_count++;
			cl.nreq[AAR_OP_MSG]++;
		} else {
			aar_entry_get_request(slot, &r);
			client_handle(c, &r);
		}
		aar_queue_pop(&c->sq, NULL);
	}
}

static void client_yield(void)
{
	cl.yields++;
	fake_now += cl.clock_step;
	if (cl.stop_at_yield != 0U && cl.yields == cl.stop_at_yield)
		cur_rec->host.stop_requested = 1U;
	/* The vertical blank that follows a flip the client submitted: it shows
	 * the address the services published. */
	if (cl.pending_vblank == 2U) {
		cl.pending_vblank = 0U;
		zzapp_services_display_vblank(
			runtime_of(cur_rec)->display_flip_address);
	}
	client_service(cur_client);
	if (cl.pending_vblank == 1U)
		cl.pending_vblank = 2U;
}

/* ---- Mailbox helpers ---------------------------------------------------------------- */

static uint32_t request_seq;

static uint16_t mbox(uint16_t op, uint32_t handle, uint32_t w0, uint32_t w1,
		     uint8_t *reply, uint16_t *reply_len)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t len = 0U;
	uint8_t scratch[ZZAPP_MAILBOX_PAYLOAD_BYTES];

	memset(req, 0, sizeof(req));
	zzapp_put_be32(req, w0);
	zzapp_put_be32(req + 4U, w1);
	zzapp_put_be32(req + ZZAPP_SEL_HANDLE, handle);
	return zzapp_mailbox_dispatch(op, req, sizeof(req),
				      reply ? reply : scratch,
				      reply_len ? reply_len : &len, 1,
				      ++request_seq, 0U);
}

static zzapp_rec_t *claim(client_t *c, uint32_t *handle_out)
{
	uint8_t reply[48];
	uint16_t len = 0U;
	uint16_t st = zzapp_mailbox_dispatch(ZZAPP_OP_CLAIM, NULL, 0U, reply,
					     &len, 1, 0U, 0U);
	uint32_t handle;
	zzapp_rec_t *rec;

	if (st != SDK_STATUS_OK) {
		CHECK(0);
		return NULL;
	}
	CHECK(len == ZZAPP_CLAIM_REPLY_BYTES);
	handle = zzapp_get_be32(reply + ZZAPP_CLAIM_HANDLE);
	rec = zzapp_rec_by_handle(handle);
	CHECK(rec != NULL);
	CHECK(zzapp_get_be32(reply + ZZAPP_CLAIM_CB_BYTES) == AAR_PAIR_BYTES);
	CHECK(client_attach(c, zzapp_get_be32(reply + ZZAPP_CLAIM_CB_TOKEN)) == 0);
	cur_client = c;
	cur_rec = rec;
	zzapp_services_select_pid(rec->hdr.pid);
	if (handle_out)
		*handle_out = handle;
	return rec;
}

/* One deferred op through the mailbox and the system pass. Returns the reply
 * status; *reply_len and reply hold the harvested reply. */
static uint16_t deferred(uint16_t op, uint32_t handle, uint32_t w0, uint32_t w1,
			 uint8_t *reply, uint16_t *reply_len)
{
	uint16_t st = mbox(op, handle, w0, w1, NULL, NULL);
	uint32_t id = 0U, cookie = 0U;
	uint16_t hop = 0U, hst = 0U, hlen = 0U;
	uint8_t hreply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	unsigned i;

	if (st != SDK_STATUS_QUEUED)
		return st;
	zzapp_services_test_set_core(1);
	for (i = 0U; i < 6U; i++) {
		zzapp_services_poll();
		if (zzapp_mailbox_harvest(&id, &cookie, &hop, &hst, hreply, &hlen))
			break;
	}
	CHECK(i < 6U);
	CHECK(id == request_seq && hop == op);
	if (reply_len)
		*reply_len = hlen;
	if (reply)
		memcpy(reply, hreply, hlen);
	zzapp_mailbox_harvest_ack();
	return hst;
}

static void reset_all(void)
{
	zzapp_xport_env_t e;
	uint32_t i;

	for (i = 0U; i < ZZAPP_REC_MAX_APPS; i++)
		; /* records are re-initialised by the arena */
	zzapp_services_test_reset();
	heap_reset();
	memset(&e, 0, sizeof(e));
	e.alloc = heap_alloc;
	e.release = heap_release;
	e.lo = HOST_VISIBLE_HEAP_ADDRESS;
	e.hi = 0x08100000U;
	e.token_adjust = 0U;
	zzapp_xport_init(&e);
	setup_arena(16U);
	client_reset();
	fake_now = 1000000U;
	latch_calls = 0U;
	revoke_visible_calls = 0U;
	request_seq = 0U;
	cur_client = NULL;
	cur_rec = NULL;
	zzapp_services_test_set_time(fake_time);
	zzapp_services_test_set_yield(client_yield);
	zzapp_services_test_set_core(1);
}

static void give_space(zzapp_rec_t *rec)
{
	CHECK(zzapp_as_create(&rec->proc.as, rec->hdr.pid) == 0);
}

/* ---- Claim, QUERY, LIST, RELEASE -------------------------------------------------------- */

static void test_claim_release(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	uint8_t reply[48];
	uint16_t len;
	zzapp_rec_stats_t before, after;

	reset_all();
	zzapp_rec_stats(&before);
	CHECK(zzapp_mailbox_dispatch(ZZAPP_OP_QUERY, NULL, 0U, reply, &len, 1, 0U,
				     0U) == SDK_STATUS_OK);
	CHECK(len == ZZAPP_MAILBOX_PAYLOAD_BYTES);
	CHECK(zzapp_get_be32(reply + ZZAPP_QUERY_ABI_MINOR) == ZZAPP_ABI_MINOR);
	CHECK(zzapp_get_be32(reply + ZZAPP_QUERY_QUEUE_DEPTH) == 64U);
	CHECK(zzapp_get_be32(reply + ZZAPP_QUERY_HOSTHEAP_FREE) == 0x100000U);
	CHECK(zzapp_get_be32(reply + ZZAPP_QUERY_HOSTHEAP_LARGEST) == 0x80000U);
	CHECK(zzapp_get_be32(reply + ZZAPP_QUERY_RECORDS_FREE) == before.free);
	CHECK((zzapp_get_be32(reply + ZZAPP_QUERY_CAPS) & ZZAPP_CAP_Z3) != 0U);

	rec = claim(&c, &handle);
	CHECK(rec && zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_LIVE);
	CHECK(heap.live == 1U && heap.allocs == 1U);
	CHECK(word(&c, AAR_CB_HANDLE) == handle);
	CHECK(word(&c, AAR_CB_STATE) == AAR_STATE_CLAIMED);
	CHECK(word(&c, AAR_CB_LEASE_PERIOD_MS) == 30000U);
	CHECK(rec->claim.state == ZZAPP_ST_CLAIMED);
	CHECK(set_core0.count == 1U);
	zzapp_rec_stats(&after);
	CHECK(after.free + 1U == before.free);

	/* LIST finds it from a cursor, by handle and index, and nothing after. */
	CHECK(mbox(ZZAPP_OP_LIST, 0U, 0U, 0U, reply, &len) == SDK_STATUS_OK);
	CHECK(len == ZZAPP_LIST_REPLY_BYTES);
	CHECK(zzapp_get_be32(reply + ZZAPP_LIST_HANDLE) == handle);
	CHECK(zzapp_get_be32(reply + ZZAPP_LIST_INDEX) == rec->hdr.pid);
	CHECK(zzapp_get_be32(reply + ZZAPP_LIST_STATE) == AAR_STATE_CLAIMED);
	CHECK(mbox(ZZAPP_OP_LIST, 0U, rec->hdr.pid + 1U, 0U, reply, &len) ==
	      SDK_STATUS_OK);
	CHECK(zzapp_get_be32(reply + ZZAPP_LIST_HANDLE) == 0U);

	/* RELEASE ends the claim: the pair, the record and the tables go back,
	 * the handle is stale, and the counts are what they were. */
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_FREE);
	CHECK(heap.live == 0U && heap.allocs == heap.frees);
	CHECK(zzapp_rec_by_handle(handle) == NULL);
	CHECK(mbox(ZZAPP_OP_STOP, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_BAD_HANDLE);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_BAD_HANDLE);
	zzapp_rec_stats(&after);
	CHECK(after.free == before.free);
	CHECK(set_core0.count == 0U && set_svc.count == 0U);

	/* A refused claim changes nothing and answers the reason alone. */
	heap.fail_after = 0;
	CHECK(zzapp_mailbox_dispatch(ZZAPP_OP_CLAIM, NULL, 0U, reply, &len, 1, 0U,
				     0U) == SDK_STATUS_NO_MEMORY);
	CHECK(len == ZZAPP_REFUSAL_REPLY_BYTES);
	CHECK(zzapp_get_be32(reply + ZZAPP_REFUSAL_REASON) == AAR_REASON_HOSTHEAP);
	zzapp_rec_stats(&after);
	CHECK(after.free == before.free);
	heap.fail_after = -1;

	/* Z2 is unsupported and QUERY says no caps. */
	CHECK(zzapp_mailbox_dispatch(ZZAPP_OP_CLAIM, NULL, 0U, reply, &len, 0, 0U,
				     0U) == SDK_STATUS_UNSUPPORTED);
	CHECK(zzapp_mailbox_dispatch(ZZAPP_OP_QUERY, NULL, 0U, reply, &len, 0, 0U,
				     0U) == SDK_STATUS_UNSUPPORTED);
	CHECK(zzapp_get_be32(reply + ZZAPP_QUERY_CAPS) == 0U);
}

static void test_bad_handles(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint8_t reply[48];
	uint16_t len;

	reset_all();
	rec = claim(&c, &handle);
	CHECK(mbox(ZZAPP_OP_STOP, 0U, 0U, 0U, NULL, NULL) == SDK_STATUS_BAD_HANDLE);
	CHECK(mbox(ZZAPP_OP_STOP, handle + 0x10000U, 0U, 0U, NULL, NULL) ==
	      SDK_STATUS_BAD_HANDLE);
	CHECK(mbox(ZZAPP_OP_STOP, 0x00ff0000U | 3U, 0U, 0U, NULL, NULL) ==
	      SDK_STATUS_BAD_HANDLE);
	/* A payload that is not 48 bytes is no handle form at all. */
	memset(req, 0, sizeof(req));
	zzapp_put_be32(req + ZZAPP_SEL_HANDLE, handle);
	CHECK(zzapp_mailbox_dispatch(ZZAPP_OP_STOP, req, 44U, reply, &len, 1, 1U, 0U) ==
	      SDK_STATUS_BAD_REQUEST);
	CHECK(zzapp_mailbox_dispatch(ZZAPP_OP_STOP, NULL, 0U, reply, &len, 1, 1U, 0U) ==
	      SDK_STATUS_BAD_REQUEST);
	/* The retired window ops are gone. */
	CHECK(mbox(0x0c07U, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_UNSUPPORTED);
	CHECK(mbox(0x0c08U, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_UNSUPPORTED);
	CHECK(mbox(0x0c0cU, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_UNSUPPORTED);
	CHECK(zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_LIVE);
	CHECK(latch_calls == 0U);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
}

static void test_kick(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;

	reset_all();
	rec = claim(&c, &handle);
	CHECK(mbox(ZZAPP_OP_KICK, handle, 0x8U, 0U, NULL, NULL) == SDK_STATUS_BAD_REQUEST);
	CHECK(zzapp_xport_kick_take(rec) == 0U);
	CHECK(mbox(ZZAPP_OP_KICK, handle, ZZAPP_KICK_CQ_POSTED | ZZAPP_KICK_SQ_SPACE,
		   0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(set_svc.count == 1U && set_svc.pids[0] == rec->hdr.pid);
	/* The pass takes the bit, finds nothing to do, and drops the record. */
	zzapp_services_poll();
	CHECK(zzapp_xport_kick_take(rec) == 0U);
	CHECK(set_svc.count == 0U);
	CHECK(mbox(ZZAPP_OP_KICK, handle + 0x10000U, ZZAPP_KICK_CQ_POSTED, 0U, NULL,
		   NULL) == SDK_STATUS_BAD_HANDLE);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
}

/* ---- Load, start, stop -------------------------------------------------------------------- */

static uint8_t chunk_seen[256];
static uint32_t chunk_off_seen, chunk_len_seen;
static int chunk_hook(uint32_t off, const void *src, uint32_t len)
{
	chunk_off_seen = off;
	chunk_len_seen = len;
	memcpy(chunk_seen, src, len < sizeof(chunk_seen) ? len : sizeof(chunk_seen));
	return AAR_OK;
}
static int end_calls;
static int end_hook(void)
{
	end_calls++;
	return AAR_OK;
}
static void begin_hook(void) { }

static void test_load_start(void)
{
	client_t c;
	zzapp_rec_t *rec;
	zzapp_xport_buf_t load;
	uint32_t handle;
	uint8_t reply[48];
	uint16_t len;
	uint32_t caps = ZZAPP_CLIENT_CLUT8 | ZZAPP_CLIENT_SCANOUT_ACK;
	uint8_t *area;
	uint32_t argc = 99U;
	uint32_t i;
	const char *argv0 = "Work:bin/prog";

	reset_all();
	service_test_loader_begin_hook = begin_hook;
	service_test_loader_chunk_hook = chunk_hook;
	service_test_loader_end_hook = end_hook;
	rec = claim(&c, &handle);
	aar_put_be32(c.cb + AAR_CB_PRIVATE_MIB, 4U);
	aar_put_be32(c.cb + AAR_CB_SHARED_MIB, 2U);

	/* The client capability word has no unknown bits. */
	CHECK(mbox(ZZAPP_OP_LOAD_BEGIN, handle, 0x80U, 0U, NULL, NULL) ==
	      SDK_STATUS_BAD_REQUEST);
	/* A limit that overflows 4095 MiB is refused before anything queues. */
	aar_put_be32(c.cb + AAR_CB_PRIVATE_MIB, 5000U);
	CHECK(mbox(ZZAPP_OP_LOAD_BEGIN, handle, caps, 0U, NULL, NULL) ==
	      SDK_STATUS_BAD_REQUEST);
	aar_put_be32(c.cb + AAR_CB_PRIVATE_MIB, 4U);

	CHECK(deferred(ZZAPP_OP_LOAD_BEGIN, handle, caps, 0U, reply, &len) ==
	      SDK_STATUS_OK);
	CHECK(len == ZZAPP_LOAD_BEGIN_REPLY_BYTES);
	CHECK(rec->claim.client_caps == caps);
	CHECK(rec->claim.memory_limit == 4U << 20);
	CHECK(rec->claim.host_visible_limit == 2U << 20);
	CHECK(rec->claim.state == ZZAPP_ST_LOADING);
	CHECK(word(&c, AAR_CB_STATE) == AAR_STATE_LOADING);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_LOAD, &load) == 1);
	CHECK(zzapp_get_be32(reply + ZZAPP_LOAD_REPLY_TOKEN) == load.token);
	CHECK(zzapp_get_be32(reply + ZZAPP_LOAD_REPLY_SIZE) == ZZAPP_LOAD_BUFFER_BYTES);
	CHECK(zzapp_get_be32(reply + ZZAPP_LOAD_REPLY_CHUNK_MAX) == ZZAPP_CHUNK_MAX);
	CHECK(zzapp_get_be32(reply + ZZAPP_LOAD_REPLY_ARGV_OFF) == ZZAPP_LOAD_ARGV_OFF);
	CHECK(zzapp_xport_shared_used(rec) == 0x4000U + ZZAPP_LOAD_BUFFER_BYTES);

	/* A chunk is read from the load buffer the client filled. */
	area = map_token(load.token);
	for (i = 0U; i < 200U; i++)
		area[i] = (uint8_t)(i ^ 0x5a);
	CHECK(deferred(ZZAPP_OP_LOAD_CHUNK, handle, 0x1000U, 200U, NULL, NULL) ==
	      SDK_STATUS_OK);
	CHECK(chunk_off_seen == 0x1000U && chunk_len_seen == 200U);
	CHECK(memcmp(chunk_seen, area, 200U) == 0);
	CHECK(deferred(ZZAPP_OP_LOAD_CHUNK, handle, 0U, 0U, NULL, NULL) ==
	      SDK_STATUS_BAD_REQUEST);
	CHECK(deferred(ZZAPP_OP_LOAD_CHUNK, handle, 0U, ZZAPP_CHUNK_MAX + 1U, NULL,
		       NULL) == SDK_STATUS_BAD_REQUEST);
	CHECK(deferred(ZZAPP_OP_LOAD_END, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(end_calls == 1);

	/* START: argc in the request, argv in the load buffer; the name LIST
	 * shows is the last component of argv[0]. */
	memset(area + ZZAPP_LOAD_ARGV_OFF, 0, ZZAPP_LOAD_ARGV_BYTES);
	zzapp_put_be32(area + ZZAPP_LOAD_ARGV_OFF + ZZAPP_ARGV_ARGC_OFF, 1U);
	zzapp_put_be32(area + ZZAPP_LOAD_ARGV_OFF + ZZAPP_ARGV_OFFS_OFF,
		       ZZAPP_LOAD_ARGV_OFF + ZZAPP_ARGV_STR_OFF);
	strcpy((char *)area + ZZAPP_LOAD_ARGV_OFF + ZZAPP_ARGV_STR_OFF, argv0);
	CHECK(mbox(ZZAPP_OP_START, handle, 1U, 0U, NULL, NULL) == SDK_STATUS_QUEUED);
	zzapp_services_poll();
	CHECK(zzapp_services_take_start(&argc) == 1 && argc == 1U);
	CHECK(zzapp_services_take_start(&argc) == 0);
	CHECK(strcmp((const char *)runtime_of(rec)->name, "prog") == 0);
	CHECK(zzapp_services_start_entered(zzapp_services_generation() + 1U) == AAR_E_IO);
	rec->claim.state = ZZAPP_ST_STARTING;

	/* The generation is the handle's. */
	CHECK(zzapp_services_generation() == aar_handle_generation(handle));
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
}

static void test_start_running(void)
{
	client_t c;
	zzapp_rec_t *rec;
	zzapp_xport_buf_t load;
	uint32_t handle;
	uint32_t argc = 0U;
	uint16_t id, op, st, len;
	uint32_t rid, cookie;

	reset_all();
	service_test_loader_begin_hook = begin_hook;
	service_test_loader_end_hook = end_hook;
	rec = claim(&c, &handle);
	CHECK(deferred(ZZAPP_OP_LOAD_BEGIN, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(deferred(ZZAPP_OP_LOAD_END, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_LOAD, &load) == 1);
	CHECK(mbox(ZZAPP_OP_START, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_QUEUED);
	zzapp_services_poll();
	CHECK(zzapp_services_take_start(&argc) == 1 && argc == 0U);
	/* The reply goes out only once the app is entered. */
	CHECK(zzapp_mailbox_harvest(&rid, &cookie, &op, &st, NULL, &len) == 0);
	CHECK(zzapp_services_start_entered(zzapp_services_generation()) == AAR_OK);
	CHECK(rec->claim.state == ZZAPP_ST_RUNNING);
	CHECK(word(&c, AAR_CB_STATE) == AAR_STATE_RUNNING);
	CHECK(zzapp_mailbox_harvest(&rid, &cookie, &op, &st, NULL, &len) == 1);
	CHECK(op == ZZAPP_OP_START && st == SDK_STATUS_OK);
	zzapp_mailbox_harvest_ack();
	(void)id;
	/* STOP of a running app latches the stop and changes nothing else. */
	CHECK(mbox(ZZAPP_OP_STOP, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(latch_calls == 1U && rec->host.stop_requested == 1U);
	CHECK(zzapp_services_stop_requested() == 1);
	/* The control block word the client writes is a stop too. */
	rec->host.stop_requested = 0U;
	CHECK(zzapp_services_stop_requested() == 0);
	aar_put_be32(c.cb + AAR_CB_STOP_REQUEST, 1U);
	CHECK(zzapp_services_stop_requested() == 1);
	/* The app ends: IDLE with the end reason in the control block, the shell,
	 * its queues and buffers held for the client. */
	zzapp_services_set_exit(-7);
	zzapp_services_set_terminal(ZZAPP_END_EXIT);
	zzapp_services_on_app_end();
	CHECK(word(&c, AAR_CB_STATE) == AAR_STATE_STOPPING);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_LOAD, NULL) == 0);
	CHECK(zzapp_rec_retire(rec->hdr.pid) == 0);
	CHECK(zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_RECLAIMED);
	CHECK(word(&c, AAR_CB_STATE) == AAR_STATE_IDLE);
	CHECK(word(&c, AAR_CB_END_REASON) == ZZAPP_END_EXIT);
	CHECK((int32_t)word(&c, AAR_CB_EXIT_CODE) == -7);
	CHECK(heap.live == 1U);
	CHECK(zzapp_rec_by_handle(handle) == rec);
	/* The client reads the status and releases. */
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_FREE);
	CHECK(heap.live == 0U);
}

static void test_load_refused(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	uint8_t reply[48];
	uint16_t len;

	reset_all();
	service_test_loader_begin_hook = begin_hook;
	rec = claim(&c, &handle);
	/* The load buffer cannot be had: LOAD_BEGIN is refused with the reason,
	 * the claim ends, and the shell stays for the client to release. */
	heap.fail_after = 0;
	CHECK(deferred(ZZAPP_OP_LOAD_BEGIN, handle, 0U, 0U, reply, &len) ==
	      SDK_STATUS_NO_MEMORY);
	CHECK(len == ZZAPP_REFUSAL_REPLY_BYTES);
	CHECK(zzapp_get_be32(reply + ZZAPP_REFUSAL_REASON) == AAR_REASON_MEMORY);
	CHECK(zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_RECLAIMED);
	CHECK(word(&c, AAR_CB_STATE) == AAR_STATE_IDLE);
	CHECK(heap.live == 1U);
	heap.fail_after = -1;
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(heap.live == 0U);
}

/* RELEASE of a claim that is running: the stop is latched, core 1's retire
 * frees it, and a second RELEASE is stale. */
static void test_release_running(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	uint32_t pid;

	reset_all();
	service_test_loader_begin_hook = begin_hook;
	rec = claim(&c, &handle);
	pid = rec->hdr.pid;
	rec->claim.state = ZZAPP_ST_RUNNING;
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(latch_calls == 1U);
	CHECK(zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_RELEASE_REQUESTED));
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_LIVE);
	CHECK(heap.live == 1U);
	/* The host stops the app and retires: the shell goes with it. */
	zzapp_services_set_terminal(ZZAPP_END_FORCED);
	zzapp_services_on_app_end();
	CHECK(zzapp_rec_retire(pid) == 1);
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_FREE);
	CHECK(heap.live == 0U);
}

/* ---- Files ------------------------------------------------------------------------------------ */

static void test_files(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	zzapp_xport_buf_t data;
	uint8_t buf[300];
	uint32_t n, i;
	int fd, list;
	aar_stat st;
	aar_dirent de;
	int64_t pos;
	uint8_t *big;
	uint32_t before;

	reset_all();
	rec = claim(&c, &handle);

	/* Open: the flags are checked in the firmware, the path goes to the
	 * scratch area, the fd is the one the request names. */
	cl.file_size = 1000U;
	CHECK(zzapp_svc_file_open("Work:a.txt", AAR_FILE_WRITE | AAR_FILE_EXCL) ==
	      AAR_E_INVAL);
	CHECK(zzapp_svc_file_open("", AAR_FILE_READ) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_FILE_OPEN] == 0U);
	fd = zzapp_svc_file_open("Work:a.txt", AAR_FILE_READ);
	CHECK(fd == ZZAPP_FD_BASE);
	CHECK(strcmp(cl.path, "Work:a.txt") == 0);
	CHECK(cl.last[AAR_OP_FILE_OPEN].arg[AAR_A_FILE_OPEN_FD] == (uint32_t)fd);
	CHECK(cl.last[AAR_OP_FILE_OPEN].arg[AAR_A_FILE_OPEN_FLAGS] == AAR_FILE_READ);
	CHECK(zzapp_xport_shared_used(rec) == 0x4000U + ZZAPP_XPORT_SERVICE_BYTES);
	CHECK(word(&c, AAR_CB_SHARED_USED_LO) == zzapp_xport_shared_used(rec));

	/* Read: through the data block, one request per chunk. */
	CHECK(zzapp_svc_file_read(fd, buf, 100U, &n) == AAR_OK && n == 100U);
	for (i = 0U; i < 100U; i++)
		CHECK(buf[i] == (uint8_t)(i * 7U + 3U));
	CHECK(cl.nreq[AAR_OP_FILE_READ] == 1U);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_DATA, &data) == 1);
	CHECK(data.bytes == 0x2000U);
	/* A short read ends the call; the next read is at the end of the file. */
	CHECK(zzapp_svc_file_read(fd, buf, 300U, &n) == AAR_OK && n == 300U);
	cl.file_size = 450U;
	CHECK(zzapp_svc_file_read(fd, buf, 300U, &n) == AAR_OK && n == 50U);
	CHECK(zzapp_svc_file_read(fd, buf, 300U, &n) == AAR_OK && n == 0U);

	/* A big read goes in 256 KiB chunks through a grown data block. */
	before = cl.nreq[AAR_OP_FILE_READ];
	cl.file_size = 600000U;
	cl.file_pos = 0U;
	big = malloc(600000U);
	CHECK(zzapp_svc_file_read(fd, big, 600000U, &n) == AAR_OK && n == 600000U);
	CHECK(cl.nreq[AAR_OP_FILE_READ] - before == 3U);
	for (i = 0U; i < 600000U; i += 4099U)
		CHECK(big[i] == (uint8_t)(i * 7U + 3U));
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_DATA, &data) == 1);
	CHECK(data.bytes == ZZAPP_XPORT_DATA_MAX);
	free(big);

	/* Write, seek, stat, close. */
	for (i = 0U; i < sizeof(buf); i++)
		buf[i] = (uint8_t)(i * 3U);
	CHECK(zzapp_svc_file_write(fd, buf, sizeof(buf), &n) == AAR_OK && n == sizeof(buf));
	CHECK(cl.written_len == sizeof(buf) && memcmp(cl.written, buf, sizeof(buf)) == 0);
	CHECK(zzapp_svc_file_seek(fd, 0x100000010LL, AAR_SEEK_SET, &pos) == AAR_OK);
	CHECK(pos == 0x100000011LL);
	CHECK(cl.last[AAR_OP_FILE_SEEK].arg[AAR_A_FILE_WHENCE] == (uint32_t)AAR_SEEK_SET);
	CHECK(zzapp_svc_file_seek(fd, 0, 9, &pos) == AAR_E_INVAL);
	cl.file_size = 4242U;
	CHECK(zzapp_svc_file_stat("Work:b", &st) == AAR_OK);
	CHECK(st.size == 4242U && st.mtime == 1234 && st.mode == AAR_MODE_FILE);
	CHECK(zzapp_svc_file_close(fd) == AAR_OK);
	CHECK(zzapp_svc_file_close(fd) == AAR_E_BADF);
	CHECK(zzapp_svc_file_read(fd, buf, 4U, &n) == AAR_E_BADF);

	/* An error result of the client comes back as it is. */
	cl.result = AAR_E_NOENT;
	CHECK(zzapp_svc_file_open("Work:nope", AAR_FILE_READ) == AAR_E_NOENT);
	CHECK(zzapp_svc_file_stat("Work:nope", &st) == AAR_E_NOENT);
	cl.result = AAR_OK;
	/* ...and its fd is free again. */
	CHECK(zzapp_svc_file_open("Work:a.txt", AAR_FILE_READ) == ZZAPP_FD_BASE);
	CHECK(zzapp_svc_file_close(ZZAPP_FD_BASE) == AAR_OK);

	/* Lists. */
	strcpy(cl.names[0], "one");
	strcpy(cl.names[1], "two");
	strcpy(cl.names[2], "three");
	list = zzapp_svc_file_list_open("Work:dir");
	CHECK(list >= ZZAPP_FD_BASE);
	CHECK(zzapp_svc_file_list_next(list, &de) == AAR_OK && strcmp(de.name, "one") == 0);
	CHECK(de.mode == AAR_MODE_FILE);
	CHECK(zzapp_svc_file_list_next(list, &de) == AAR_OK && strcmp(de.name, "two") == 0);
	CHECK(zzapp_svc_file_list_next(list, &de) == AAR_OK && strcmp(de.name, "three") == 0);
	CHECK(zzapp_svc_file_list_next(list, &de) == AAR_E_NOENT);
	CHECK(zzapp_svc_file_list_close(list) == AAR_OK);
	CHECK(zzapp_svc_file_list_next(list, &de) == AAR_E_BADF);

	CHECK(set_svc.count == 1U);	/* parked waits registered it */
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
}

/* A request the client never answers times out after 30 s and its late
 * completion is dropped by id; the next request is unaffected. */
static void test_file_timeout(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	int fd;
	uint32_t late_before;

	reset_all();
	rec = claim(&c, &handle);
	cl.ignore = 1;
	cl.clock_step = 10000000U;
	CHECK(zzapp_svc_file_open("Work:x", AAR_FILE_READ) == AAR_E_TIMEDOUT);
	CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_BLOCKING) == ZZAPP_XPORT_REQ_FREE);
	CHECK(runtime_of(rec)->wait_kind == ZZAPP_WAIT_NONE);
	/* The fd stays retired (the client may still complete into it). */
	cl.ignore = 0;
	cl.clock_step = 1000U;
	fd = zzapp_svc_file_open("Work:y", AAR_FILE_READ);
	CHECK(fd == ZZAPP_FD_BASE + 1);
	/* The old request's completion arrives now and is dropped as late. */
	{
		aar_completion d;

		memset(&d, 0, sizeof(d));
		d.op = AAR_OP_FILE_OPEN;
		d.id = zzapp_xport_stats(rec)->late_dropped + 1U;	/* not the live id */
		late_before = zzapp_xport_stats(rec)->late_dropped;
		d.id = 1U;
		client_post(&c, &d);
		(void)zzapp_xport_pump(rec);
		CHECK(zzapp_xport_stats(rec)->late_dropped == late_before + 1U);
	}
	CHECK(zzapp_svc_file_close(fd) == AAR_OK);

	/* A stop ends the wait with AAR_E_IO. */
	cl.ignore = 1;
	cl.stop_at_yield = cl.yields + 2U;
	CHECK(zzapp_svc_file_open("Work:z", AAR_FILE_READ) == AAR_E_IO);
	CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_BLOCKING) == ZZAPP_XPORT_REQ_FREE);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
}

/* A full SQ parks the caller until the client drains it; requests never fail
 * on a full SQ. */
static void test_sq_full(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle, i;
	uint8_t msg[AAR_MSG_BYTES] = { 1 };
	aar_stat st;

	reset_all();
	rec = claim(&c, &handle);
	for (i = 0U; i < AAR_MSG_CAPACITY; i++)
		CHECK(zzapp_svc_msg_send(msg) == AAR_OK);
	CHECK(zzapp_svc_msg_send(msg) == AAR_E_BUSY);	/* the reserve entry stays */
	CHECK(zzapp_xport_sq_space(rec) == 1);		/* one entry for a request */
	CHECK(zzapp_svc_file_stat("Work:q", &st) == AAR_OK);
	CHECK(cl.msg_count == AAR_MSG_CAPACITY);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);

	/* Really full: the request itself waits for space. */
	reset_all();
	rec = claim(&c, &handle);
	{
		aar_request r;

		memset(&r, 0, sizeof(r));
		r.op = AAR_OP_LOG;
		for (i = 0U; i < 64U; i++)
			if (aar_queue_post_request(&rec->xport.sq, &r, NULL, NULL) != AAR_OK)
				break;
		CHECK(zzapp_xport_sq_space(rec) == 0);
	}
	CHECK(zzapp_svc_file_stat("Work:q", &st) == AAR_OK);
	CHECK(word(&c, AAR_CB_SQ_FULL) >= 1U);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
}

/* ---- Console -------------------------------------------------------------------------------- */

static void test_console(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle, n, i;
	uint8_t *big;
	char in[32];

	reset_all();
	rec = claim(&c, &handle);

	/* A write returns at once; one span is outstanding at the client. */
	CHECK(zzapp_svc_console_write("hello ", 6U, &n) == AAR_OK && n == 6U);
	CHECK(word(&c, AAR_CB_FLAGS) & AAR_CBF_CONSOLE);
	CHECK(cl.console_writes == 0U);
	CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_CONSOLE_OUT) == ZZAPP_XPORT_REQ_POSTED);
	CHECK(zzapp_svc_console_write("world\n", 6U, &n) == AAR_OK && n == 6U);
	/* The second write waits behind the outstanding one: still one request. */
	CHECK(zzapp_xport_stats(rec)->pumped == 0U);
	client_service(&c);
	CHECK(cl.console_writes == 1U && cl.console_out_len == 6U);
	zzapp_services_poll();	/* the pass completes the span and posts the next */
	client_service(&c);
	zzapp_services_poll();
	CHECK(cl.console_out_len == 12U);
	CHECK(memcmp(cl.console_out, "hello world\n", 12U) == 0);
	CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_CONSOLE_OUT) == ZZAPP_XPORT_REQ_FREE);
	CHECK(set_svc.count == 0U);	/* nothing left in flight: the pass let go */

	/* A client that consumes part of a span gets the rest in the next. */
	cl.console_accept = 4U;
	cl.console_out_len = 0U;
	cl.console_writes = 0U;
	CHECK(zzapp_svc_console_write("abcdefghij", 10U, &n) == AAR_OK && n == 10U);
	for (i = 0U; i < 8U; i++) {
		client_service(&c);
		zzapp_services_poll();
	}
	CHECK(cl.console_out_len == 10U && memcmp(cl.console_out, "abcdefghij", 10U) == 0);
	CHECK(cl.console_writes == 3U);
	cl.console_accept = 0U;

	/* More than the ring holds: the writer parks, the client drains, the
	 * stream arrives complete and in order, in spans that never wrap. */
	big = malloc(100000U);
	for (i = 0U; i < 100000U; i++)
		big[i] = (uint8_t)(i * 13U + (i >> 8));
	cl.console_out_len = 0U;
	cl.console_writes = 0U;
	CHECK(zzapp_svc_console_write(big, 100000U, &n) == AAR_OK && n == 100000U);
	for (i = 0U; i < 40U && cl.console_out_len < 100000U; i++) {
		client_service(&c);
		zzapp_services_poll();
	}
	CHECK(cl.console_out_len == 100000U);
	CHECK(memcmp(cl.console_out, big, 100000U) == 0);
	CHECK(runtime_of(rec)->console_head - runtime_of(rec)->console_tail <= 32768U);
	free(big);

	/* Input is a request the client answers while it is outstanding. */
	cl.console_in = "ab\n";
	cl.console_in_len = 3U;
	CHECK(zzapp_svc_console_read(in, sizeof(in), &n) == AAR_OK && n == 3U);
	CHECK(memcmp(in, "ab\n", 3U) == 0);
	CHECK(cl.last[AAR_OP_CONSOLE_READ].buf_len == sizeof(in));
	CHECK(zzapp_svc_console_read(in, sizeof(in), &n) == AAR_OK && n == 0U);	/* EOF */
	CHECK(zzapp_svc_console_read(in, 0U, &n) == AAR_OK && n == 0U);
	/* The room is at most the 4 KiB console-in area. */
	CHECK(zzapp_svc_console_read(in, 0U, &n) == AAR_OK);
	big = malloc(10000U);
	cl.console_in_pos = cl.console_in_len;
	CHECK(zzapp_svc_console_read(big, 10000U, &n) == AAR_OK && n == 0U);
	CHECK(cl.last[AAR_OP_CONSOLE_READ].buf_len == ZZAPP_XPORT_CONSOLE_IN_BYTES);
	free(big);

	/* A stop ends a read that is waiting. */
	cl.ignore = 1;
	cl.stop_at_yield = cl.yields + 3U;
	CHECK(zzapp_svc_console_read(in, sizeof(in), &n) == AAR_E_IO);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
}

/* Output the app wrote before it ended still goes out, and the shell keeps
 * the pass until it has. */
static void test_console_after_exit(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle, n, i;

	reset_all();
	rec = claim(&c, &handle);
	rec->claim.state = ZZAPP_ST_RUNNING;
	CHECK(zzapp_svc_console_write("bye ", 4U, &n) == AAR_OK);
	CHECK(zzapp_svc_console_write("bye\n", 4U, &n) == AAR_OK);
	zzapp_services_set_terminal(ZZAPP_END_EXIT);
	zzapp_services_on_app_end();
	CHECK(zzapp_rec_retire(rec->hdr.pid) == 0);
	for (i = 0U; i < 6U; i++) {
		client_service(&c);
		zzapp_services_poll();
	}
	CHECK(cl.console_out_len == 8U && memcmp(cl.console_out, "bye bye\n", 8U) == 0);
	CHECK(set_svc.count == 0U);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(heap.live == 0U);
}

/* ---- Log, events, messages -------------------------------------------------------------------- */

static void test_log(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle, i;

	reset_all();
	rec = claim(&c, &handle);
	zzapp_svc_log("first line", 10U);
	client_service(&c);
	CHECK(cl.log_count == 1U && strcmp(cl.logs[0], "first line") == 0);
	/* 32 a second reach the client; the rest are counted. */
	for (i = 1U; i < 40U; i++) {
		char s[16];

		snprintf(s, sizeof(s), "l%u", (unsigned)i);
		zzapp_svc_log(s, (uint32_t)strlen(s));
		client_service(&c);
	}
	CHECK(cl.log_count == 32U);
	CHECK(word(&c, AAR_CB_LOG_DROPPED) == 8U);
	fake_now += 1000000U;
	zzapp_svc_log("again", 5U);
	client_service(&c);
	CHECK(cl.log_count == 33U && strcmp(cl.logs[32], "again") == 0);
	/* Eight records the client has not taken fill the slots; the ninth drops. */
	fake_now += 1000000U;
	for (i = 0U; i < 9U; i++)
		zzapp_svc_log("x", 1U);
	CHECK(word(&c, AAR_CB_LOG_DROPPED) == 9U);
	/* Truncated at AAR_LOG_MAX. */
	client_service(&c);
	{
		char big[AAR_LOG_MAX + 50U];

		memset(big, 'z', sizeof(big));
		fake_now += 1000000U;
		zzapp_svc_log(big, sizeof(big));
		client_service(&c);
		CHECK(cl.log_len[cl.log_count > 63U ? 63U : cl.log_count - 1U] <= AAR_LOG_MAX ||
		      cl.log_count > 63U);
	}
	(void)rec;
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
}

static void test_events_messages(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle, n, i;
	aar_event ev[8], in;
	uint8_t msg[AAR_MSG_BYTES], got[AAR_MSG_BYTES];

	reset_all();
	rec = claim(&c, &handle);

	/* Events the client posts reach event_poll in order. */
	for (i = 0U; i < 5U; i++) {
		memset(&in, 0, sizeof(in));
		in.type = AAR_EV_KEY_DOWN;
		in.code = (uint16_t)(0x10U + i);
		in.time_ms = i;
		CHECK(aar_queue_post_event(&c.cq, &in, NULL, NULL) == AAR_OK);
	}
	CHECK(zzapp_svc_event_poll(ev, 3U, &n) == AAR_OK && n == 3U);
	CHECK(ev[0].code == 0x10U && ev[1].code == 0x11U && ev[2].code == 0x12U);
	CHECK(zzapp_svc_event_poll(ev, 8U, &n) == AAR_OK && n == 2U);
	CHECK(ev[0].code == 0x13U && ev[1].code == 0x14U);
	CHECK(zzapp_svc_event_poll(ev, 8U, &n) == AAR_OK && n == 0U);

	/* The client's stop event is the delivery of a stop request the app was
	 * already told of; lifecycle events come first. */
	memset(&in, 0, sizeof(in));
	in.type = AAR_EV_MOUSE;
	in.dx = -3;
	CHECK(aar_queue_post_event(&c.cq, &in, NULL, NULL) == AAR_OK);
	CHECK(zzapp_svc_event_poll(ev, 8U, &n) == AAR_OK && n == 1U &&
	      ev[0].type == AAR_EV_MOUSE && ev[0].dx == -3);

	/* Messages to the client. */
	memset(msg, 0x42, sizeof(msg));
	CHECK(zzapp_svc_msg_send(msg) == AAR_OK);
	client_service(&c);
	CHECK(cl.msg_count == 1U && memcmp(cl.msgs[0], msg, sizeof(msg)) == 0);

	/* Messages from the client: credits come back when the app takes one. */
	for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
		memset(msg, (int)i, sizeof(msg));
		CHECK(aar_cb_post_msg(&c.cq, c.cb, &c.msgs_posted, msg, NULL, NULL) == AAR_OK);
	}
	CHECK(aar_cb_post_msg(&c.cq, c.cb, &c.msgs_posted, msg, NULL, NULL) == AAR_E_BUSY);
	CHECK(word(&c, AAR_CB_MSG_CONSUMED) == 0U);
	CHECK(zzapp_svc_msg_poll(got) == 1 && got[0] == 0U);
	CHECK(word(&c, AAR_CB_MSG_CONSUMED) == 1U);
	CHECK(aar_cb_post_msg(&c.cq, c.cb, &c.msgs_posted, msg, NULL, NULL) == AAR_OK);
	for (i = 1U; i < AAR_MSG_CAPACITY; i++)
		CHECK(zzapp_svc_msg_poll(got) == 1 && got[0] == (uint8_t)i);
	CHECK(zzapp_svc_msg_poll(got) == 1);	/* the one posted last */
	CHECK(zzapp_svc_msg_poll(got) == 0);
	CHECK(word(&c, AAR_CB_MSG_CONSUMED) == AAR_MSG_CAPACITY + 1U);

	/* msg_wait: zero only tests; a message that arrives while parked ends it;
	 * a timeout is reported; a stop ends it. */
	CHECK(zzapp_svc_msg_wait(0U) == AAR_E_TIMEDOUT);
	cl.clock_step = 5000U;
	CHECK(zzapp_svc_msg_wait(20000U) == AAR_E_TIMEDOUT);
	cl.clock_step = 1000U;
	cl.stop_at_yield = cl.yields + 2U;
	CHECK(zzapp_svc_msg_wait(50000000U) == AAR_E_IO);
	rec->host.stop_requested = 0U;
	cl.stop_at_yield = 0U;
	memset(msg, 9, sizeof(msg));
	CHECK(aar_cb_post_msg(&c.cq, c.cb, &c.msgs_posted, msg, NULL, NULL) == AAR_OK);
	CHECK(zzapp_svc_msg_wait(1000000U) == AAR_OK);
	CHECK(zzapp_svc_msg_poll(got) == 1 && got[0] == 9U);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
}

/* ---- The system pass ---------------------------------------------------------------------------- */

static void test_pass_wakes(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle, id;
	aar_request r;
	aar_completion d;
	struct zzapp_service_runtime *st;
	uint8_t msg[AAR_MSG_BYTES] = { 1 };

	reset_all();
	rec = claim(&c, &handle);
	st = runtime_of(rec);

	/* A parked file wait wakes when its completion is in the CQ, not before. */
	memset(&r, 0, sizeof(r));
	r.op = AAR_OP_FILE_STAT;
	CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &r, 0U, &id) == AAR_OK);
	st->wait_kind = ZZAPP_WAIT_FILE;
	st->wait_value = id;
	slot_state_lock_acquire();
	set_add(&set_svc, rec->hdr.pid);
	slot_state_lock_release();
	service_test_wake_count = 0U;
	zzapp_services_poll();
	CHECK(service_test_wake_count == 0U);
	cl.ignore = 0;
	client_service(&c);
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U);
	CHECK(rec->thread.wake_reason == ZZAPP_WAKE_READY);
	CHECK(set_svc.count == 1U);	/* still parked as far as the pass knows */
	st->wait_kind = ZZAPP_WAIT_NONE;
	(void)zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, &d);
	zzapp_services_poll();
	CHECK(set_svc.count == 0U);

	/* A wrong id does not wake. */
	CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &r, 0U, &id) == AAR_OK);
	st->wait_kind = ZZAPP_WAIT_FILE;
	st->wait_value = id + 100U;
	slot_state_lock_acquire();
	set_add(&set_svc, rec->hdr.pid);
	slot_state_lock_release();
	service_test_wake_count = 0U;
	client_service(&c);
	zzapp_services_poll();
	CHECK(service_test_wake_count == 0U);
	/* The deadline wakes with the timeout reason. */
	st->wait_deadline = fake_now + 5U;
	fake_now += 10U;
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U && rec->thread.wake_reason == ZZAPP_WAKE_TIMEOUT);
	zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);
	(void)zzapp_xport_pump(rec);
	st->wait_kind = ZZAPP_WAIT_NONE;
	st->wait_deadline = 0U;

	/* An event wait wakes on a message in the inbox, and the message beats the
	 * deadline. */
	st->wait_kind = ZZAPP_WAIT_EVENT;
	st->wait_deadline = fake_now - 1U;
	CHECK(aar_cb_post_msg(&c.cq, c.cb, &c.msgs_posted, msg, NULL, NULL) == AAR_OK);
	service_test_wake_count = 0U;
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U && rec->thread.wake_reason == ZZAPP_WAKE_READY);
	st->wait_kind = ZZAPP_WAIT_NONE;
	st->wait_deadline = 0U;

	/* SQ space wakes when the client has drained. */
	{
		uint32_t i;

		memset(&r, 0, sizeof(r));
		r.op = AAR_OP_LOG;
		for (i = 0U; i < 64U; i++)
			if (aar_queue_post_request(&rec->xport.sq, &r, NULL, NULL) != AAR_OK)
				break;
		st->wait_kind = ZZAPP_WAIT_SQ_SPACE;
		slot_state_lock_acquire();
		set_add(&set_svc, rec->hdr.pid);
		slot_state_lock_release();
		service_test_wake_count = 0U;
		zzapp_services_poll();
		CHECK(service_test_wake_count == 0U);
		client_service(&c);
		zzapp_services_poll();
		CHECK(service_test_wake_count == 1U);
		st->wait_kind = ZZAPP_WAIT_NONE;
	}
	/* A corrupt CQ wakes the waiter so it can end with an error. */
	st->wait_kind = ZZAPP_WAIT_FILE;
	st->wait_value = 0xfffffU;
	aar_put_be32(c.cq.tail, 0x1000U);
	service_test_wake_count = 0U;
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U);
	CHECK(zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_CORRUPT));
	st->wait_kind = ZZAPP_WAIT_NONE;
	aar_put_be32(c.cq.tail, 0U);
	zzapp_services_poll();
	CHECK(set_svc.count == 0U);
	zzapp_xport_flag_clear(rec, ZZAPP_XPORT_F_CORRUPT);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
}

/* ---- Display ----------------------------------------------------------------------------------- */

static void test_display_pan(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_surface s;
	struct zzapp_service_runtime *st;
	uint32_t pal[4] = { 0xff112233U, 0x00445566U, 0x00778899U, 0x00aabbccU };

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_SCANOUT_ACK | ZZAPP_CLIENT_CLUT8;
	st = runtime_of(rec);

	CHECK(zzapp_svc_display_open(0U, 480U, AAR_FMT_RGB565, 0U, &s) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0x80U, &s) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_DISPLAY_OPEN] == 0U);
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0U, &s) == AAR_OK);
	CHECK(cl.last[AAR_OP_DISPLAY_OPEN].arg[AAR_A_DISPLAY_OPEN_W] == 640U);
	CHECK(cl.last[AAR_OP_DISPLAY_OPEN].arg[AAR_A_DISPLAY_OPEN_H] == 480U);
	CHECK(cl.last[AAR_OP_DISPLAY_OPEN].arg[AAR_A_DISPLAY_OPEN_FORMAT] == AAR_FMT_RGB565);
	CHECK(s.id == 1U && s.width == 640U && s.pitch == 1280U);
	CHECK(s.format == AAR_FMT_RGB565 && s.layout == AAR_LAYOUT_RGB565_BE);
	CHECK((s.caps & AAR_DISPCAP_ASYNC) && (s.caps & AAR_DISPCAP_CLUT8) &&
	      !(s.caps & AAR_DISPCAP_LATEST));
	CHECK(s.front != NULL && s.back != NULL);
	CHECK(st->display_bound == 1 && st->display_client_id == 7U);
	CHECK(st->display_front == 0x00400000U && st->display_back == 0x00500000U);
	CHECK(word(&c, AAR_CB_FLAGS) & AAR_CBF_DISPLAY);
	CHECK(set_display.count == 1U);
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0U, &s) == AAR_E_BUSY);
	CHECK(zzapp_display_latest() == 0 && zzapp_services_flags() == 0U);

	/* A flip is a request; the completion with SUBMITTED leaves the
	 * acknowledgement to the vertical blank, which shows the flip address. */
	cl.flip_flags = ZZAPP_FLIPF_SUBMITTED;
	CHECK(zzapp_display_flip_idle() == 1);
	CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
	CHECK(cl.nreq[AAR_OP_DISPLAY_FLIP] == 1U);
	CHECK(cl.last[AAR_OP_DISPLAY_FLIP].arg[AAR_A_DISPLAY_ID] == 7U);
	CHECK(cl.last[AAR_OP_DISPLAY_FLIP].arg[AAR_A_DISPLAY_FLIP_FLAGS] == 0U);
	CHECK(st->display_flip_gen == 1U && st->display_flip_ack == 1U);
	CHECK(st->display_flip_address == 0x00500000U);
	CHECK(zzapp_display_flip_idle() == 1);
	/* The front and back buffers swapped when the flip was acknowledged. */
	CHECK(st->display_front == 0x00500000U && st->display_back == 0x00400000U);

	/* A flip the client completes without SUBMITTED is complete at once, with
	 * no vertical blank. */
	cl.flip_flags = 0U;
	CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
	CHECK(st->display_flip_gen == 2U && st->display_flip_ack == 2U);
	/* ...and the async form returns before the client has answered. */
	cl.ignore = 1;
	CHECK(zzapp_svc_display_flip_async(1U) == AAR_OK);
	client_service(&c);
	CHECK(cl.last[AAR_OP_DISPLAY_FLIP].arg[AAR_A_DISPLAY_FLIP_FLAGS] == AAR_FLIPF_ASYNC);
	CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_FLIP) == ZZAPP_XPORT_REQ_POSTED);
	CHECK(zzapp_display_flip_idle() == 0);
	CHECK(zzapp_svc_display_flip_async(1U) == AAR_E_BUSY);
	cl.ignore = 0;
	{
		aar_completion d;

		memset(&d, 0, sizeof(d));
		d.op = AAR_OP_DISPLAY_FLIP;
		d.id = rec->xport.req[ZZAPP_XPORT_REQ_FLIP].id;
		client_post(&c, &d);
	}
	CHECK(zzapp_display_flip_idle() == 1);
	/* A vertical blank with nothing submitted changes nothing. */
	zzapp_services_display_vblank(0x00400000U);
	CHECK(st->display_flip_ack == st->display_flip_gen);

	/* A wait for the flip that never gets its blank is reported, not silent:
	 * the stop ends it. */
	cl.flip_flags = ZZAPP_FLIPF_SUBMITTED;
	cl.pending_vblank = 0U;
	cl.ignore = 0;
	st->display_scanout_ack = 0U;
	CHECK(zzapp_svc_display_flip_async(1U) == AAR_OK);
	CHECK(zzapp_display_flip_idle() == 0);
	client_service(&c);
	CHECK(zzapp_display_flip_idle() == 1);
	st->display_scanout_ack = 1U;

	/* Palette of a CLUT8 surface only. */
	CHECK(zzapp_svc_display_set_palette(1U, 0U, 4U, pal) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_close(1U) == AAR_OK);
	CHECK(cl.nreq[AAR_OP_DISPLAY_CLOSE] == 1U && cl.last[AAR_OP_DISPLAY_CLOSE].arg[0] == 7U);
	CHECK(st->display_bound == 0 && set_display.count == 0U);
	CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_FLIP) == ZZAPP_XPORT_REQ_FREE);
	CHECK(zzapp_svc_display_close(1U) == AAR_E_INVAL);

	/* CLUT8 with a palette, only for a client that serves it. */
	rec->claim.client_caps = 0U;
	CHECK(zzapp_svc_display_open(320U, 200U, AAR_FMT_CLUT8, 0U, &s) == AAR_E_NOSYS);
	rec->claim.client_caps = ZZAPP_CLIENT_CLUT8;
	cl.open_pitch = 320U;
	cl.open_layout = AAR_LAYOUT_CLUT8;
	CHECK(zzapp_svc_display_open(320U, 200U, AAR_FMT_CLUT8, 0U, &s) == AAR_OK);
	CHECK(zzapp_svc_display_set_palette(1U, 252U, 4U, pal) == AAR_OK);
	CHECK(cl.palette[252] == 0x00112233U && cl.palette[253] == 0x00445566U);
	CHECK(cl.last[AAR_OP_DISPLAY_PALETTE].arg[AAR_A_DISPLAY_PALETTE_FIRST] == 252U &&
	      cl.last[AAR_OP_DISPLAY_PALETTE].arg[AAR_A_DISPLAY_PALETTE_COUNT] == 4U);
	CHECK(zzapp_svc_display_set_palette(1U, 0U, 0U, pal) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_set_palette(1U, 255U, 2U, pal) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_set_palette(2U, 0U, 1U, pal) == AAR_E_INVAL);

	/* The app ends with the display bound: grants and the surface lease go. */
	zzapp_services_on_app_end();
	CHECK(st->display_bound == 0 && set_display.count == 0U);
	CHECK(zzapp_rec_retire(rec->hdr.pid) == 0);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(heap.live == 0U);
}

/* The handshake with the vblank ISR: the completion publishes the submit, the
 * ISR acknowledges at the flip address, and not at another. */
static void test_display_vblank(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_surface s;
	struct zzapp_service_runtime *st;
	aar_completion d;

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_SCANOUT_ACK;
	st = runtime_of(rec);
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0U, &s) == AAR_OK);
	cl.ignore = 1;
	CHECK(zzapp_svc_display_flip_async(1U) == AAR_OK);
	CHECK(st->display_flip_gen == 1U && st->display_flip_ack == 0U &&
	      st->display_flip_submit == 0U);
	memset(&d, 0, sizeof(d));
	d.op = AAR_OP_DISPLAY_FLIP;
	d.id = rec->xport.req[ZZAPP_XPORT_REQ_FLIP].id;
	d.flags = ZZAPP_FLIPF_SUBMITTED;
	client_post(&c, &d);
	CHECK(zzapp_display_flip_idle() == 0);
	CHECK(st->display_flip_submit == 1U && st->display_flip_ack == 0U);
	zzapp_services_display_vblank(0x12345678U);
	CHECK(st->display_flip_ack == 0U);
	zzapp_services_display_vblank(st->display_flip_address);
	CHECK(st->display_flip_ack == 1U);
	CHECK(zzapp_display_flip_idle() == 1);
	/* A completion carrying an error finishes the flip too (no stall). */
	CHECK(zzapp_svc_display_flip_async(1U) == AAR_OK);
	d.id = rec->xport.req[ZZAPP_XPORT_REQ_FLIP].id;
	d.flags = 0U;
	d.result = AAR_E_IO;
	client_post(&c, &d);
	CHECK(zzapp_display_flip_idle() == 1);
	/* A stop abandons a wait for a flip the client never answers. */
	cl.ignore = 1;
	cl.stop_at_yield = cl.yields + 2U;
	CHECK(zzapp_svc_display_flip(1U) == AAR_E_IO);
	CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_FLIP) == ZZAPP_XPORT_REQ_FREE);
	zzapp_services_on_app_end();
	zzapp_services_set_terminal(ZZAPP_END_FORCED);
	(void)zzapp_rec_retire(rec->hdr.pid);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(heap.live == 0U);
}

static void test_display_latest(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_surface s;
	struct zzapp_service_runtime *st;
	uint32_t scan = 0U;

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_LATEST;
	st = runtime_of(rec);
	cl.open_spare = 0x00600000U;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, AAR_DISPLAY_LATEST, &s) ==
	      AAR_OK);
	CHECK(s.caps & AAR_DISPCAP_LATEST);
	CHECK(zzapp_display_latest() == 1 && (zzapp_services_flags() & ZZAPP_F_MAILBOX));
	CHECK(zzapp_svc_display_latest_dropped() == 0U);
	/* The card owns these flips: the client sees only the open. */
	CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
	CHECK(cl.nreq[AAR_OP_DISPLAY_FLIP] == 0U);
	CHECK(st->display_mailbox_pending == 0x00500000U);
	/* The vblank latches the pending frame and the front moves. */
	CHECK(zzapp_services_display_mailbox_begin(0x00400000U, &scan) == 1);
	CHECK(scan == st->display_mailbox_pending || scan == 0x00500000U);
	zzapp_services_display_mailbox_end(1);
	CHECK(st->display_mailbox_pending == 0U);
	CHECK(st->display_front == st->display_mailbox_shown);
	CHECK(zzapp_services_display_mailbox_begin(0x09000000U, &scan) == 0);
	/* Without the client's LATEST capability the open falls back to plain. */
	zzapp_services_on_app_end();
	CHECK(zzapp_display_latest() == 0);
	rec->claim.client_caps = 0U;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, AAR_DISPLAY_LATEST, &s) ==
	      AAR_OK);
	CHECK(!(s.caps & AAR_DISPCAP_LATEST));
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(rec->hdr.pid);
	(void)mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL);
}

/* A completion that names bitmaps outside the card's ranges is refused, and the
 * client is told to close its screen. */
static void test_display_bad_tokens(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_surface s;

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	cl.open_front = 0x0c000000U;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0U, &s) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_DISPLAY_CLOSE] == 1U);
	CHECK(runtime_of(rec)->display_bound == 0);
	cl.open_front = 0x00400000U;
	cl.open_pitch = 100U;	/* less than a row */
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0U, &s) == AAR_E_INVAL);
	cl.open_pitch = 1280U;
	cl.open_layout = AAR_LAYOUT_BGRA8888;	/* not a layout of RGB565 */
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0U, &s) == AAR_E_INVAL);
	CHECK(set_display.count == 0U);
	(void)mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL);
}

/* ---- Audio ----------------------------------------------------------------------------------------- */

static void test_audio_card(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_audio a;
	zzapp_xport_buf_t ring;
	const uint32_t *hdr;

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	CHECK(zzapp_svc_audio_open(44100U, 2U, &a) == AAR_E_INVAL);
	CHECK(zzapp_svc_audio_open(22050U, 1U, &a) == AAR_E_INVAL);
	CHECK(zzapp_svc_audio_open(22050U, 2U, &a) == AAR_OK);
	CHECK(a.frames == 8192U && a.rate == 22050U && a.channels == 2U);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, &ring) == 1);
	CHECK(ring.bytes == ZZAPP_XPORT_AUDIO_BYTES(8192U));
	hdr = (const uint32_t *)ring.ptr;
	/* The ring is little-endian and the runtime fills its header. */
	CHECK(hdr[AAR_AUDIO_RING_RATE / 4U] == 22050U);
	CHECK(hdr[AAR_AUDIO_RING_CHANNELS / 4U] == 2U);
	CHECK(hdr[AAR_AUDIO_RING_FORMAT / 4U] == AAR_AUDIO_FORMAT_S16LE);
	CHECK(hdr[AAR_AUDIO_RING_WRITE_FRAME / 4U] == 0U &&
	      hdr[AAR_AUDIO_RING_READ_FRAME / 4U] == 0U);
	CHECK((uintptr_t)a.write_frame - (uintptr_t)a.read_frame ==
	      (uintptr_t)(AAR_AUDIO_RING_WRITE_FRAME) - AAR_AUDIO_RING_READ_FRAME);
	CHECK(runtime_of(rec)->audio_open == 1 && runtime_of(rec)->audio_ahi_route == 0);
	CHECK(word(&c, AAR_CB_FLAGS) & AAR_CBF_AUDIO);
	CHECK(cl.nreq[AAR_OP_AUDIO_OPEN] == 0U);	/* the card route needs no client */
	CHECK(zzapp_svc_audio_open(22050U, 2U, &a) == AAR_E_BUSY);
	CHECK(set_core0.count == 1U);
	/* Close returns the ring. */
	CHECK(zzapp_svc_audio_close() == AAR_OK);
	CHECK(zzapp_services_test_audio_detach_count() == 1U);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 0);
	CHECK(zzapp_svc_audio_close() == AAR_E_INVAL);
	/* The platform lanes busy: no ring is taken. */
	zzapp_services_test_set_slot_busy(AUDIO_FABRIC_SLOT_MAILBOX, 1);
	zzapp_services_test_set_slot_busy(AUDIO_FABRIC_SLOT_RESERVED, 1);
	CHECK(zzapp_svc_audio_open(22050U, 2U, &a) == AAR_E_BUSY);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 0);
	zzapp_services_test_set_slot_busy(AUDIO_FABRIC_SLOT_MAILBOX, 0);
	zzapp_services_test_set_slot_busy(AUDIO_FABRIC_SLOT_RESERVED, 0);
	/* The ring is limited by the shared limit like any block. */
	zzapp_xport_set_shared_limit(rec, 0x4000U + 0x1000U);
	CHECK(zzapp_svc_audio_open(22050U, 2U, &a) == AAR_E_NOMEM);
	zzapp_xport_set_shared_limit(rec, 0U);
	CHECK(zzapp_svc_audio_open(22050U, 2U, &a) == AAR_OK);
	/* The app ends with the stream open: the handoff is posted, core 0
	 * finishes it, and only then does the shell read IDLE. */
	zzapp_services_set_terminal(ZZAPP_END_EXIT);
	zzapp_services_on_app_end();
	CHECK(runtime_of(rec)->audio_request == AUDIO_REQUEST_CLOSE);
	CHECK(zzapp_rec_retire(rec->hdr.pid) == 0);
	CHECK(word(&c, AAR_CB_STATE) == AAR_STATE_STOPPING);
	zzapp_services_poll();	/* core 1 quiesces */
	CHECK(runtime_of(rec)->audio_request == AUDIO_REQUEST_CLOSE_QUIESCED);
	zzapp_services_core0_poll();
	CHECK(runtime_of(rec)->audio_request == AUDIO_REQUEST_NONE);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 0);
	CHECK(word(&c, AAR_CB_STATE) == AAR_STATE_IDLE);
	/* A release requested during the handoff waits for it. */
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_FREE);
	CHECK(heap.live == 0U);
}

static void test_audio_ahi(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_audio a;
	zzapp_xport_buf_t ring;

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_AUDIO_AHI;
	CHECK(zzapp_svc_audio_open(22050U, 2U, &a) == AAR_OK);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, &ring) == 1);
	CHECK(runtime_of(rec)->audio_ahi_route == 1);
	/* The request names the ring the runtime allocated. */
	CHECK(cl.nreq[AAR_OP_AUDIO_OPEN] == 1U);
	CHECK(cl.last[AAR_OP_AUDIO_OPEN].buf == ring.token);
	CHECK(cl.last[AAR_OP_AUDIO_OPEN].buf_len == AAR_AUDIO_RING_HDR_BYTES + 8192U * 4U);
	CHECK(cl.last[AAR_OP_AUDIO_OPEN].arg[AAR_A_AUDIO_OPEN_RATE] == 22050U);
	CHECK(cl.last[AAR_OP_AUDIO_OPEN].arg[AAR_A_AUDIO_OPEN_CHANNELS] == 2U);
	CHECK(cl.last[AAR_OP_AUDIO_OPEN].arg[AAR_A_AUDIO_OPEN_ROUTE] == AAR_AUDIO_ROUTE_AHI);
	/* Close tells the client first, then returns the ring. */
	CHECK(zzapp_svc_audio_close() == AAR_OK);
	CHECK(cl.nreq[AAR_OP_AUDIO_CLOSE] == 1U);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 0);
	/* A client that echoes a different ring is refused and nothing stays. */
	cl.ring_delta = 0x2000U;
	CHECK(zzapp_svc_audio_open(22050U, 2U, &a) == AAR_E_IO);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 0);
	CHECK(runtime_of(rec)->audio_open == 0);
	CHECK(rec->kernel.resources.direct_audio == 0U);
	/* A client that fails the open is the same. */
	cl.ring_delta = 0U;
	cl.result = AAR_E_NOSYS;
	CHECK(zzapp_svc_audio_open(22050U, 2U, &a) == AAR_E_NOSYS);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 0);
	cl.result = AAR_OK;
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(heap.live == 0U);
}

/* ---- Lease, quarantine, reset ------------------------------------------------------------------------- */

static void test_lease_and_quarantine(void)
{
	client_t c;
	zzapp_rec_t *rec, *rec2;
	uint32_t handle, handle2, pid, newh;
	uint8_t reply[48];
	uint16_t len;

	reset_all();
	service_test_loader_begin_hook = begin_hook;
	rec = claim(&c, &handle);
	pid = rec->hdr.pid;
	/* A claim that keeps its lease counter moving is not expired. */
	aar_put_be32(c.cb + AAR_CB_LEASE_COUNTER, 1U);
	zzapp_services_expire_claims(fake_now + 20000000U);
	aar_put_be32(c.cb + AAR_CB_LEASE_COUNTER, 2U);
	zzapp_services_expire_claims(fake_now + 40000000U);
	CHECK(rec->claim.state == ZZAPP_ST_CLAIMED && latch_calls == 0U);
	/* Silent for 30 s: the claim ends, its shell is kept under a revoked
	 * handle and reads IDLE with the end reason. */
	zzapp_services_expire_claims(fake_now + 40000000U + 31000000U);
	CHECK(latch_calls == 1U);
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_RECLAIMED);
	CHECK(zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_QUARANTINED));
	CHECK(zzapp_rec_by_handle(handle) == NULL);
	CHECK(rec->hdr.handle != handle && rec->hdr.handle != 0U);
	newh = rec->hdr.handle;
	CHECK(word(&c, AAR_CB_STATE) == AAR_STATE_IDLE);
	CHECK(word(&c, AAR_CB_END_REASON) == ZZAPP_END_LEASE);
	CHECK(heap.live == 1U);
	/* LIST reports it under its current handle. */
	CHECK(mbox(ZZAPP_OP_LIST, 0U, 0U, 0U, reply, &len) == SDK_STATUS_OK);
	CHECK(zzapp_get_be32(reply + ZZAPP_LIST_HANDLE) == newh);
	CHECK(reply[ZZAPP_LIST_REASONS + 1U] == ZZAPP_END_LEASE);
	/* The old client's next request is stale and lets the shell go. */
	CHECK(mbox(ZZAPP_OP_LOAD_BEGIN, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_BAD_HANDLE);
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_FREE);
	CHECK(heap.live == 0U);
	CHECK(revoke_visible_calls == 1U);

	/* An administrative RELEASE with the listed handle does the same. */
	reset_all();
	rec = claim(&c, &handle);
	pid = rec->hdr.pid;
	zzapp_services_expire_claims(fake_now + 31000000U);
	newh = rec->hdr.handle;
	CHECK(mbox(ZZAPP_OP_RELEASE, newh, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_FREE && heap.live == 0U);

	/* A load in flight: cancelled and quarantined at once, the shell is held
	 * once core 1 retires it, and an old-handle request meanwhile is queued for
	 * release. */
	reset_all();
	service_test_loader_begin_hook = begin_hook;
	rec = claim(&c, &handle);
	pid = rec->hdr.pid;
	CHECK(deferred(ZZAPP_OP_LOAD_BEGIN, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(rec->claim.state == ZZAPP_ST_LOADING);
	zzapp_services_expire_claims(runtime_of(rec)->load_progress + 31000000U);
	CHECK(latch_calls == 1U);
	CHECK(zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_QUARANTINED));
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_LIVE);	/* core 1 retires it */
	CHECK(rec->claim.terminal_reason == ZZAPP_END_QUARANTINED);
	CHECK(mbox(ZZAPP_OP_STOP, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_BAD_HANDLE);
	CHECK(zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_RELEASE_REQUESTED));
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_LIVE);
	zzapp_services_on_app_end();
	CHECK(zzapp_rec_retire(pid) == 1);	/* the retire frees it: release was asked */
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_FREE && heap.live == 0U);

	/* Two claims: the quarantine of one leaves the other alone. */
	reset_all();
	rec = claim(&c, &handle);
	rec2 = claim(&c, &handle2);
	CHECK(rec != rec2);
	zzapp_services_select_pid(rec->hdr.pid);
	zzapp_services_expire_claims(fake_now + 31000000U);
	CHECK(zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_RECLAIMED &&
	      zzapp_rec_state(rec2->hdr.pid) == ZZAPP_REC_RECLAIMED);
	/* both were silent; releasing the second by its stale handle frees only it */
	CHECK(mbox(ZZAPP_OP_STOP, handle2, 0U, 0U, NULL, NULL) == SDK_STATUS_BAD_HANDLE);
	CHECK(zzapp_rec_state(rec2->hdr.pid) == ZZAPP_REC_FREE);
	CHECK(zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_RECLAIMED);
	CHECK(heap.live == 1U);
	zzapp_services_on_reset();
	CHECK(zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_FREE && heap.live == 0U);
}

static void test_reset(void)
{
	client_t c, c2, c3;
	zzapp_rec_t *a, *b, *d;
	uint32_t ha, hb, hd;
	zzapp_rec_stats_t before, after;
	aar_audio au;

	reset_all();
	zzapp_rec_stats(&before);
	a = claim(&c, &ha);
	a->claim.state = ZZAPP_ST_RUNNING;
	give_space(a);
	CHECK(zzapp_svc_audio_open(22050U, 2U, &au) == AAR_OK);
	CHECK(zzapp_svc_console_write("x", 1U, &(uint32_t){ 0 }) == AAR_OK);
	b = claim(&c2, &hb);
	zzapp_services_expire_claims(fake_now + 31000000U);	/* b quarantined */
	d = claim(&c3, &hd);
	CHECK(heap.live >= 4U);
	CHECK(zzapp_rec_state(b->hdr.pid) == ZZAPP_REC_RECLAIMED);
	/* The host retires every record before it calls this. */
	zzapp_services_select_pid(a->hdr.pid);
	zzapp_services_set_terminal(ZZAPP_END_AMIGA_RESET);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(a->hdr.pid);
	zzapp_services_select_pid(d->hdr.pid);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(d->hdr.pid);
	/* a's audio handoff is still owed to core 0: its shell stays. */
	zzapp_services_on_reset();
	CHECK(zzapp_rec_state(b->hdr.pid) == ZZAPP_REC_FREE);
	CHECK(zzapp_rec_state(d->hdr.pid) == ZZAPP_REC_FREE);
	CHECK(zzapp_rec_state(a->hdr.pid) != ZZAPP_REC_FREE);
	zzapp_services_select_pid(a->hdr.pid);
	zzapp_services_poll();
	zzapp_services_core0_poll();
	CHECK(zzapp_rec_state(a->hdr.pid) == ZZAPP_REC_FREE);
	CHECK(heap.live == 0U);
	zzapp_rec_stats(&after);
	CHECK(after.free == before.free);
	/* Every handle of before the reset is stale. */
	CHECK(mbox(ZZAPP_OP_STOP, ha, 0U, 0U, NULL, NULL) == SDK_STATUS_BAD_HANDLE);
	CHECK(mbox(ZZAPP_OP_STOP, hb, 0U, 0U, NULL, NULL) == SDK_STATUS_BAD_HANDLE);
	CHECK(mbox(ZZAPP_OP_STOP, hd, 0U, 0U, NULL, NULL) == SDK_STATUS_BAD_HANDLE);
}

/* Many claims at once: records and heap stay balanced through a churn. */
static void test_churn(void)
{
	client_t c[8];
	zzapp_rec_t *rec[8];
	uint32_t h[8], i, round;
	zzapp_rec_stats_t before, after;

	reset_all();
	zzapp_rec_stats(&before);
	for (round = 0U; round < 3U; round++) {
		for (i = 0U; i < 8U; i++)
			rec[i] = claim(&c[i], &h[i]);
		CHECK(heap.live == 8U);
		for (i = 0U; i < 8U; i++) {
			uint32_t n;

			cur_client = &c[i];
			cur_rec = rec[i];
			zzapp_services_select_pid(rec[i]->hdr.pid);
			CHECK(zzapp_svc_console_write("x", 1U, &n) == AAR_OK);
		}
		for (i = 0U; i < 8U; i++)
			CHECK(mbox(ZZAPP_OP_RELEASE, h[i], 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
		CHECK(heap.live == 0U);
		zzapp_services_poll();
		CHECK(set_svc.count == 0U && set_core0.count == 0U);
	}
	zzapp_rec_stats(&after);
	CHECK(after.free == before.free);
}

int main(void)
{
	test_claim_release();
	test_bad_handles();
	test_kick();
	test_load_start();
	test_start_running();
	test_load_refused();
	test_release_running();
	test_files();
	test_file_timeout();
	test_sq_full();
	test_console();
	test_console_after_exit();
	test_log();
	test_events_messages();
	test_pass_wakes();
	test_display_pan();
	test_display_vblank();
	test_display_latest();
	test_display_bad_tokens();
	test_audio_card();
	test_audio_ahi();
	test_lease_and_quarantine();
	test_reset();
	test_churn();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
