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
	uint32_t flip_front;		/* the page a flip completion says is shown */
	int no_vblank;			/* the blank never shows a submitted flip */
	uint32_t open_front, open_back, open_spare, open_pitch, open_layout;
	uint32_t ring_delta;		/* added to the ring token an audio open echoes */
	uint32_t clock_step;		/* microseconds per yield */
	unsigned stop_at_yield;		/* set the host stop on this yield; 0: never */
	unsigned yields;
	unsigned nreq[0x60];
	aar_request last[0x60];
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
	/* API 0.6 */
	char path2[300];
	uint32_t vol[4];		/* blocks total, used, block bytes, flags */
	uint32_t protect;		/* the protection word FILE_STAT reports */
	uint32_t open_caps;		/* reply flags of a display open and resize */
	uint32_t resize_front, resize_back, resize_pitch, resize_w, resize_h;
	int32_t resize_result;
	uint32_t info[6];		/* the out words of DISPLAY_INFO */
	uint32_t mode_total, mode_max_len;
	char title[300];
	uint32_t ptr_len, ptr_words[2], ptr_last;
	int vblank_silent;
	int32_t vblank_result;
	int64_t time_us;
	uint32_t time_flags;
	uint8_t locale_rec[AAR_LOCALE_REC_BYTES];
	const uint8_t *env_value;
	uint32_t env_len, env_buf_len;
	const uint8_t *clip_data;
	uint32_t clip_len, clip_cap, clip_seq, clip_max_len;
	int clip_drift, clip_stall;
	uint32_t clip_wr_chunks, clip_wr_off[16], clip_wr_flags[16];
	uint32_t clip_wr_len[16], clip_wr_total[16];
	uint32_t clip_take, clip_fail_chunk, clip_committed;
	/* The card's own blank, from the yield hook: the address handed to the
	 * callbacks of video.c, and which one (0 plain, 1 mailbox with a
	 * latch, 2 mailbox whose latch failed). */
	unsigned blank_at_yield;
	uint32_t blank_addr;
	int blank_mailbox;
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

static uint8_t clip_sink[1u << 20];

static void client_handle(client_t *c, const aar_request *r)
{
	aar_completion d;
	uint8_t *buf = r->buf ? (uint8_t *)map_token(r->buf) + r->buf_off : NULL;
	uint32_t i;

	if (r->op < 0x60U) {
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
		d.out[AAR_R_STAT_PROTECT] = cl.protect;
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
		d.flags = (uint16_t)cl.open_caps;
		d.out[AAR_R_DISPLAY_OPEN_ID] = 7U;
		d.out[AAR_R_DISPLAY_OPEN_PITCH] = cl.open_pitch;
		d.out[AAR_R_DISPLAY_OPEN_LAYOUT] = cl.open_layout;
		d.out[AAR_R_DISPLAY_OPEN_FRONT] = cl.open_front;
		d.out[AAR_R_DISPLAY_OPEN_BACK] = cl.open_back;
		d.out[AAR_R_DISPLAY_OPEN_SPARE] = cl.open_spare;
		break;
	case AAR_OP_DISPLAY_FLIP:
		d.flags = (uint16_t)cl.flip_flags;
		d.out[AAR_R_DISPLAY_FLIP_FRONT] = cl.flip_front;
		if ((cl.flip_flags & ZZAPP_FLIPF_SUBMITTED) != 0U && !cl.no_vblank)
			cl.pending_vblank = 1U;	/* resolved by the yield hook */
		break;
	case AAR_OP_DISPLAY_PALETTE:
		for (i = 0U; i < r->arg[AAR_A_DISPLAY_PALETTE_COUNT] && i < 256U; i++)
			cl.palette[r->arg[AAR_A_DISPLAY_PALETTE_FIRST] + i] =
				zzapp_get_be32(buf + i * 4U);
		break;
	case AAR_OP_FILE_DELETE:
	case AAR_OP_DIR_CREATE:
	case AAR_OP_FILE_SET_DATE:
	case AAR_OP_FILE_SET_PROTECT:
		if (buf)
			strncpy(cl.path, (const char *)buf, sizeof(cl.path) - 1U);
		break;
	case AAR_OP_FILE_RENAME:
		if (buf) {
			strncpy(cl.path, (const char *)buf, sizeof(cl.path) - 1U);
			strncpy(cl.path2,
				(const char *)buf + strlen((const char *)buf) + 1U,
				sizeof(cl.path2) - 1U);
		}
		break;
	case AAR_OP_VOLUME_INFO:
		if (buf)
			strncpy(cl.path, (const char *)buf, sizeof(cl.path) - 1U);
		memcpy(d.out, cl.vol, sizeof(cl.vol));
		break;
	case AAR_OP_DISPLAY_INFO:
		memcpy(d.out, cl.info, sizeof(cl.info));
		break;
	case AAR_OP_DISPLAY_MODES: {
		uint32_t room = buf ? r->buf_len / AAR_DISPLAY_MODE_REC_BYTES : 0U;
		uint32_t n = room < cl.mode_total ? room : cl.mode_total;

		if (r->buf_len > cl.mode_max_len)
			cl.mode_max_len = r->buf_len;
		for (i = 0U; i < n; i++) {
			uint8_t *p = buf + i * AAR_DISPLAY_MODE_REC_BYTES;

			zzapp_put_be32(p, 100U + i);
			zzapp_put_be32(p + 4U, 200U + i);
			zzapp_put_be32(p + 8U, 50000U + i);
			zzapp_put_be32(p + 12U, 0xffffffffU);
			zzapp_put_be32(p + 16U, 0xffffffffU);
			zzapp_put_be32(p + 20U, 7U);
		}
		d.out[AAR_R_DISPLAY_MODES_COUNT] = n;
		d.out[AAR_R_DISPLAY_MODES_TOTAL] = cl.mode_total;
		break;
	}
	case AAR_OP_DISPLAY_RESIZE:
		cl.resize_w = r->arg[AAR_A_DISPLAY_RESIZE_W];
		cl.resize_h = r->arg[AAR_A_DISPLAY_RESIZE_H];
		if (cl.resize_result != 0) {
			d.result = cl.resize_result;
			break;
		}
		d.flags = (uint16_t)cl.open_caps;
		d.out[AAR_R_DISPLAY_OPEN_ID] = 9U;
		d.out[AAR_R_DISPLAY_OPEN_PITCH] = cl.resize_pitch;
		d.out[AAR_R_DISPLAY_OPEN_LAYOUT] = cl.open_layout;
		d.out[AAR_R_DISPLAY_OPEN_FRONT] = cl.resize_front;
		d.out[AAR_R_DISPLAY_OPEN_BACK] = cl.resize_back;
		break;
	case AAR_OP_DISPLAY_TITLE:
		if (buf)
			strncpy(cl.title, (const char *)buf, sizeof(cl.title) - 1U);
		break;
	case AAR_OP_DISPLAY_POINTER:
		cl.ptr_len = r->buf_len;
		if (buf && r->buf_len >= 8U) {
			cl.ptr_words[0] = zzapp_get_be32(buf);
			cl.ptr_words[1] = zzapp_get_be32(buf + 4U);
			cl.ptr_last = zzapp_get_be32(buf + r->buf_len - 4U);
		}
		break;
	case AAR_OP_DISPLAY_VBLANK:
		if (cl.vblank_silent)
			return;
		if (cl.vblank_result != 0)
			d.result = cl.vblank_result;
		break;
	case AAR_OP_TIME_UNIX:
		d.out[AAR_R_TIME_UNIX_US_HI] = (uint32_t)((uint64_t)cl.time_us >> 32);
		d.out[AAR_R_TIME_UNIX_US_LO] = (uint32_t)cl.time_us;
		d.out[AAR_R_TIME_UNIX_FLAGS] = cl.time_flags;
		break;
	case AAR_OP_LOCALE_INFO:
		if (buf)
			memcpy(buf, cl.locale_rec, sizeof(cl.locale_rec));
		break;
	case AAR_OP_ENV_GET: {
		uint32_t n = r->buf_len < cl.env_len ? r->buf_len : cl.env_len;

		/* The name is read first, the value is written over it. */
		strncpy(cl.path, (const char *)buf, sizeof(cl.path) - 1U);
		cl.env_buf_len = r->buf_len;
		if (n)
			memcpy(buf, cl.env_value, n);
		if (cl.env_len < r->buf_len)
			buf[cl.env_len] = 0;
		d.out[AAR_R_ENV_LEN] = cl.env_len;
		break;
	}
	case AAR_OP_CLIP_READ: {
		uint32_t off = r->arg[AAR_A_CLIP_OFFSET];
		uint32_t size;
		uint32_t n = 0U;

		if (off == 0U)
			cl.clip_seq = 0U;
		cl.clip_seq++;
		if (r->buf_len > cl.clip_max_len)
			cl.clip_max_len = r->buf_len;
		size = cl.clip_len + (cl.clip_drift && cl.clip_seq > 1U ? 1U : 0U);
		if (off <= cl.clip_len && !(cl.clip_stall && cl.clip_seq > 1U)) {
			n = cl.clip_len - off;
			if (n > r->buf_len)
				n = r->buf_len;
			if (cl.clip_cap != 0U && n > cl.clip_cap)
				n = cl.clip_cap;
			if (n)
				memcpy(buf, cl.clip_data + off, n);
		}
		d.out[AAR_R_CLIP_READ_SIZE] = size;
		d.out[AAR_R_CLIP_READ_COUNT] = n;
		break;
	}
	case AAR_OP_CLIP_WRITE: {
		uint32_t off = r->arg[AAR_A_CLIP_OFFSET];
		unsigned k = cl.clip_wr_chunks;

		if (k < 16U) {
			cl.clip_wr_off[k] = off;
			cl.clip_wr_flags[k] = r->flags;
			cl.clip_wr_len[k] = r->buf_len;
			cl.clip_wr_total[k] = r->arg[AAR_A_CLIP_TOTAL];
		}
		cl.clip_wr_chunks++;
		if (cl.clip_fail_chunk != 0U && cl.clip_wr_chunks == cl.clip_fail_chunk) {
			d.result = AAR_E_NOMEM;
			break;
		}
		if (buf && off + r->buf_len <= sizeof(clip_sink))
			memcpy(clip_sink + off, buf, r->buf_len);
		d.out[AAR_R_CLIP_WRITE_COUNT] = cl.clip_take != 0U ?
			cl.clip_take : r->buf_len;
		if (r->flags & AAR_CLIPF_LAST)
			cl.clip_committed++;
		break;
	}
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
	if (cl.blank_at_yield != 0U && cl.yields == cl.blank_at_yield) {
		uint32_t scan = 0U;

		if (cl.blank_mailbox) {
			if (zzapp_services_display_mailbox_begin(cl.blank_addr, &scan))
				zzapp_services_display_mailbox_end(cl.blank_mailbox == 1);
		} else {
			zzapp_services_display_vblank(cl.blank_addr);
		}
	}
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
	/* The event block of an earlier test lived in the heap just reset. */
	zzapp_xport_init(NULL);
	(void)zzapp_xport_events_init();
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
	CHECK(len == ZZAPP_REFUSAL_CODE_REPLY_BYTES);
	CHECK(zzapp_get_be32(reply + ZZAPP_REFUSAL_REASON) == AAR_REASON_MEMORY);
	CHECK(zzapp_get_be32(reply + ZZAPP_REFUSAL_CODE) == (uint32_t)-AAR_E_NOMEM);
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

	/* An id remembers how it was opened (spec 7.1): a write through an id
	 * opened without WRITE and a read through one opened without READ are
	 * AAR_E_BADF, and nothing reaches the client. */
	{
		uint32_t reads = cl.nreq[AAR_OP_FILE_READ];
		uint32_t writes = cl.nreq[AAR_OP_FILE_WRITE];
		int wfd, rwfd;

		CHECK(fd == ZZAPP_FD_BASE);
		n = 77U;
		CHECK(zzapp_svc_file_write(fd, buf, 16U, &n) == AAR_E_BADF && n == 0U);
		CHECK(zzapp_svc_file_write(fd, buf, 0U, &n) == AAR_E_BADF);
		wfd = zzapp_svc_file_open("Work:w.txt", AAR_FILE_WRITE | AAR_FILE_CREATE);
		CHECK(wfd == ZZAPP_FD_BASE + 1);
		n = 77U;
		CHECK(zzapp_svc_file_read(wfd, buf, 16U, &n) == AAR_E_BADF && n == 0U);
		CHECK(zzapp_svc_file_read(wfd, buf, 0U, &n) == AAR_E_BADF);
		CHECK(cl.nreq[AAR_OP_FILE_READ] == reads);
		CHECK(cl.nreq[AAR_OP_FILE_WRITE] == writes);
		CHECK(zzapp_svc_file_write(wfd, buf, 16U, &n) == AAR_OK && n == 16U);
		CHECK(cl.nreq[AAR_OP_FILE_WRITE] == writes + 1U);
		CHECK(zzapp_svc_file_close(wfd) == AAR_OK);
		/* The flags belong to the open, not to the number: the same id
		 * reopened read-only refuses writes. */
		wfd = zzapp_svc_file_open("Work:w.txt", AAR_FILE_READ);
		CHECK(wfd == ZZAPP_FD_BASE + 1);
		CHECK(zzapp_svc_file_write(wfd, buf, 16U, &n) == AAR_E_BADF);
		CHECK(zzapp_svc_file_read(wfd, buf, 16U, &n) == AAR_OK);
		CHECK(zzapp_svc_file_close(wfd) == AAR_OK);
		rwfd = zzapp_svc_file_open("Work:rw.txt", AAR_FILE_READ | AAR_FILE_WRITE);
		CHECK(rwfd == ZZAPP_FD_BASE + 1);
		CHECK(zzapp_svc_file_read(rwfd, buf, 16U, &n) == AAR_OK);
		CHECK(zzapp_svc_file_write(rwfd, buf, 16U, &n) == AAR_OK && n == 16U);
		CHECK(zzapp_svc_file_close(rwfd) == AAR_OK);
		cl.written_len = 0U;
	}

	/* Write, seek, stat, close. */
	for (i = 0U; i < sizeof(buf); i++)
		buf[i] = (uint8_t)(i * 3U);
	{
		int wfd = zzapp_svc_file_open("Work:w.txt", AAR_FILE_WRITE);

		CHECK(zzapp_svc_file_write(wfd, buf, sizeof(buf), &n) == AAR_OK && n == sizeof(buf));
		CHECK(cl.written_len == sizeof(buf) && memcmp(cl.written, buf, sizeof(buf)) == 0);
		CHECK(zzapp_svc_file_close(wfd) == AAR_OK);
	}
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
	/* The abandoned open still names its path in the scratch area: the next
	 * open answers BUSY until the client has answered the first. */
	cl.ignore = 0;
	cl.clock_step = 1000U;
	CHECK(zzapp_svc_file_open("Work:y", AAR_FILE_READ) == AAR_E_BUSY);
	/* The old request's completion arrives now and is dropped as late. The
	 * fd stays retired (the client may still complete into it). */
	{
		aar_completion d;

		memset(&d, 0, sizeof(d));
		d.op = AAR_OP_FILE_OPEN;
		late_before = zzapp_xport_stats(rec)->late_dropped;
		d.id = 1U;
		client_post(&c, &d);
	}
	/* No other call pumps: the next open takes the late completion first. */
	fd = zzapp_svc_file_open("Work:y", AAR_FILE_READ);
	CHECK(fd == ZZAPP_FD_BASE + 1);
	CHECK(zzapp_xport_stats(rec)->late_dropped == late_before + 1U);
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

static void post_mouse_at(client_t *c, int16_t dx, int16_t dy, uint16_t buttons,
			  uint16_t code, uint32_t time_ms)
{
	aar_event in;

	memset(&in, 0, sizeof(in));
	in.type = AAR_EV_MOUSE;
	in.code = code;
	in.dx = dx;
	in.dy = dy;
	in.buttons = buttons;
	in.time_ms = time_ms;
	CHECK(aar_queue_post_event(&c->cq, &in, NULL, NULL) == AAR_OK);
}

static void post_mouse(client_t *c, int16_t dx, int16_t dy, uint16_t buttons)
{
	post_mouse_at(c, dx, dy, buttons, 0U, 0U);
}

/* Takes the 64 keys of a full ring and then the one held mouse event after
 * them, and nothing more. */
static void take_full_ring_and_held(zzapp_rec_t *rec, aar_event *held)
{
	aar_event ev;
	uint32_t i;

	for (i = 0U; i < ZZAPP_XPORT_EVENT_ENTRIES; i++)
		CHECK(zzapp_xport_take_event(rec, &ev) == 1 &&
		      ev.type == AAR_EV_KEY_DOWN && ev.code == i);
	CHECK(zzapp_xport_take_event(rec, held) == 1);
	CHECK(zzapp_xport_take_event(rec, &ev) == 0);
}

/* Fills the event ring with ZZAPP_XPORT_EVENT_ENTRIES key events. */
static void fill_event_ring(client_t *c, zzapp_rec_t *rec)
{
	aar_event in;
	uint32_t i;

	for (i = 0U; i < ZZAPP_XPORT_EVENT_ENTRIES; i++) {
		memset(&in, 0, sizeof(in));
		in.type = AAR_EV_KEY_DOWN;
		in.code = (uint16_t)i;
		CHECK(aar_queue_post_event(&c->cq, &in, NULL, NULL) == AAR_OK);
		if ((i & 15U) == 15U)
			CHECK(zzapp_xport_pump(rec) == 16);
	}
	CHECK(zzapp_xport_event_pending(rec) == ZZAPP_XPORT_EVENT_ENTRIES);
}

struct drained {
	uint32_t keys, mice, mouse_after_keys;
	int32_t dx, dy;
};

static void drain_events(struct drained *d)
{
	aar_event ev[8];
	uint32_t n, i;

	memset(d, 0, sizeof(*d));
	while (zzapp_svc_event_poll(ev, 8U, &n) == AAR_OK && n != 0U) {
		for (i = 0U; i < n; i++) {
			if (ev[i].type == AAR_EV_MOUSE) {
				d->mice++;
				d->dx += ev[i].dx;
				d->dy += ev[i].dy;
				if (d->keys == ZZAPP_XPORT_EVENT_ENTRIES)
					d->mouse_after_keys++;
			} else if (ev[i].type == AAR_EV_KEY_DOWN) {
				d->keys++;
			}
		}
	}
}

/* Mouse motion that finds the event ring full is merged and delivered when
 * there is room (spec 10.4), not lost. Anything else that finds it full is
 * dropped and counted. */
static void test_event_ring_full_mouse(void)
{
	client_t c;
	zzapp_rec_t *rec;
	struct drained d;
	uint32_t handle, i;
	aar_event in;

	reset_all();
	rec = claim(&c, &handle);

	/* 64 keys fill the ring, then 20 steps of motion: all of it arrives, in
	 * one event, after the keys. */
	fill_event_ring(&c, rec);
	for (i = 0U; i < 20U; i++)
		post_mouse(&c, 1, -2, 0U);
	CHECK(zzapp_xport_pump(rec) == 20);
	CHECK(zzapp_xport_stats(rec)->events_dropped == 0U);
	drain_events(&d);
	CHECK(d.keys == ZZAPP_XPORT_EVENT_ENTRIES);
	CHECK(d.dx == 20 && d.dy == -40);
	CHECK(d.mice == 1U && d.mouse_after_keys == 1U);
	CHECK(zzapp_xport_event_pending(rec) == 0U);

	/* A button change is not folded into earlier motion, a key has no place
	 * to wait: both are dropped and counted. Motion with the same buttons
	 * still merges. */
	fill_event_ring(&c, rec);
	post_mouse(&c, 2, 0, 0U);
	post_mouse(&c, 5, 0, 1U);
	memset(&in, 0, sizeof(in));
	in.type = AAR_EV_KEY_UP;
	CHECK(aar_queue_post_event(&c.cq, &in, NULL, NULL) == AAR_OK);
	post_mouse(&c, 3, 0, 0U);
	CHECK(zzapp_xport_pump(rec) == 4);
	CHECK(zzapp_xport_stats(rec)->events_dropped == 2U);
	drain_events(&d);
	CHECK(d.keys == ZZAPP_XPORT_EVENT_ENTRIES && d.mice == 1U && d.dx == 5);

	/* Motion saturates instead of wrapping. */
	fill_event_ring(&c, rec);
	post_mouse(&c, 30000, 0, 0U);
	post_mouse(&c, 30000, 0, 0U);
	CHECK(zzapp_xport_pump(rec) == 2);
	drain_events(&d);
	CHECK(d.mice == 1U && d.dx == 32767);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
}

/* An absolute mouse event (AAR_MOUSE_ABSOLUTE in buttons) is a position: the
 * merge while the ring is full keeps the latest one with its time, never sums
 * it and never saturates it as a delta; the flag and code stay as posted.
 * Absolute and relative motion, and different buttons, are not folded into
 * each other: the second one is dropped and counted (spec 8.3). */
static void test_event_ring_full_absolute_mouse(void)
{
	client_t c;
	zzapp_rec_t *rec;
	aar_event held;
	uint32_t handle, dropped, i;

	reset_all();
	rec = claim(&c, &handle);

	/* 20 positions (10 i, 5 i): one event, the last position, the flag kept. */
	fill_event_ring(&c, rec);
	dropped = zzapp_xport_stats(rec)->events_dropped;
	for (i = 1U; i <= 20U; i++)
		post_mouse_at(&c, (int16_t)(10U * i), (int16_t)(5U * i),
			      AAR_MOUSE_ABSOLUTE, 0U, 1000U + i);
	CHECK(zzapp_xport_pump(rec) == 20);
	CHECK(zzapp_xport_stats(rec)->events_dropped == dropped);
	take_full_ring_and_held(rec, &held);
	CHECK(held.type == AAR_EV_MOUSE && held.code == 0U);
	CHECK(held.dx == 200 && held.dy == 100);
	CHECK(held.buttons == AAR_MOUSE_ABSOLUTE && held.time_ms == 1020U);

	/* A held button stays set even at coordinates near the int16 limits. */
	fill_event_ring(&c, rec);
	post_mouse_at(&c, 30000, -30000, AAR_MOUSE_ABSOLUTE | AAR_MOUSE_LEFT, 0U, 5U);
	post_mouse_at(&c, -32768, 32767, AAR_MOUSE_ABSOLUTE | AAR_MOUSE_LEFT, 0U, 6U);
	post_mouse_at(&c, 30000, 30000, AAR_MOUSE_ABSOLUTE | AAR_MOUSE_LEFT, 0U, 7U);
	CHECK(zzapp_xport_pump(rec) == 3);
	take_full_ring_and_held(rec, &held);
	CHECK(held.dx == 30000 && held.dy == 30000 && held.code == 0U);
	CHECK(held.buttons == (AAR_MOUSE_ABSOLUTE | AAR_MOUSE_LEFT) && held.time_ms == 7U);

	/* Relative motion into a pending absolute event, a click into it, and
	 * the reverse: dropped, not summed. The position is still the latest
	 * absolute one. */
	fill_event_ring(&c, rec);
	dropped = zzapp_xport_stats(rec)->events_dropped;
	post_mouse_at(&c, 7, 8, AAR_MOUSE_ABSOLUTE, 0U, 1U);
	post_mouse_at(&c, 3, 3, 0U, 0U, 2U);
	post_mouse_at(&c, 9, 9, AAR_MOUSE_ABSOLUTE | AAR_MOUSE_LEFT, 1U, 3U);
	post_mouse_at(&c, 11, 12, AAR_MOUSE_ABSOLUTE, 0U, 4U);
	CHECK(zzapp_xport_pump(rec) == 4);
	CHECK(zzapp_xport_stats(rec)->events_dropped == dropped + 2U);
	take_full_ring_and_held(rec, &held);
	CHECK(held.dx == 11 && held.dy == 12 && held.buttons == AAR_MOUSE_ABSOLUTE &&
	      held.time_ms == 4U);

	fill_event_ring(&c, rec);
	dropped = zzapp_xport_stats(rec)->events_dropped;
	post_mouse_at(&c, 5, 6, 0U, 0U, 1U);
	post_mouse_at(&c, 100, 100, AAR_MOUSE_ABSOLUTE, 0U, 2U);
	post_mouse_at(&c, 1, 1, 0U, 0U, 3U);
	CHECK(zzapp_xport_pump(rec) == 3);
	CHECK(zzapp_xport_stats(rec)->events_dropped == dropped + 1U);
	take_full_ring_and_held(rec, &held);
	CHECK(held.dx == 6 && held.dy == 7 && held.buttons == 0U && held.time_ms == 3U);
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
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0x100U, &s) == AAR_E_INVAL);
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
	CHECK(zzapp_display_latest() == 0);

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
	/* A flip the client completes with an error leaves the scanout alone: the
	 * pages stay where they are and the error reaches the caller. */
	cl.result = AAR_E_IO;
	cl.flip_flags = 0U;
	CHECK(zzapp_svc_display_flip(1U) == AAR_E_IO);
	CHECK(st->display_front == 0x00500000U && st->display_back == 0x00400000U);
	CHECK(zzapp_display_flip_idle() == 1);
	cl.result = AAR_OK;

	/* A flip the client completes without SUBMITTED is complete at once, with
	 * no vertical blank. */
	cl.flip_flags = 0U;
	CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
	CHECK(st->display_flip_gen == 3U && st->display_flip_ack == 3U);
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
	/* The call does not wait: the client takes the request when it serves. */
	client_service(&c);
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

/* The client names the page it shows; the vblank acknowledges at that address.
 * A submitted flip whose blank never comes is complete after 100 ms in the
 * firmware, and a front the client names that is not one of its pages is
 * ignored (the presented page stays the address). */
static void test_display_flip_front_and_timeout(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_surface s;
	struct zzapp_service_runtime *st;
	uint64_t t0;

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_SCANOUT_ACK;
	st = runtime_of(rec);
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0U, &s) == AAR_OK);
	cl.flip_flags = ZZAPP_FLIPF_SUBMITTED;
	cl.flip_front = 0x00400000U;	/* the client shows the old front page */
	CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
	CHECK(st->display_flip_address == 0x00400000U);
	CHECK(st->display_flip_ack == 1U);

	/* An address that is none of the pages is ignored. */
	cl.flip_front = 0x00999000U;
	CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
	CHECK(st->display_flip_address != 0x00999000U);

	/* No blank ever comes: the flip completes by the firmware's timeout, not
	 * before it. */
	cl.no_vblank = 1;
	cl.clock_step = 30000U;
	t0 = fake_now;
	CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
	CHECK(fake_now - t0 >= FLIP_ACK_TIMEOUT_US);
	CHECK(fake_now - t0 < FLIP_ACK_TIMEOUT_US + 2U * cl.clock_step + 1U);
	CHECK(st->display_flip_ack == st->display_flip_gen);
	CHECK(st->display_flip_gen == 3U);
	/* The pass does it for a parked waiter too. */
	cl.ignore = 1;
	cl.clock_step = 1000U;
	CHECK(zzapp_svc_display_flip_async(1U) == AAR_OK);
	cl.ignore = 0;
	client_service(&c);
	CHECK(zzapp_display_flip_idle() == 0);
	fake_now += FLIP_ACK_TIMEOUT_US;
	st->wait_kind = ZZAPP_WAIT_FLIP;
	st->wait_value = st->display_flip_gen;
	slot_state_lock_acquire();
	set_add(&set_svc, rec->hdr.pid);
	slot_state_lock_release();
	service_test_wake_count = 0U;
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U);
	CHECK(zzapp_display_flip_idle() == 1);
	st->wait_kind = ZZAPP_WAIT_NONE;
	zzapp_services_on_app_end();
	zzapp_services_poll();
	(void)zzapp_rec_retire(rec->hdr.pid);
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
	uint32_t rings;

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
	/* An app parked on the flip is woken by core 1's pass. The vblank ends
	 * its wait without a KICK, so the acknowledgement rings the doorbell:
	 * with another app on the core the pass would otherwise wait for the
	 * next 5 ms tick. A vblank at another address acknowledges nothing and
	 * rings nothing. */
	rings = zzapp_services_test_core1_doorbells();
	zzapp_services_display_vblank(0x12345678U);
	CHECK(st->display_flip_ack == 0U);
	CHECK(zzapp_services_test_core1_doorbells() == rings);
	zzapp_services_display_vblank(st->display_flip_address);
	CHECK(st->display_flip_ack == 1U);
	CHECK(zzapp_services_test_core1_doorbells() == rings + 1U);
	/* The same vblank again has nothing left to acknowledge. */
	zzapp_services_display_vblank(st->display_flip_address);
	CHECK(zzapp_services_test_core1_doorbells() == rings + 1U);
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
	CHECK(zzapp_display_latest() == 1);
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

/* ---- API 0.6: files, host environment, clipboard ---------------------------------- */

static void fill_pattern(uint8_t *p, uint32_t n)
{
	uint32_t i;

	for (i = 0U; i < n; i++)
		p[i] = (uint8_t)(i * 13U + (i >> 8) + 1U);
}

static void test_files_v06(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	char p255[256], q255[256], p256[257];
	aar_volume_info vi[4];
	aar_stat st;
	int rfd, wfd;
	uint32_t before;
	unsigned late;
	aar_completion d;
	int32_t errs[4] = { AAR_E_NOENT, AAR_E_EXIST, AAR_E_ACCES, AAR_E_BUSY };
	unsigned i;

	reset_all();
	rec = claim(&c, &handle);
	memset(p255, 'a', 255);
	p255[255] = '\0';
	memset(q255, 'b', 255);
	q255[255] = '\0';
	memset(p256, 'a', 256);
	p256[256] = '\0';

	/* delete and dir_create: the path rules of file_open, the client's error
	 * comes back as it is. */
	CHECK(zzapp_svc_file_delete("Work:old") == AAR_OK);
	CHECK(cl.nreq[AAR_OP_FILE_DELETE] == 1U && strcmp(cl.path, "Work:old") == 0);
	CHECK(zzapp_svc_file_delete("") == AAR_E_INVAL);
	CHECK(zzapp_svc_file_delete(NULL) == AAR_E_INVAL);
	CHECK(zzapp_svc_file_delete(p256) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_FILE_DELETE] == 1U);
	CHECK(zzapp_svc_file_delete(p255) == AAR_OK && strlen(cl.path) == 255U);
	for (i = 0U; i < 4U; i++) {
		cl.result = errs[i];
		CHECK(zzapp_svc_file_delete("Work:x") == errs[i]);
		CHECK(zzapp_svc_dir_create("Work:x") == errs[i]);
	}
	cl.result = AAR_OK;
	CHECK(zzapp_svc_dir_create("Work:newdir") == AAR_OK);
	CHECK(cl.nreq[AAR_OP_DIR_CREATE] == 5U && strcmp(cl.path, "Work:newdir") == 0);
	CHECK(zzapp_svc_dir_create("") == AAR_E_INVAL && cl.nreq[AAR_OP_DIR_CREATE] == 5U);

	/* rename: two paths in one buffer, the flags checked here. */
	CHECK(zzapp_svc_file_rename("Work:a", "Work:b", 0U) == AAR_OK);
	CHECK(strcmp(cl.path, "Work:a") == 0 && strcmp(cl.path2, "Work:b") == 0);
	CHECK(cl.last[AAR_OP_FILE_RENAME].arg[AAR_A_FILE_RENAME_FLAGS] == 0U);
	CHECK(cl.last[AAR_OP_FILE_RENAME].buf_len == 14U);
	CHECK(zzapp_svc_file_rename("Work:a", "Work:b", AAR_RENAME_REPLACE) == AAR_OK);
	CHECK(cl.last[AAR_OP_FILE_RENAME].arg[AAR_A_FILE_RENAME_FLAGS] == AAR_RENAME_REPLACE);
	before = cl.nreq[AAR_OP_FILE_RENAME];
	CHECK(zzapp_svc_file_rename("a", "b", 2U) == AAR_E_INVAL);
	CHECK(zzapp_svc_file_rename("a", "b", 0x80000000U) == AAR_E_INVAL);
	CHECK(zzapp_svc_file_rename("", "b", 0U) == AAR_E_INVAL);
	CHECK(zzapp_svc_file_rename("a", "", 0U) == AAR_E_INVAL);
	CHECK(zzapp_svc_file_rename(NULL, "b", 0U) == AAR_E_INVAL);
	CHECK(zzapp_svc_file_rename("a", NULL, 0U) == AAR_E_INVAL);
	CHECK(zzapp_svc_file_rename(p256, "b", 0U) == AAR_E_INVAL);
	CHECK(zzapp_svc_file_rename("a", p256, 0U) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_FILE_RENAME] == before);
	CHECK(zzapp_svc_file_rename(p255, q255, 0U) == AAR_OK);
	CHECK(strcmp(cl.path, p255) == 0 && strcmp(cl.path2, q255) == 0);
	CHECK(cl.last[AAR_OP_FILE_RENAME].buf_len == 512U);

	/* set_date: an int64 of seconds in two words; set_protect: only the
	 * eight AAR_PROT_* bits. */
	CHECK(zzapp_svc_file_set_date("Work:f", -2) == AAR_OK);
	CHECK(cl.last[AAR_OP_FILE_SET_DATE].arg[AAR_A_FILE_DATE_HI] == 0xffffffffU);
	CHECK(cl.last[AAR_OP_FILE_SET_DATE].arg[AAR_A_FILE_DATE_LO] == 0xfffffffeU);
	CHECK(zzapp_svc_file_set_date("Work:f", 0x100000005LL) == AAR_OK);
	CHECK(cl.last[AAR_OP_FILE_SET_DATE].arg[AAR_A_FILE_DATE_HI] == 1U);
	CHECK(zzapp_svc_file_set_date("", 5) == AAR_E_INVAL);
	before = cl.nreq[AAR_OP_FILE_SET_PROTECT];
	CHECK(zzapp_svc_file_set_protect("Work:f", 0x100U) == AAR_E_INVAL);
	CHECK(zzapp_svc_file_set_protect("Work:f", 0x80000000U) == AAR_E_INVAL);
	CHECK(zzapp_svc_file_set_protect("", 0U) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_FILE_SET_PROTECT] == before);
	CHECK(zzapp_svc_file_set_protect("Work:f", 0xffU) == AAR_OK);
	CHECK(cl.last[AAR_OP_FILE_SET_PROTECT].arg[AAR_A_FILE_PROTECT] == 0xffU);

	/* truncate: an id opened for writing only. */
	cl.file_size = 100U;
	rfd = zzapp_svc_file_open("Work:r", AAR_FILE_READ);
	wfd = zzapp_svc_file_open("Work:w", AAR_FILE_WRITE | AAR_FILE_CREATE);
	CHECK(rfd == ZZAPP_FD_BASE && wfd == ZZAPP_FD_BASE + 1);
	CHECK(zzapp_svc_file_truncate(rfd, 5U) == AAR_E_BADF);
	CHECK(zzapp_svc_file_truncate(99, 5U) == AAR_E_BADF);
	CHECK(zzapp_svc_file_truncate(0, 5U) == AAR_E_BADF);
	CHECK(cl.nreq[AAR_OP_FILE_TRUNCATE] == 0U);
	CHECK(zzapp_svc_file_truncate(wfd, 0x100000002ULL) == AAR_OK);
	CHECK(cl.last[AAR_OP_FILE_TRUNCATE].arg[AAR_A_FILE_TRUNC_FD] == (uint32_t)wfd);
	CHECK(cl.last[AAR_OP_FILE_TRUNCATE].arg[AAR_A_FILE_TRUNC_SIZE_HI] == 1U);
	CHECK(cl.last[AAR_OP_FILE_TRUNCATE].arg[AAR_A_FILE_TRUNC_SIZE_LO] == 2U);
	CHECK(zzapp_svc_file_close(wfd) == AAR_OK);
	CHECK(zzapp_svc_file_truncate(wfd, 0U) == AAR_E_BADF);
	CHECK(zzapp_svc_file_close(rfd) == AAR_OK);

	/* stat reports the protection bits, and only those. */
	cl.protect = 0x1ffU;
	CHECK(zzapp_svc_file_stat("Work:b", &st) == AAR_OK && st.protect == 0xffU);

	/* volume_info: bytes from the block counts, the structure capped at the
	 * caller's size. */
	cl.vol[0] = 1000U;
	cl.vol[1] = 250U;
	cl.vol[2] = 512U;
	cl.vol[3] = AAR_VOLF_READONLY | 0x10U;
	memset(vi, 0xee, sizeof(vi));
	vi[0].size = sizeof(vi[0]);
	CHECK(zzapp_svc_volume_info("Work:", &vi[0]) == AAR_OK);
	CHECK(vi[0].size == sizeof(vi[0]) && vi[0].block_bytes == 512U);
	CHECK(vi[0].total_bytes == 512000U && vi[0].free_bytes == 384000U);
	CHECK(vi[0].flags == AAR_VOLF_READONLY);
	CHECK(vi[0].reserved[0] == 0U && vi[0].reserved[2] == 0U);
	CHECK(vi[1].size == 0xeeeeeeeeU);
	memset(vi, 0xee, sizeof(vi));
	vi[0].size = 8U;
	CHECK(zzapp_svc_volume_info("Work:", &vi[0]) == AAR_OK);
	CHECK(vi[0].size == 8U && vi[0].block_bytes == 512U);
	CHECK(vi[0].total_bytes == 0xeeeeeeeeeeeeeeeeULL && vi[0].flags == 0xeeeeeeeeU);
	memset(vi, 0xee, sizeof(vi));
	vi[0].size = sizeof(vi[0]) + 16U;
	CHECK(zzapp_svc_volume_info("Work:", &vi[0]) == AAR_OK);
	CHECK(vi[0].size == sizeof(vi[0]) && vi[1].size == 0xeeeeeeeeU);
	cl.vol[0] = 10U;
	cl.vol[1] = 20U;	/* more used than there is: nothing free */
	vi[0].size = sizeof(vi[0]);
	CHECK(zzapp_svc_volume_info("Work:", &vi[0]) == AAR_OK && vi[0].free_bytes == 0U);
	cl.vol[2] = 0U;
	memset(vi, 0xee, sizeof(vi));
	vi[0].size = sizeof(vi[0]);
	CHECK(zzapp_svc_volume_info("Work:", &vi[0]) == AAR_E_IO);
	CHECK(vi[0].block_bytes == 0xeeeeeeeeU);
	cl.vol[2] = 512U;
	cl.result = AAR_E_NOENT;
	CHECK(zzapp_svc_volume_info("Work:", &vi[0]) == AAR_E_NOENT);
	cl.result = AAR_OK;
	CHECK(zzapp_svc_volume_info("", &vi[0]) == AAR_E_INVAL);
	CHECK(zzapp_svc_volume_info("Work:", NULL) == AAR_E_INVAL);

	/* A request the client never answers ends after 30 s like a file
	 * request, and leaves the scratch area borrowed until its late
	 * completion. */
	cl.ignore = 1;
	cl.clock_step = 10000000U;
	CHECK(zzapp_svc_file_delete("Work:slow") == AAR_E_TIMEDOUT);
	CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_BLOCKING) == ZZAPP_XPORT_REQ_FREE);
	cl.ignore = 0;
	cl.clock_step = 1000U;
	CHECK(zzapp_svc_dir_create("Work:next") == AAR_E_BUSY);
	CHECK(zzapp_svc_file_rename("a", "b", 0U) == AAR_E_BUSY);
	memset(&d, 0, sizeof(d));
	d.op = AAR_OP_FILE_DELETE;
	d.id = zzapp_xport_req_id(rec, ZZAPP_XPORT_REQ_BLOCKING);
	late = zzapp_xport_stats(rec)->late_dropped;
	client_post(&c, &d);
	CHECK(zzapp_svc_dir_create("Work:next") == AAR_OK);
	CHECK(zzapp_xport_stats(rec)->late_dropped == late + 1U);
	/* A stop ends the wait. */
	cl.ignore = 1;
	cl.stop_at_yield = cl.yields + 2U;
	CHECK(zzapp_svc_file_set_protect("Work:z", 1U) == AAR_E_IO);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
}

/* The clipboard the scripted client received (clip_sink, above). */

static void test_time_locale_env(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	zzapp_xport_buf_t data;
	static const uint8_t hello[] = "hello";
	char buf[64];
	char name255[256], name256[257];
	uint8_t *v;
	uint8_t *big;
	uint32_t n, before;
	int64_t us;
	aar_locale_info li;

	reset_all();
	rec = claim(&c, &handle);

	/* Time: an int64 in two words. */
	cl.time_us = 0x0000000123456789LL;
	CHECK(zzapp_svc_time_unix_us(&us) == AAR_OK && us == 0x0000000123456789LL);
	cl.time_us = -5;
	CHECK(zzapp_svc_time_unix_us(&us) == AAR_OK && us == -5);
	CHECK(zzapp_svc_time_unix_us(NULL) == AAR_E_INVAL);
	cl.result = AAR_E_NOSYS;
	us = 77;
	CHECK(zzapp_svc_time_unix_us(&us) == AAR_E_NOSYS && us == 77);
	cl.result = AAR_OK;

	/* Environment, a short value. The first request stages at most the
	 * scratch area, whatever the caller offers, so no data block is made
	 * for it. */
	cl.env_value = hello;
	cl.env_len = 5U;
	memset(buf, 'x', sizeof(buf));
	CHECK(zzapp_svc_env_get("PATH", buf, 0xffffffffU, &n) == AAR_OK);
	CHECK(n == 5U && strcmp(buf, "hello") == 0);
	CHECK(cl.nreq[AAR_OP_ENV_GET] == 1U && strcmp(cl.path, "hello") != 0);
	CHECK(cl.last[AAR_OP_ENV_GET].buf_len == ZZAPP_XPORT_SCRATCH_BYTES);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_DATA, &data) == 0);
	CHECK(zzapp_svc_env_get("PATH", buf, 64U, &n) == AAR_OK);
	CHECK(cl.last[AAR_OP_ENV_GET].buf_len == 64U);
	CHECK(zzapp_svc_env_get("PATH", buf, 6U, &n) == AAR_OK && n == 5U);
	CHECK(strcmp(buf, "hello") == 0);
	/* A value that does not fit with its NUL is AAR_E_NOSPC, the length is
	 * reported, and the caller's buffer is not touched. */
	memset(buf, 'x', sizeof(buf));
	CHECK(zzapp_svc_env_get("PATH", buf, 5U, &n) == AAR_E_NOSPC && n == 5U);
	CHECK(buf[0] == 'x');
	CHECK(zzapp_svc_env_get("PATH", buf, 3U, &n) == AAR_E_NOSPC && n == 5U);
	CHECK(zzapp_svc_env_get("PATH", NULL, 0U, &n) == AAR_E_NOSPC && n == 5U);
	CHECK(cl.last[AAR_OP_ENV_GET].buf_len == 5U);	/* the name and its NUL */
	/* Argument checks, before anything reaches the client. */
	before = cl.nreq[AAR_OP_ENV_GET];
	CHECK(zzapp_svc_env_get(NULL, buf, 8U, &n) == AAR_E_INVAL);
	CHECK(zzapp_svc_env_get("", buf, 8U, &n) == AAR_E_INVAL);
	CHECK(zzapp_svc_env_get("PATH", buf, 8U, NULL) == AAR_E_INVAL);
	CHECK(zzapp_svc_env_get("PATH", NULL, 8U, &n) == AAR_E_INVAL);
	memset(name256, 'n', 256);
	name256[256] = '\0';
	CHECK(zzapp_svc_env_get(name256, buf, 8U, &n) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_ENV_GET] == before);
	memcpy(name255, name256, 255);
	name255[255] = '\0';
	CHECK(zzapp_svc_env_get(name255, buf, 8U, &n) == AAR_OK);
	CHECK(cl.nreq[AAR_OP_ENV_GET] == before + 1U && strlen(cl.path) == 255U);
	cl.result = AAR_E_NOENT;
	n = 99U;
	CHECK(zzapp_svc_env_get("NOPE", buf, 8U, &n) == AAR_E_NOENT && n == 0U);
	cl.result = AAR_OK;

	/* Values beyond the scratch area: the first request learns the length,
	 * the second has room for the value and its NUL. */
	v = malloc(600000U);
	big = malloc(600000U);
	fill_pattern(v, 600000U);
	cl.env_value = v;
	cl.env_len = 5000U;
	before = cl.nreq[AAR_OP_ENV_GET];
	CHECK(zzapp_svc_env_get("BIG", (char *)big, 10000U, &n) == AAR_OK && n == 5000U);
	CHECK(cl.nreq[AAR_OP_ENV_GET] - before == 2U);
	CHECK(cl.last[AAR_OP_ENV_GET].buf_len == 5001U);
	CHECK(memcmp(big, v, 5000U) == 0 && big[5000] == 0U);
	before = cl.nreq[AAR_OP_ENV_GET];
	CHECK(zzapp_svc_env_get("BIG", (char *)big, 5000U, &n) == AAR_E_NOSPC && n == 5000U);
	CHECK(cl.nreq[AAR_OP_ENV_GET] - before == 1U);
	before = cl.nreq[AAR_OP_ENV_GET];
	CHECK(zzapp_svc_env_get("BIG", (char *)big, 5001U, &n) == AAR_OK && n == 5000U);
	CHECK(cl.nreq[AAR_OP_ENV_GET] - before == 2U && big[5000] == 0U);
	/* The largest value one request can carry is DATA_MAX - 1 bytes. */
	cl.env_len = ZZAPP_XPORT_DATA_MAX - 1U;
	before = cl.nreq[AAR_OP_ENV_GET];
	CHECK(zzapp_svc_env_get("BIG", (char *)big, 300000U, &n) == AAR_OK);
	CHECK(n == ZZAPP_XPORT_DATA_MAX - 1U && cl.nreq[AAR_OP_ENV_GET] - before == 2U);
	CHECK(cl.last[AAR_OP_ENV_GET].buf_len == ZZAPP_XPORT_DATA_MAX);
	CHECK(memcmp(big, v, n) == 0 && big[n] == 0U);
	/* One more byte cannot be carried: AAR_E_NOSPC with the full length,
	 * and nothing written, even for a caller that has room. */
	cl.env_len = ZZAPP_XPORT_DATA_MAX;
	memset(big, 0x5a, 16U);
	before = cl.nreq[AAR_OP_ENV_GET];
	CHECK(zzapp_svc_env_get("BIG", (char *)big, 400000U, &n) == AAR_E_NOSPC);
	CHECK(n == ZZAPP_XPORT_DATA_MAX && big[0] == 0x5aU);
	CHECK(cl.nreq[AAR_OP_ENV_GET] - before == 1U);
	cl.env_len = 600000U;
	CHECK(zzapp_svc_env_get("BIG", (char *)big, 0xffffffffU, &n) == AAR_E_NOSPC);
	CHECK(n == 600000U && big[0] == 0x5aU);
	free(v);
	free(big);

	/* Locale: a 64-byte record of big-endian words, unknown bits and
	 * missing NULs repaired here, the structure capped at the caller's
	 * size. */
	zzapp_put_be32(cl.locale_rec + AAR_LOCALE_REC_UTC_OFFSET_MIN, (uint32_t)-90);
	zzapp_put_be32(cl.locale_rec + AAR_LOCALE_REC_FLAGS, AAR_LOCALEF_OFFSET_KNOWN | 0x80U);
	memset(cl.locale_rec + AAR_LOCALE_REC_LANGUAGE, 'x', 32U);
	memcpy(cl.locale_rec + AAR_LOCALE_REC_COUNTRY, "GB", 2U);
	memcpy(cl.locale_rec + AAR_LOCALE_REC_DECIMAL_POINT, ",", 1U);
	memset(cl.locale_rec + AAR_LOCALE_REC_GROUP_SEPARATOR, 'y', 4U);
	memset(&li, 0xee, sizeof(li));
	li.size = sizeof(li);
	CHECK(zzapp_svc_locale_info(&li) == AAR_OK);
	CHECK(cl.last[AAR_OP_LOCALE_INFO].buf_len == AAR_LOCALE_REC_BYTES);
	CHECK(li.size == sizeof(li) && li.utc_offset_min == -90);
	CHECK(li.flags == AAR_LOCALEF_OFFSET_KNOWN);
	CHECK(strlen(li.language) == 31U && li.language[0] == 'x');
	CHECK(strcmp(li.country, "GB") == 0 && strcmp(li.decimal_point, ",") == 0);
	CHECK(strlen(li.group_separator) == 3U);
	CHECK(li.reserved[0] == 0U && li.reserved[2] == 0U);
	memset(&li, 0xee, sizeof(li));
	li.size = 4U;
	CHECK(zzapp_svc_locale_info(&li) == AAR_OK);
	CHECK(li.size == 4U && li.utc_offset_min == (int32_t)0xeeeeeeee);
	memset(&li, 0xee, sizeof(li));
	li.size = 0U;
	CHECK(zzapp_svc_locale_info(&li) == AAR_OK && li.size == 0U);
	CHECK(li.utc_offset_min == (int32_t)0xeeeeeeee && li.flags == 0xeeeeeeeeU);
	CHECK(zzapp_svc_locale_info(NULL) == AAR_E_INVAL);
	cl.result = AAR_E_ACCES;
	li.size = sizeof(li);
	CHECK(zzapp_svc_locale_info(&li) == AAR_E_ACCES && li.utc_offset_min == (int32_t)0xeeeeeeee);
	cl.result = AAR_OK;
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
}

static void test_clipboard(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	uint8_t *data;
	uint8_t *out;
	uint32_t n, before;

	reset_all();
	rec = claim(&c, &handle);
	(void)rec;
	data = malloc(700000U);
	out = malloc(700000U);
	fill_pattern(data, 700000U);
	cl.clip_data = data;
	cl.clip_len = 700000U;

	/* A clip larger than one chunk is read in chunks of at most
	 * ZZAPP_XPORT_DATA_MAX, the offset rising from 0. */
	memset(out, 0, 700000U);
	CHECK(zzapp_svc_clip_read(0U, AAR_CLIP_TEXT, out, 700000U, &n) == AAR_OK);
	CHECK(n == 700000U && memcmp(out, data, 700000U) == 0);
	CHECK(cl.nreq[AAR_OP_CLIP_READ] == 3U);
	CHECK(cl.clip_max_len == ZZAPP_XPORT_DATA_MAX);
	CHECK(cl.last[AAR_OP_CLIP_READ].arg[AAR_A_CLIP_OFFSET] == 2U * ZZAPP_XPORT_DATA_MAX);
	CHECK(cl.last[AAR_OP_CLIP_READ].buf_len == 700000U - 2U * ZZAPP_XPORT_DATA_MAX);
	/* A buffer that is too small: AAR_E_NOSPC, the full size, and the bytes
	 * that fit. */
	before = cl.nreq[AAR_OP_CLIP_READ];
	memset(out, 0, 2000U);
	CHECK(zzapp_svc_clip_read(255U, AAR_CLIP_IFF, out, 1000U, &n) == AAR_E_NOSPC);
	CHECK(n == 700000U && memcmp(out, data, 1000U) == 0 && out[1000] == 0U);
	CHECK(cl.nreq[AAR_OP_CLIP_READ] - before == 1U);
	CHECK(cl.last[AAR_OP_CLIP_READ].arg[AAR_A_CLIP_UNIT] == 255U);
	CHECK(cl.last[AAR_OP_CLIP_READ].arg[AAR_A_CLIP_TYPE] == AAR_CLIP_IFF);
	/* Count only: no buffer at all. */
	before = cl.nreq[AAR_OP_CLIP_READ];
	CHECK(zzapp_svc_clip_read(0U, AAR_CLIP_TEXT, NULL, 0U, &n) == AAR_E_NOSPC && n == 700000U);
	CHECK(cl.nreq[AAR_OP_CLIP_READ] - before == 1U);
	CHECK(cl.last[AAR_OP_CLIP_READ].buf == 0U && cl.last[AAR_OP_CLIP_READ].buf_len == 0U);
	/* A client that answers less than it was asked for is asked again at
	 * the new offset. */
	cl.clip_cap = 1000U;
	before = cl.nreq[AAR_OP_CLIP_READ];
	memset(out, 0, 4000U);
	CHECK(zzapp_svc_clip_read(0U, AAR_CLIP_TEXT, out, 3000U, &n) == AAR_E_NOSPC);
	CHECK(cl.nreq[AAR_OP_CLIP_READ] - before == 3U && n == 700000U);
	CHECK(memcmp(out, data, 3000U) == 0 && out[3000] == 0U);
	/* A client that stops answering bytes, or changes the clip's size under
	 * a sequence, breaks it. */
	cl.clip_stall = 1;
	CHECK(zzapp_svc_clip_read(0U, AAR_CLIP_TEXT, out, 3000U, &n) == AAR_E_IO);
	cl.clip_stall = 0;
	cl.clip_drift = 1;
	CHECK(zzapp_svc_clip_read(0U, AAR_CLIP_TEXT, out, 3000U, &n) == AAR_E_IO);
	cl.clip_drift = 0;
	cl.clip_cap = 0U;
	/* An empty clip, and the client's errors. */
	cl.clip_len = 0U;
	n = 99U;
	CHECK(zzapp_svc_clip_read(0U, AAR_CLIP_TEXT, out, 10U, &n) == AAR_OK && n == 0U);
	cl.result = AAR_E_NOENT;
	CHECK(zzapp_svc_clip_read(0U, AAR_CLIP_TEXT, out, 10U, &n) == AAR_E_NOENT && n == 0U);
	cl.result = AAR_OK;
	/* Unknown unit, unknown type, a missing buffer: nothing is sent. */
	before = cl.nreq[AAR_OP_CLIP_READ];
	CHECK(zzapp_svc_clip_read(256U, AAR_CLIP_TEXT, out, 10U, &n) == AAR_E_INVAL);
	CHECK(zzapp_svc_clip_read(0U, 0U, out, 10U, &n) == AAR_E_INVAL);
	CHECK(zzapp_svc_clip_read(0U, 3U, out, 10U, &n) == AAR_E_INVAL);
	CHECK(zzapp_svc_clip_read(0U, AAR_CLIP_TEXT, NULL, 10U, &n) == AAR_E_INVAL);
	CHECK(zzapp_svc_clip_read(0U, AAR_CLIP_TEXT, out, 10U, NULL) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_CLIP_READ] == before);

	/* Write: chunks of at most ZZAPP_XPORT_DATA_MAX, offsets rising from 0,
	 * the total in every one, and AAR_CLIPF_LAST on the final chunk only. */
	cl.clip_wr_chunks = 0U;
	cl.clip_committed = 0U;
	memset(clip_sink, 0, 700000U);
	CHECK(zzapp_svc_clip_write(3U, AAR_CLIP_TEXT, data, 600000U) == AAR_OK);
	CHECK(cl.clip_wr_chunks == 3U && cl.clip_committed == 1U);
	CHECK(cl.clip_wr_off[0] == 0U && cl.clip_wr_off[1] == ZZAPP_XPORT_DATA_MAX &&
	      cl.clip_wr_off[2] == 2U * ZZAPP_XPORT_DATA_MAX);
	CHECK(cl.clip_wr_len[0] == ZZAPP_XPORT_DATA_MAX && cl.clip_wr_len[1] == ZZAPP_XPORT_DATA_MAX);
	CHECK(cl.clip_wr_len[2] == 600000U - 2U * ZZAPP_XPORT_DATA_MAX);
	CHECK(cl.clip_wr_flags[0] == 0U && cl.clip_wr_flags[1] == 0U &&
	      cl.clip_wr_flags[2] == AAR_CLIPF_LAST);
	CHECK(cl.clip_wr_total[0] == 600000U && cl.clip_wr_total[2] == 600000U);
	CHECK(cl.last[AAR_OP_CLIP_WRITE].arg[AAR_A_CLIP_UNIT] == 3U);
	CHECK(memcmp(clip_sink, data, 600000U) == 0);
	/* Exactly one chunk is the last one. */
	cl.clip_wr_chunks = 0U;
	CHECK(zzapp_svc_clip_write(0U, AAR_CLIP_IFF, data, ZZAPP_XPORT_DATA_MAX) == AAR_OK);
	CHECK(cl.clip_wr_chunks == 1U && cl.clip_wr_flags[0] == AAR_CLIPF_LAST);
	/* An empty clip is one request, no buffer, and the last. */
	cl.clip_wr_chunks = 0U;
	CHECK(zzapp_svc_clip_write(0U, AAR_CLIP_TEXT, NULL, 0U) == AAR_OK);
	CHECK(cl.clip_wr_chunks == 1U && cl.clip_wr_flags[0] == AAR_CLIPF_LAST);
	CHECK(cl.clip_wr_len[0] == 0U && cl.clip_wr_total[0] == 0U);
	CHECK(cl.last[AAR_OP_CLIP_WRITE].buf == 0U);
	/* An error on a middle chunk ends the sequence before the last one: the
	 * client's clip is not replaced. */
	cl.clip_wr_chunks = 0U;
	cl.clip_committed = 0U;
	cl.clip_fail_chunk = 2U;
	CHECK(zzapp_svc_clip_write(0U, AAR_CLIP_TEXT, data, 600000U) == AAR_E_NOMEM);
	CHECK(cl.clip_wr_chunks == 2U && cl.clip_committed == 0U);
	CHECK(cl.clip_wr_flags[0] == 0U && cl.clip_wr_flags[1] == 0U);
	cl.clip_fail_chunk = 0U;
	/* A client that takes less than it was given breaks the sequence. */
	cl.clip_wr_chunks = 0U;
	cl.clip_take = 1U;
	CHECK(zzapp_svc_clip_write(0U, AAR_CLIP_TEXT, data, 100U) == AAR_E_IO);
	CHECK(cl.clip_wr_chunks == 1U);
	cl.clip_take = 0U;
	cl.clip_wr_chunks = 0U;
	CHECK(zzapp_svc_clip_write(256U, AAR_CLIP_TEXT, data, 10U) == AAR_E_INVAL);
	CHECK(zzapp_svc_clip_write(0U, 0U, data, 10U) == AAR_E_INVAL);
	CHECK(zzapp_svc_clip_write(0U, 3U, data, 10U) == AAR_E_INVAL);
	CHECK(zzapp_svc_clip_write(0U, AAR_CLIP_TEXT, NULL, 10U) == AAR_E_INVAL);
	CHECK(cl.clip_wr_chunks == 0U);
	free(data);
	free(out);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
}

/* ---- API 0.6: display --------------------------------------------------------------- */

#define FEATURE_CAPS (AAR_DISPCAP_WINDOW | AAR_DISPCAP_RESIZE | \
		      AAR_DISPCAP_POINTER_ABS | AAR_DISPCAP_TEXT_INPUT | \
		      AAR_DISPCAP_CLOSE_REQUEST | AAR_DISPCAP_SCALED)

/* The caps are what was asked for and what the client says it did, nothing
 * else; a window or a scaled surface is the client's, never scanned out. */
static void test_display_caps_v06(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_surface s;
	struct zzapp_service_runtime *st;
	uint32_t all = AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE |
		       AAR_DISPLAY_POINTER_ABS | AAR_DISPLAY_TEXT_INPUT |
		       AAR_DISPLAY_CLOSE_REQUEST | AAR_DISPLAY_SCALE;

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_SCANOUT_ACK | ZZAPP_CLIENT_LATEST;
	st = runtime_of(rec);
	cl.open_spare = 0x00600000U;

	/* An unknown flag bit is refused; the eight known ones are not. */
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0x100U, &s) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0x80000000U, &s) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_DISPLAY_OPEN] == 0U);

	/* Every feature asked for and granted; runtime-owned bits and junk in
	 * the reply are not the client's to grant. */
	cl.open_caps = FEATURE_CAPS | AAR_DISPCAP_LATEST | AAR_DISPCAP_CLUT8 | 0x8000U;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565,
				     all | AAR_DISPLAY_VSYNC | AAR_DISPLAY_LATEST, &s) == AAR_OK);
	CHECK(cl.last[AAR_OP_DISPLAY_OPEN].arg[AAR_A_DISPLAY_OPEN_FLAGS] ==
	      (all | AAR_DISPLAY_VSYNC | AAR_DISPLAY_LATEST));
	CHECK(s.caps == (AAR_DISPCAP_ASYNC | FEATURE_CAPS));
	CHECK(st->display_caps == FEATURE_CAPS);
	/* Never direct scanout, never card-owned, even with a client that
	 * offers both. */
	CHECK(st->display_scanout_ack == 0U && st->display_mailbox_active == 0);
	CHECK(zzapp_display_latest() == 0);
	CHECK(zzapp_svc_display_close(1U) == AAR_OK);
	CHECK(st->display_caps == 0U && st->display_flags == 0U);

	/* Asked for, not granted. */
	cl.open_caps = 0U;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, all, &s) == AAR_OK);
	CHECK(s.caps == AAR_DISPCAP_ASYNC && st->display_caps == 0U);
	CHECK(st->display_scanout_ack == 1U);
	CHECK(zzapp_svc_display_close(1U) == AAR_OK);
	/* Granted, not asked for. */
	cl.open_caps = FEATURE_CAPS;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0U, &s) == AAR_OK);
	CHECK(s.caps == AAR_DISPCAP_ASYNC);
	CHECK(zzapp_svc_display_close(1U) == AAR_OK);
	/* A resizable window and its close gadget need the window. */
	cl.open_caps = AAR_DISPCAP_WINDOW | AAR_DISPCAP_RESIZE | AAR_DISPCAP_CLOSE_REQUEST;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, AAR_DISPLAY_RESIZABLE |
				     AAR_DISPLAY_CLOSE_REQUEST, &s) == AAR_OK);
	CHECK(s.caps == AAR_DISPCAP_ASYNC);
	CHECK(zzapp_svc_display_close(1U) == AAR_OK);
	cl.open_caps = AAR_DISPCAP_RESIZE | AAR_DISPCAP_CLOSE_REQUEST;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW |
				     AAR_DISPLAY_RESIZABLE | AAR_DISPLAY_CLOSE_REQUEST, &s) == AAR_OK);
	CHECK(s.caps == AAR_DISPCAP_ASYNC);
	CHECK(zzapp_svc_display_close(1U) == AAR_OK);
	/* Scaled alone is the client's too. */
	cl.open_caps = AAR_DISPCAP_SCALED;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, AAR_DISPLAY_SCALE |
				     AAR_DISPLAY_LATEST, &s) == AAR_OK);
	CHECK(s.caps == (AAR_DISPCAP_ASYNC | AAR_DISPCAP_SCALED));
	CHECK(st->display_scanout_ack == 0U && zzapp_display_latest() == 0);
	CHECK(zzapp_svc_display_close(1U) == AAR_OK);
	/* A plain screen keeps both. */
	cl.open_caps = 0U;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, AAR_DISPLAY_LATEST, &s) == AAR_OK);
	CHECK((s.caps & AAR_DISPCAP_LATEST) && zzapp_display_latest() == 1);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(rec->hdr.pid);
	(void)mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL);
}

/* A window or scaled display completes a flip on the client's reply even
 * when the reply asks for a submitted pan: the card never scans it out, so
 * there is no blank to wait for. */
static void test_display_window_flip(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_surface s;
	struct zzapp_service_runtime *st;
	uint32_t flips;
	unsigned round;

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_SCANOUT_ACK | ZZAPP_CLIENT_LATEST;
	st = runtime_of(rec);
	cl.open_spare = 0x00600000U;
	for (round = 0U; round < 2U; round++) {
		cl.open_caps = round == 0U ? AAR_DISPCAP_WINDOW : AAR_DISPCAP_SCALED;
		CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565,
					     (round == 0U ? AAR_DISPLAY_WINDOW : AAR_DISPLAY_SCALE) |
					     AAR_DISPLAY_LATEST | AAR_DISPLAY_VSYNC, &s) == AAR_OK);
		CHECK(!(s.caps & AAR_DISPCAP_LATEST));
		flips = cl.nreq[AAR_OP_DISPLAY_FLIP];
		cl.flip_flags = ZZAPP_FLIPF_SUBMITTED;
		cl.flip_front = st->display_back;
		CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
		/* It went to the client, not to a card-owned mailbox, and nothing
		 * waits for the scanout. */
		CHECK(cl.nreq[AAR_OP_DISPLAY_FLIP] == flips + 1U);
		CHECK(st->display_flip_ack == st->display_flip_gen);
		CHECK(st->display_flip_submit == 0U && st->display_mailbox_active == 0);
		CHECK(zzapp_display_flip_idle() == 1);
		/* A blank at the address the client named does not matter. */
		zzapp_services_display_vblank(st->display_flip_address);
		CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
		CHECK(zzapp_svc_display_close(1U) == AAR_OK);
	}
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(rec->hdr.pid);
	(void)mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL);
}

/* aar_display_last_present_us: 0 until a frame is known to be shown, then the
 * time that was learnt; a submission, a timeout and an error are not it. */
static void test_display_present_time(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_surface s;
	struct zzapp_service_runtime *st;
	aar_completion d;
	uint64_t t1, t2;
	uint32_t scan = 0U;

	/* A window: the client's reply is the presentation. */
	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_SCANOUT_ACK;
	st = runtime_of(rec);
	CHECK(zzapp_svc_display_last_present_us() == 0U);
	cl.open_caps = AAR_DISPCAP_WINDOW;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW, &s) == AAR_OK);
	CHECK(zzapp_svc_display_last_present_us() == 0U);
	/* An error reply shows nothing. */
	cl.result = AAR_E_IO;
	CHECK(zzapp_svc_display_flip(1U) == AAR_E_IO);
	cl.result = AAR_OK;
	CHECK(zzapp_svc_display_last_present_us() == 0U);
	CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
	t1 = zzapp_svc_display_last_present_us();
	CHECK(t1 > 1000000U && t1 <= fake_now);
	/* An async flip the client has not answered is not shown yet. */
	cl.ignore = 1;
	fake_now += 50000U;
	CHECK(zzapp_svc_display_flip_async(1U) == AAR_OK);
	client_service(&c);
	CHECK(zzapp_display_flip_idle() == 0);
	CHECK(zzapp_svc_display_last_present_us() == t1);
	cl.ignore = 0;
	fake_now += 50000U;
	memset(&d, 0, sizeof(d));
	d.op = AAR_OP_DISPLAY_FLIP;
	d.id = rec->xport.req[ZZAPP_XPORT_REQ_FLIP].id;
	client_post(&c, &d);
	CHECK(zzapp_display_flip_idle() == 1);
	t2 = zzapp_svc_display_last_present_us();
	CHECK(t2 >= t1 + 100000U);
	/* A new launch starts again from 0. */
	zzapp_services_on_launch();
	CHECK(zzapp_svc_display_last_present_us() == 0U);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(rec->hdr.pid);
	(void)mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL);

	/* A screen with scanout acknowledgement: submitting is not showing. The
	 * firmware's 100 ms timeout lets the app go and is not showing either;
	 * the blank that sees the address is. */
	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_SCANOUT_ACK;
	st = runtime_of(rec);
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0U, &s) == AAR_OK);
	cl.flip_flags = ZZAPP_FLIPF_SUBMITTED;
	cl.no_vblank = 1;
	cl.clock_step = 30000U;
	CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
	CHECK(st->display_flip_ack == st->display_flip_gen);
	CHECK(zzapp_svc_display_last_present_us() == 0U);
	cl.no_vblank = 0;
	cl.clock_step = 1000U;
	CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
	t1 = zzapp_svc_display_last_present_us();
	CHECK(t1 != 0U && t1 <= fake_now);
	/* The blank itself, from the interrupt, stamps the time it ran. */
	cl.ignore = 1;
	CHECK(zzapp_svc_display_flip_async(1U) == AAR_OK);
	memset(&d, 0, sizeof(d));
	d.op = AAR_OP_DISPLAY_FLIP;
	d.id = rec->xport.req[ZZAPP_XPORT_REQ_FLIP].id;
	d.flags = ZZAPP_FLIPF_SUBMITTED;
	client_post(&c, &d);
	CHECK(zzapp_display_flip_idle() == 0);
	CHECK(zzapp_svc_display_last_present_us() == t1);
	fake_now += 16000U;
	zzapp_services_display_vblank(st->display_flip_address);
	CHECK(zzapp_svc_display_last_present_us() == fake_now);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(rec->hdr.pid);
	(void)mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL);

	/* A card-owned (LATEST) frame is shown when the blank latches it, not
	 * when it is queued, and not when a latch is abandoned. */
	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_LATEST;
	st = runtime_of(rec);
	cl.open_spare = 0x00600000U;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, AAR_DISPLAY_LATEST, &s) == AAR_OK);
	CHECK(zzapp_display_latest() == 1);
	CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
	CHECK(zzapp_svc_display_last_present_us() == 0U);
	CHECK(zzapp_services_display_mailbox_begin(0x00400000U, &scan) == 1);
	zzapp_services_display_mailbox_end(0);
	CHECK(zzapp_svc_display_last_present_us() == 0U);
	fake_now += 7000U;
	CHECK(zzapp_services_display_mailbox_begin(0x00400000U, &scan) == 1);
	zzapp_services_display_mailbox_end(1);
	CHECK(zzapp_svc_display_last_present_us() == fake_now);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(rec->hdr.pid);
	(void)mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL);
}

static void test_display_info_modes(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_surface s;
	aar_display_info di;
	aar_display_mode ms[100];
	aar_display_mode *big;
	uint32_t n, before;

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);

	/* Modes need no display; the first request holds at most 42 records. */
	cl.mode_total = 100U;
	memset(ms, 0xee, sizeof(ms));
	before = cl.nreq[AAR_OP_DISPLAY_MODES];
	CHECK(zzapp_svc_display_modes(ms, 10U, &n) == AAR_E_NOSPC && n == 100U);
	CHECK(cl.nreq[AAR_OP_DISPLAY_MODES] - before == 1U);
	CHECK(cl.last[AAR_OP_DISPLAY_MODES].buf_len == 240U);
	CHECK(ms[0].width == 100U && ms[0].height == 200U && ms[0].refresh_mhz == 50000U);
	CHECK(ms[0].formats == ((1U << AAR_FMT_RGB565) | (1U << AAR_FMT_BGRA8888) | (1U << AAR_FMT_CLUT8)));
	CHECK(ms[0].flags == AAR_MODEF_PREFERRED && ms[0].reserved == 0U);
	CHECK(ms[9].width == 109U && ms[10].width == 0xeeeeeeeeU);
	/* A caller with room for them all has them all, in a second request. */
	before = cl.nreq[AAR_OP_DISPLAY_MODES];
	CHECK(zzapp_svc_display_modes(ms, 100U, &n) == AAR_OK && n == 100U);
	CHECK(cl.nreq[AAR_OP_DISPLAY_MODES] - before == 2U);
	CHECK(cl.last[AAR_OP_DISPLAY_MODES].buf_len == 2400U && ms[99].width == 199U);
	/* A huge `max` stages what the client has, never more than a data
	 * block of records. */
	big = malloc(20000U * sizeof(*big));
	cl.mode_total = 5U;
	before = cl.nreq[AAR_OP_DISPLAY_MODES];
	CHECK(zzapp_svc_display_modes(big, 20000U, &n) == AAR_OK && n == 5U);
	CHECK(cl.nreq[AAR_OP_DISPLAY_MODES] - before == 1U);
	cl.mode_total = 600U;
	before = cl.nreq[AAR_OP_DISPLAY_MODES];
	CHECK(zzapp_svc_display_modes(big, 0xffffffffU, &n) == AAR_OK && n == 600U);
	CHECK(cl.nreq[AAR_OP_DISPLAY_MODES] - before == 2U);
	CHECK(cl.last[AAR_OP_DISPLAY_MODES].buf_len == 600U * 24U && big[599].width == 699U);
	cl.mode_total = 11000U;
	cl.mode_max_len = 0U;
	CHECK(zzapp_svc_display_modes(big, 20000U, &n) == AAR_E_NOSPC && n == 11000U);
	CHECK(cl.mode_max_len == (ZZAPP_XPORT_DATA_MAX / 24U) * 24U);
	CHECK(cl.mode_max_len <= ZZAPP_XPORT_DATA_MAX);
	CHECK(big[ZZAPP_XPORT_DATA_MAX / 24U - 1U].width == 100U + ZZAPP_XPORT_DATA_MAX / 24U - 1U);
	free(big);
	/* Count only. */
	cl.mode_total = 5U;
	CHECK(zzapp_svc_display_modes(NULL, 0U, &n) == AAR_E_NOSPC && n == 5U);
	CHECK(cl.last[AAR_OP_DISPLAY_MODES].buf == 0U && cl.last[AAR_OP_DISPLAY_MODES].buf_len == 0U);
	cl.mode_total = 0U;
	CHECK(zzapp_svc_display_modes(NULL, 0U, &n) == AAR_OK && n == 0U);
	before = cl.nreq[AAR_OP_DISPLAY_MODES];
	CHECK(zzapp_svc_display_modes(NULL, 3U, &n) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_modes(ms, 3U, NULL) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_DISPLAY_MODES] == before);

	/* Info: the surface size is the runtime's, WINDOW and SCALED are what
	 * was granted, and ACTIVE is the only bit the client adds. */
	cl.info[0] = 111U;
	cl.info[1] = 222U;
	cl.info[2] = 60000U;
	cl.info[3] = 800U;
	cl.info[4] = 600U;
	cl.info[5] = 0xffU;
	memset(&di, 0xee, sizeof(di));
	di.size = sizeof(di);
	CHECK(zzapp_svc_display_info(1U, &di) == AAR_E_INVAL);	/* no display */
	cl.open_caps = AAR_DISPCAP_WINDOW | AAR_DISPCAP_SCALED;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565,
				     AAR_DISPLAY_WINDOW | AAR_DISPLAY_SCALE, &s) == AAR_OK);
	CHECK(zzapp_svc_display_info(2U, &di) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_info(1U, NULL) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_DISPLAY_INFO] == 0U);
	CHECK(zzapp_svc_display_info(1U, &di) == AAR_OK);
	CHECK(cl.last[AAR_OP_DISPLAY_INFO].arg[AAR_A_DISPLAY_ID] == 7U);
	CHECK(di.size == sizeof(di) && di.width == 640U && di.height == 480U);
	CHECK(di.refresh_mhz == 60000U && di.screen_width == 800U && di.screen_height == 600U);
	CHECK(di.flags == (AAR_DISPINFO_ACTIVE | AAR_DISPINFO_WINDOW | AAR_DISPINFO_SCALED));
	CHECK(di.reserved[0] == 0U && di.reserved[4] == 0U);
	memset(&di, 0xee, sizeof(di));
	di.size = 12U;
	CHECK(zzapp_svc_display_info(1U, &di) == AAR_OK);
	CHECK(di.size == 12U && di.height == 480U && di.refresh_mhz == 0xeeeeeeeeU);
	cl.result = AAR_E_NOSYS;
	CHECK(zzapp_svc_display_info(1U, &di) == AAR_E_NOSYS);
	cl.result = AAR_OK;
	CHECK(zzapp_svc_display_close(1U) == AAR_OK);
	cl.open_caps = 0U;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0U, &s) == AAR_OK);
	di.size = sizeof(di);
	CHECK(zzapp_svc_display_info(1U, &di) == AAR_OK && di.flags == AAR_DISPINFO_ACTIVE);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(rec->hdr.pid);
	(void)mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL);
}

static void test_display_title_pointer(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_surface s;
	char t255[256], t256[257];
	uint32_t px[64 * 64];
	aar_pointer p;
	uint32_t before, i;

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	memset(t255, 'T', 255);
	t255[255] = '\0';
	memset(t256, 'T', 256);
	t256[256] = '\0';
	CHECK(zzapp_svc_display_set_title(1U, "x") == AAR_E_INVAL);	/* no display */
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_HIDDEN, NULL) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0U, &s) == AAR_OK);

	CHECK(zzapp_svc_display_set_title(1U, "My Window") == AAR_OK);
	CHECK(strcmp(cl.title, "My Window") == 0);
	CHECK(cl.last[AAR_OP_DISPLAY_TITLE].arg[AAR_A_DISPLAY_ID] == 7U);
	CHECK(cl.last[AAR_OP_DISPLAY_TITLE].buf_len == 10U);
	CHECK(zzapp_svc_display_set_title(1U, "") == AAR_OK);
	CHECK(cl.last[AAR_OP_DISPLAY_TITLE].buf_len == 1U);
	CHECK(zzapp_svc_display_set_title(1U, t255) == AAR_OK);
	CHECK(cl.last[AAR_OP_DISPLAY_TITLE].buf_len == 256U && strlen(cl.title) == 255U);
	before = cl.nreq[AAR_OP_DISPLAY_TITLE];
	CHECK(zzapp_svc_display_set_title(1U, t256) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_set_title(1U, NULL) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_set_title(2U, "x") == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_DISPLAY_TITLE] == before);

	/* Pointer modes. The image is read only for CUSTOM. */
	before = cl.nreq[AAR_OP_DISPLAY_POINTER];
	CHECK(zzapp_svc_display_pointer(1U, 4U, NULL) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_pointer(2U, AAR_POINTER_HIDDEN, NULL) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_DISPLAY_POINTER] == before);
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_BUSY, NULL) == AAR_OK);
	CHECK(cl.last[AAR_OP_DISPLAY_POINTER].arg[AAR_A_DISPLAY_ID] == 7U);
	CHECK(cl.last[AAR_OP_DISPLAY_POINTER].arg[AAR_A_DISPLAY_POINTER_MODE] == AAR_POINTER_BUSY);
	CHECK(cl.last[AAR_OP_DISPLAY_POINTER].buf == 0U);
	memset(&p, 0, sizeof(p));
	p.width = 99U;
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_DEFAULT, &p) == AAR_OK);
	CHECK(cl.last[AAR_OP_DISPLAY_POINTER].arg[AAR_A_DISPLAY_POINTER_W] == 0U);
	for (i = 0U; i < 64U * 64U; i++)
		px[i] = 0xff000000U | i;
	before = cl.nreq[AAR_OP_DISPLAY_POINTER];
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_CUSTOM, NULL) == AAR_E_INVAL);
	memset(&p, 0, sizeof(p));
	p.width = 2U;
	p.height = 3U;
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_CUSTOM, &p) == AAR_E_INVAL);	/* no pixels */
	p.argb = px;
	p.width = 0U;
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_CUSTOM, &p) == AAR_E_INVAL);
	p.width = 65U;
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_CUSTOM, &p) == AAR_E_INVAL);
	p.width = 2U;
	p.height = 65U;
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_CUSTOM, &p) == AAR_E_INVAL);
	p.height = 0U;
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_CUSTOM, &p) == AAR_E_INVAL);
	p.height = 3U;
	p.hot_x = -1;
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_CUSTOM, &p) == AAR_E_INVAL);
	p.hot_x = 2;
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_CUSTOM, &p) == AAR_E_INVAL);
	p.hot_x = 1;
	p.hot_y = 3;
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_CUSTOM, &p) == AAR_E_INVAL);
	p.hot_y = -1;
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_CUSTOM, &p) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_DISPLAY_POINTER] == before);
	/* A good image goes out as big-endian words. */
	p.hot_y = 2;
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_CUSTOM, &p) == AAR_OK);
	CHECK(cl.last[AAR_OP_DISPLAY_POINTER].arg[AAR_A_DISPLAY_POINTER_MODE] == AAR_POINTER_CUSTOM);
	CHECK(cl.last[AAR_OP_DISPLAY_POINTER].arg[AAR_A_DISPLAY_POINTER_W] == 2U);
	CHECK(cl.last[AAR_OP_DISPLAY_POINTER].arg[AAR_A_DISPLAY_POINTER_H] == 3U);
	CHECK(cl.last[AAR_OP_DISPLAY_POINTER].arg[AAR_A_DISPLAY_POINTER_HOT_X] == 1U);
	CHECK(cl.last[AAR_OP_DISPLAY_POINTER].arg[AAR_A_DISPLAY_POINTER_HOT_Y] == 2U);
	CHECK(cl.ptr_len == 24U && cl.ptr_words[0] == px[0] && cl.ptr_words[1] == px[1]);
	CHECK(cl.ptr_last == px[5]);
	/* The largest image, 16 KiB, through the data block. */
	p.width = 64U;
	p.height = 64U;
	p.hot_x = 63;
	p.hot_y = 63;
	CHECK(zzapp_svc_display_pointer(1U, AAR_POINTER_CUSTOM, &p) == AAR_OK);
	CHECK(cl.ptr_len == 16384U && cl.ptr_last == px[4095]);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(rec->hdr.pid);
	(void)mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL);
}

/* A window or scaled display is the client's: the request goes to the
 * client, its answers pass through, and the card's own blank, which this
 * screen does not show, never ends the wait. */
static void test_display_wait_vblank(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_surface s;
	struct zzapp_service_runtime *st;
	uint64_t t0;
	uint64_t dt;
	unsigned round;

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	st = runtime_of(rec);
	CHECK(zzapp_svc_display_wait_vblank(1000U) == AAR_E_INVAL);	/* no display */
	for (round = 0U; round < 2U; round++) {
		cl.open_caps = round == 0U ? AAR_DISPCAP_WINDOW : AAR_DISPCAP_SCALED;
		CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565,
					     round == 0U ? AAR_DISPLAY_WINDOW : AAR_DISPLAY_SCALE,
					     &s) == AAR_OK);
		cl.nreq[AAR_OP_DISPLAY_VBLANK] = 0U;
		CHECK(zzapp_svc_display_wait_vblank(0U) == AAR_E_INVAL);
		CHECK(cl.nreq[AAR_OP_DISPLAY_VBLANK] == 0U);
		CHECK(zzapp_svc_display_wait_vblank(50000U) == AAR_OK);
		CHECK(cl.nreq[AAR_OP_DISPLAY_VBLANK] == 1U);
		CHECK(cl.last[AAR_OP_DISPLAY_VBLANK].arg[AAR_A_DISPLAY_ID] == 7U);
		CHECK(cl.last[AAR_OP_DISPLAY_VBLANK].arg[AAR_A_DISPLAY_VBLANK_TIMEOUT_US] == 50000U);
		/* The client's own results pass through. */
		cl.vblank_result = AAR_E_TIMEDOUT;
		CHECK(zzapp_svc_display_wait_vblank(50000U) == AAR_E_TIMEDOUT);
		cl.vblank_result = AAR_E_NOSYS;
		CHECK(zzapp_svc_display_wait_vblank(50000U) == AAR_E_NOSYS);
		cl.vblank_result = AAR_E_IO;
		CHECK(zzapp_svc_display_wait_vblank(50000U) == AAR_E_IO);
		cl.vblank_result = AAR_OK;
		/* A client that never answers: AAR_E_TIMEDOUT, not before the
		 * timeout the app gave and not long after it, whatever the card's
		 * blank does meanwhile. */
		cl.vblank_silent = 1;
		cl.clock_step = 1000U;
		cl.blank_at_yield = cl.yields + 1U;
		cl.blank_addr = st->display_front;
		t0 = fake_now;
		CHECK(zzapp_svc_display_wait_vblank(50000U) == AAR_E_TIMEDOUT);
		dt = fake_now - t0;
		CHECK(dt >= 50000U && dt < 50000U + ZZAPP_VBLANK_GRACE_US + 3U * cl.clock_step);
		CHECK(st->vblank_seq == 0U && st->vblank_waiting == 0U);
		CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_BLOCKING) == ZZAPP_XPORT_REQ_FREE);
		CHECK(st->wait_kind == ZZAPP_WAIT_NONE);
		cl.blank_at_yield = 0U;
		/* The next wait is served as usual. */
		cl.vblank_silent = 0;
		CHECK(zzapp_svc_display_wait_vblank(50000U) == AAR_OK);
		/* A stop ends the wait with AAR_E_IO. */
		cl.vblank_silent = 1;
		cl.stop_at_yield = cl.yields + 2U;
		CHECK(zzapp_svc_display_wait_vblank(5000000U) == AAR_E_IO);
		CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_BLOCKING) == ZZAPP_XPORT_REQ_FREE);
		cl.vblank_silent = 0;
		cl.stop_at_yield = 0U;
		rec->host.stop_requested = 0U;
		/* Nothing here counts as a presentation. */
		CHECK(zzapp_svc_display_last_present_us() == 0U);
		CHECK(zzapp_svc_display_close(1U) == AAR_OK);
	}
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(rec->hdr.pid);
	(void)mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL);
}

/* A display the card scans out directly is waited on natively: the blank
 * callbacks of video.c end the wait, but only a blank of this display's own
 * screen; the client is never asked. */
static void test_display_native_vblank(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_surface s;
	struct zzapp_service_runtime *st;
	uint64_t t0;
	uint64_t dt;
	uint32_t rings;

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_SCANOUT_ACK;
	st = runtime_of(rec);
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, 0U, &s) == AAR_OK);
	CHECK(zzapp_svc_display_wait_vblank(0U) == AAR_E_INVAL);

	/* With nobody waiting a blank costs this record nothing. */
	rings = zzapp_services_test_core1_doorbells();
	zzapp_services_display_vblank(0x00400000U);
	CHECK(st->vblank_seq == 0U && zzapp_services_test_core1_doorbells() == rings);

	/* A blank of this screen (either page of it) ends the wait, with the
	 * doorbell for the parked app; the client is not asked. */
	cl.clock_step = 1000U;
	cl.blank_at_yield = cl.yields + 3U;
	cl.blank_addr = 0x00500000U;
	CHECK(zzapp_svc_display_wait_vblank(50000U) == AAR_OK);
	CHECK(cl.nreq[AAR_OP_DISPLAY_VBLANK] == 0U);
	CHECK(st->vblank_seq == 1U && st->vblank_waiting == 0U && st->wait_kind == ZZAPP_WAIT_NONE);
	CHECK(zzapp_services_test_core1_doorbells() == rings + 1U);
	CHECK(zzapp_svc_display_last_present_us() == 0U);	/* a blank is no flip */
	zzapp_services_display_vblank(0x00400000U);	/* nobody waits again */
	CHECK(st->vblank_seq == 1U);

	/* The blank of another screen, or none, confirms nothing: the wait ends
	 * at its timeout, not before and not long after, and not as success. */
	cl.blank_at_yield = cl.yields + 1U;
	cl.blank_addr = 0x12345678U;
	t0 = fake_now;
	CHECK(zzapp_svc_display_wait_vblank(20000U) == AAR_E_TIMEDOUT);
	dt = fake_now - t0;
	CHECK(dt >= 20000U && dt < 20000U + 3U * cl.clock_step);
	cl.blank_at_yield = cl.yields + 1U;
	cl.blank_addr = 0U;
	CHECK(zzapp_svc_display_wait_vblank(20000U) == AAR_E_TIMEDOUT);
	cl.blank_at_yield = 0U;
	CHECK(zzapp_svc_display_wait_vblank(20000U) == AAR_E_TIMEDOUT);
	CHECK(st->vblank_seq == 1U && st->vblank_waiting == 0U && st->wait_kind == ZZAPP_WAIT_NONE);
	CHECK(cl.nreq[AAR_OP_DISPLAY_VBLANK] == 0U);

	/* A stop ends it; a blank that lands with the stop is reported first. */
	cl.stop_at_yield = cl.yields + 2U;
	CHECK(zzapp_svc_display_wait_vblank(5000000U) == AAR_E_IO);
	CHECK(st->vblank_waiting == 0U && st->wait_kind == ZZAPP_WAIT_NONE);
	rec->host.stop_requested = 0U;
	cl.stop_at_yield = cl.yields + 1U;
	cl.blank_at_yield = cl.yields + 1U;
	cl.blank_addr = 0x00400000U;
	CHECK(zzapp_svc_display_wait_vblank(5000000U) == AAR_OK);
	cl.blank_at_yield = 0U;
	cl.stop_at_yield = 0U;
	rec->host.stop_requested = 0U;

	/* The system pass wakes a parked waiter at the blank and not before. */
	st->vblank_waiting = 1U;
	st->wait_kind = ZZAPP_WAIT_FLIP;
	st->wait_value = st->vblank_seq;
	st->wait_deadline = 0U;
	slot_state_lock_acquire();
	set_add(&set_svc, rec->hdr.pid);
	slot_state_lock_release();
	service_test_wake_count = 0U;
	zzapp_services_poll();
	CHECK(service_test_wake_count == 0U);
	zzapp_services_display_vblank(0x12345678U);
	zzapp_services_poll();
	CHECK(service_test_wake_count == 0U);
	zzapp_services_display_vblank(0x00400000U);
	zzapp_services_poll();
	CHECK(service_test_wake_count == 1U);
	st->vblank_waiting = 0U;
	st->wait_kind = ZZAPP_WAIT_NONE;
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(rec->hdr.pid);
	(void)mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL);

	/* A card-owned (LATEST) display: the blank the mailbox matched to its
	 * buffers counts, with or without a frame to latch; a blank whose
	 * address is not its buffers, or whose latch failed, does not. */
	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_LATEST;
	st = runtime_of(rec);
	cl.open_spare = 0x00600000U;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, AAR_DISPLAY_LATEST, &s) == AAR_OK);
	CHECK(zzapp_display_latest() == 1);
	cl.clock_step = 1000U;
	cl.blank_mailbox = 1;
	cl.blank_addr = 0x00400000U;
	cl.blank_at_yield = cl.yields + 2U;
	CHECK(zzapp_svc_display_wait_vblank(50000U) == AAR_OK);
	CHECK(st->vblank_seq == 1U && cl.nreq[AAR_OP_DISPLAY_VBLANK] == 0U);
	cl.blank_addr = 0x09000000U;
	cl.blank_at_yield = cl.yields + 1U;
	CHECK(zzapp_svc_display_wait_vblank(20000U) == AAR_E_TIMEDOUT);
	cl.blank_mailbox = 2;
	cl.blank_addr = 0x00400000U;
	cl.blank_at_yield = cl.yields + 1U;
	CHECK(zzapp_svc_display_wait_vblank(20000U) == AAR_E_TIMEDOUT);
	CHECK(st->vblank_seq == 1U && st->vblank_waiting == 0U);
	/* A latched frame is a presentation as well as a blank. */
	CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
	cl.blank_mailbox = 1;
	cl.blank_at_yield = cl.yields + 1U;
	CHECK(zzapp_svc_display_wait_vblank(50000U) == AAR_OK);
	CHECK(st->vblank_seq == 2U && zzapp_svc_display_last_present_us() != 0U);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(rec->hdr.pid);
	(void)mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL);
}

static void test_display_resize(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_surface s, s2, s3;
	struct zzapp_service_runtime *st;
	aar_completion d;
	uint32_t old_front, old_back, old_front_alias, before, close_before;
	uint32_t pal[4] = { 0x00112233U, 0x00445566U, 0x00778899U, 0x00aabbccU };

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	st = runtime_of(rec);

	/* Without the granted cap: AAR_E_NOSYS, nothing sent. */
	CHECK(zzapp_svc_display_resize(1U, 800U, 600U, &s2) == AAR_E_INVAL);	/* no display */
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565, AAR_DISPLAY_RESIZABLE, &s) == AAR_OK);
	CHECK(zzapp_svc_display_resize(1U, 800U, 600U, &s2) == AAR_E_NOSYS);
	CHECK(cl.nreq[AAR_OP_DISPLAY_RESIZE] == 0U);
	CHECK(zzapp_svc_display_close(1U) == AAR_OK);

	cl.open_caps = AAR_DISPCAP_WINDOW | AAR_DISPCAP_RESIZE;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565,
				     AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE, &s) == AAR_OK);
	CHECK((s.caps & AAR_DISPCAP_RESIZE) && (s.caps & AAR_DISPCAP_WINDOW));
	old_front = st->display_front;
	old_back = st->display_back;
	old_front_alias = st->display_alias[0];
	CHECK(old_front == 0x00400000U && old_back == 0x00500000U);
	CHECK(zzapp_as_alias_phys(&rec->proc.as, old_front_alias) == old_front);

	/* Arguments. */
	CHECK(zzapp_svc_display_resize(2U, 800U, 600U, &s2) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_resize(1U, 0U, 600U, &s2) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_resize(1U, 800U, 0U, &s2) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_resize(1U, AAR_DISPLAY_MAX_DIM + 1U, 600U, &s2) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_resize(1U, 800U, AAR_DISPLAY_MAX_DIM + 1U, &s2) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_resize(1U, 800U, 600U, NULL) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_DISPLAY_RESIZE] == 0U);

	/* A flip pending. */
	cl.ignore = 1;
	CHECK(zzapp_svc_display_flip_async(1U) == AAR_OK);
	client_service(&c);
	CHECK(zzapp_svc_display_resize(1U, 800U, 600U, &s2) == AAR_E_BUSY);
	CHECK(cl.nreq[AAR_OP_DISPLAY_RESIZE] == 0U);
	cl.ignore = 0;
	memset(&d, 0, sizeof(d));
	d.op = AAR_OP_DISPLAY_FLIP;
	d.id = rec->xport.req[ZZAPP_XPORT_REQ_FLIP].id;
	client_post(&c, &d);
	CHECK(zzapp_display_flip_idle() == 1);
	old_front = st->display_front;
	old_back = st->display_back;
	old_front_alias = st->display_alias[0];

	/* The client refuses: the surface is exactly as it was. */
	cl.resize_result = AAR_E_NOMEM;
	CHECK(zzapp_svc_display_resize(1U, 800U, 600U, &s2) == AAR_E_NOMEM);
	CHECK(cl.nreq[AAR_OP_DISPLAY_RESIZE] == 1U);
	CHECK(cl.last[AAR_OP_DISPLAY_RESIZE].arg[AAR_A_DISPLAY_ID] == 7U);
	CHECK(cl.last[AAR_OP_DISPLAY_RESIZE].arg[AAR_A_DISPLAY_RESIZE_W] == 800U);
	CHECK(cl.last[AAR_OP_DISPLAY_RESIZE].arg[AAR_A_DISPLAY_RESIZE_H] == 600U);
	CHECK(st->display_bound == 1 && st->display_width == 640U && st->display_height == 480U);
	CHECK(st->display_front == old_front && st->display_back == old_back);
	CHECK(st->display_client_id == 7U && st->display_caps == (AAR_DISPCAP_WINDOW | AAR_DISPCAP_RESIZE));
	CHECK(st->display_alias[0] == old_front_alias);
	CHECK(zzapp_as_alias_phys(&rec->proc.as, old_front_alias) == 0x00400000U);
	memset(&s3, 0, sizeof(s3));
	s3.id = 1U;
	CHECK(zzapp_svc_display_refresh(&s3) == AAR_OK);
	CHECK(s3.width == 640U && s3.height == 480U && s3.pitch == 1280U);
	CHECK(s3.front == s.front || s3.front == s.back);
	CHECK(zzapp_svc_display_flip(1U) == AAR_OK);	/* still usable */
	old_front = st->display_front;
	old_back = st->display_back;
	cl.resize_result = AAR_OK;

	/* The client reallocates: the old mappings go, the new ones come, and
	 * the geometry is the reply's. */
	cl.resize_front = 0x00600000U;
	cl.resize_back = 0x00700000U;
	cl.resize_pitch = 1600U;
	CHECK(zzapp_svc_display_resize(1U, 800U, 600U, &s2) == AAR_OK);
	CHECK(s2.id == 1U && s2.width == 800U && s2.height == 600U && s2.pitch == 1600U);
	CHECK(s2.format == AAR_FMT_RGB565 && s2.layout == AAR_LAYOUT_RGB565_BE);
	CHECK((s2.caps & AAR_DISPCAP_RESIZE) && (s2.caps & AAR_DISPCAP_WINDOW));
	CHECK(s2.front != NULL && s2.back != NULL);
	CHECK(st->display_front == 0x00600000U && st->display_back == 0x00700000U);
	CHECK(st->display_width == 800U && st->display_height == 600U);
	CHECK(st->display_pitch == 1600U && st->display_bytes == 1600U * 600U);
	CHECK(st->display_client_id == 9U && st->display_id == 1U);
	CHECK(zzapp_as_alias_phys(&rec->proc.as, (uint32_t)(uintptr_t)s2.front) == 0x00600000U);
	CHECK(zzapp_as_alias_phys(&rec->proc.as, (uint32_t)(uintptr_t)s2.back) == 0x00700000U);
	/* Nothing maps the old bitmaps any more. */
	CHECK(zzapp_as_alias_phys(&rec->proc.as, old_front_alias) != old_front);
	CHECK(zzapp_as_alias_phys(&rec->proc.as, old_front_alias) != old_back);
	CHECK(st->display_flip_gen == 0U && zzapp_display_flip_idle() == 1);
	CHECK(set_display.count == 1U && st->display_bound == 1);
	cl.flip_flags = 0U;
	CHECK(zzapp_svc_display_flip(1U) == AAR_OK);
	CHECK(cl.last[AAR_OP_DISPLAY_FLIP].arg[AAR_A_DISPLAY_ID] == 9U);

	/* A reply that cannot be used leaves no display: the client is told to
	 * close its screen, nothing stays mapped. */
	close_before = cl.nreq[AAR_OP_DISPLAY_CLOSE];
	cl.resize_front = 0x0c000000U;
	CHECK(zzapp_svc_display_resize(1U, 1024U, 768U, &s3) == AAR_E_INVAL);
	CHECK(cl.nreq[AAR_OP_DISPLAY_CLOSE] == close_before + 1U);
	CHECK(cl.last[AAR_OP_DISPLAY_CLOSE].arg[0] == 9U);
	CHECK(st->display_bound == 0 && set_display.count == 0U);
	CHECK(st->display_caps == 0U && st->display_grant_bytes[0] == 0U &&
	      st->display_grant_bytes[1] == 0U);
	CHECK(zzapp_as_alias_phys(&rec->proc.as, (uint32_t)(uintptr_t)s2.front) == 0U);
	CHECK(zzapp_as_alias_phys(&rec->proc.as, (uint32_t)(uintptr_t)s2.back) == 0U);
	CHECK(zzapp_svc_display_flip(1U) == AAR_E_INVAL);
	CHECK(zzapp_svc_display_resize(1U, 800U, 600U, &s3) == AAR_E_INVAL);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(rec->hdr.pid);
	(void)mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL);

	/* A CLUT8 screen the client replaced starts with its own colours: the
	 * app's are sent again. */
	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	st = runtime_of(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_CLUT8;
	cl.open_caps = AAR_DISPCAP_WINDOW | AAR_DISPCAP_RESIZE;
	cl.open_pitch = 320U;
	cl.open_layout = AAR_LAYOUT_CLUT8;
	CHECK(zzapp_svc_display_open(320U, 200U, AAR_FMT_CLUT8,
				     AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE, &s) == AAR_OK);
	CHECK(zzapp_svc_display_set_palette(1U, 252U, 4U, pal) == AAR_OK);
	client_service(&c);
	CHECK(cl.palette[252] == 0x00112233U);
	memset(cl.palette, 0, sizeof(cl.palette));
	before = cl.nreq[AAR_OP_DISPLAY_PALETTE];
	cl.resize_front = 0x00600000U;
	cl.resize_back = 0x00700000U;
	cl.resize_pitch = 640U;
	CHECK(zzapp_svc_display_resize(1U, 640U, 400U, &s2) == AAR_OK);
	client_service(&c);
	CHECK(cl.nreq[AAR_OP_DISPLAY_PALETTE] == before + 1U);
	CHECK(cl.last[AAR_OP_DISPLAY_PALETTE].arg[AAR_A_DISPLAY_PALETTE_FIRST] == 252U);
	CHECK(cl.last[AAR_OP_DISPLAY_PALETTE].arg[AAR_A_DISPLAY_PALETTE_COUNT] == 4U);
	CHECK(cl.palette[252] == 0x00112233U && cl.palette[255] == 0x00aabbccU && cl.palette[0] == 0U);
	(void)st;
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(rec->hdr.pid);
	(void)mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL);
}

/* The input events a display delivers only on request are not passed on
 * without the matching granted cap. */
static void test_display_event_gating(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_surface s;
	aar_event ev, out[8];
	uint32_t n;

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);

#define POST_EVENT(t, cd) do { \
	memset(&ev, 0, sizeof(ev)); \
	ev.type = (t); \
	ev.code = (cd); \
	CHECK(aar_queue_post_event(&c.cq, &ev, NULL, NULL) == AAR_OK); \
} while (0)

	/* No display: a resize event means nothing. */
	POST_EVENT(AAR_EV_RESIZE, 0U);
	POST_EVENT(AAR_EV_WHEEL, 0U);
	CHECK(zzapp_svc_event_poll(out, 8U, &n) == AAR_OK);
	CHECK(n == 1U && out[0].type == AAR_EV_WHEEL);
	/* A resizable window without text input or close request. */
	cl.open_caps = AAR_DISPCAP_WINDOW | AAR_DISPCAP_RESIZE;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565,
				     AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE, &s) == AAR_OK);
	POST_EVENT(AAR_EV_RESIZE, 0U);
	POST_EVENT(AAR_EV_TEXT, 'a');
	POST_EVENT(AAR_EV_CLOSE_REQUEST, 0U);
	POST_EVENT(AAR_EV_ACTIVE, 1U);
	POST_EVENT(AAR_EV_MOUSE, 0U);
	CHECK(zzapp_svc_event_poll(out, 8U, &n) == AAR_OK);
	CHECK(n == 3U && out[0].type == AAR_EV_RESIZE && out[1].type == AAR_EV_ACTIVE &&
	      out[2].type == AAR_EV_MOUSE);
	CHECK(zzapp_svc_display_close(1U) == AAR_OK);
	/* Everything granted. */
	cl.open_caps = FEATURE_CAPS;
	CHECK(zzapp_svc_display_open(640U, 480U, AAR_FMT_RGB565,
				     AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE |
				     AAR_DISPLAY_TEXT_INPUT | AAR_DISPLAY_CLOSE_REQUEST, &s) == AAR_OK);
	POST_EVENT(AAR_EV_RESIZE, 0U);
	POST_EVENT(AAR_EV_TEXT, 'a');
	POST_EVENT(AAR_EV_CLOSE_REQUEST, 0U);
	CHECK(zzapp_svc_event_poll(out, 8U, &n) == AAR_OK);
	CHECK(n == 3U && out[1].type == AAR_EV_TEXT && out[2].type == AAR_EV_CLOSE_REQUEST);
#undef POST_EVENT
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(rec->hdr.pid);
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
	/* The runtime counts the underruns of the card route in the ring header
	 * (spec 8.4), little-endian next to read_frame; the app and the client
	 * read the count from there. */
	CHECK(hdr[AAR_AUDIO_RING_UNDERRUNS / 4U] == 0U);
	zzapp_services_test_audio_underrun();
	zzapp_services_test_audio_underrun();
	zzapp_services_test_audio_underrun();
	CHECK(hdr[AAR_AUDIO_RING_UNDERRUNS / 4U] == 3U);
	CHECK(hdr[AAR_AUDIO_RING_READ_FRAME / 4U] == 0U &&
	      hdr[AAR_AUDIO_RING_WRITE_FRAME / 4U] == 0U);
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

/* A lease lost with the AHI stream open: the client maps the ring and writes
 * READ_FRAME through it, so the quarantined shell keeps the ring until the
 * shell is released. A stale KICK, which the client posts without reading
 * its status, releases nothing; a request the client is answered does. */
static void test_audio_ring_kept_by_quarantine(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle, pid;
	aar_audio a;

	reset_all();
	rec = claim(&c, &handle);
	pid = rec->hdr.pid;
	give_space(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_AUDIO_AHI;
	rec->claim.state = ZZAPP_ST_RUNNING;
	CHECK(zzapp_svc_audio_open(22050U, 2U, &a) == AAR_OK);
	CHECK(runtime_of(rec)->audio_ahi_route == 1);
	zzapp_services_set_terminal(ZZAPP_END_LEASE);
	zzapp_services_on_app_end();
	CHECK(runtime_of(rec)->audio_request == AUDIO_REQUEST_CLOSE);
	CHECK(zzapp_rec_retire(pid) == 0);
	zzapp_services_poll();	/* core 1 quiesces */
	zzapp_services_core0_poll();
	CHECK(runtime_of(rec)->audio_request == AUDIO_REQUEST_NONE);
	CHECK(runtime_of(rec)->audio_open == 0);
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_RECLAIMED);
	CHECK(zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_QUARANTINED));
	/* The app is gone and its stream is closed, but the ring stays. */
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 1);
	/* A stale doorbell tells the client nothing. */
	CHECK(mbox(ZZAPP_OP_KICK, handle, ZZAPP_KICK_CQ_POSTED, 0U, NULL, NULL) ==
	      SDK_STATUS_BAD_HANDLE);
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_RECLAIMED);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 1);
	/* A stale request it is answered releases the shell and the ring. */
	CHECK(mbox(ZZAPP_OP_STOP, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_BAD_HANDLE);
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_FREE);
	CHECK(heap.live == 0U);
}

/* An AHI app that ends without closing the stream (exit, fault or a forced
 * stop) never told its client to stop reading, so the ring stays until the
 * claim is released. The card route's ring, which no client maps, goes with
 * the app. */
static void test_audio_ahi_ring_kept_when_app_ends(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_audio a;

	reset_all();
	rec = claim(&c, &handle);
	give_space(rec);
	rec->claim.client_caps = ZZAPP_CLIENT_AUDIO_AHI;
	CHECK(zzapp_svc_audio_open(22050U, 2U, &a) == AAR_OK);
	CHECK(cl.nreq[AAR_OP_AUDIO_CLOSE] == 0U);
	zzapp_services_set_terminal(ZZAPP_END_EXIT);
	zzapp_services_on_app_end();
	CHECK(zzapp_rec_retire(rec->hdr.pid) == 0);
	zzapp_services_poll();	/* core 1 quiesces */
	zzapp_services_core0_poll();
	CHECK(runtime_of(rec)->audio_request == AUDIO_REQUEST_NONE);
	CHECK(runtime_of(rec)->audio_open == 0);
	CHECK(word(&c, AAR_CB_STATE) == AAR_STATE_IDLE);
	/* No close went to the client, so the ring stays. */
	CHECK(cl.nreq[AAR_OP_AUDIO_CLOSE] == 0U);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 1);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_FREE);
	CHECK(heap.live == 0U);
}

/* An AHI close returns the ring only once the client has answered it. A close
 * that could not be posted, whose wait ended without an answer, that the
 * client answered with an error, or that met a corrupt CQ leaves the stream
 * open and the ring with the client; the app can retry, and its end or the
 * claim's release returns the ring. */
static void ahi_open(client_t *c, zzapp_rec_t **rec, uint32_t *handle)
{
	aar_audio a;

	reset_all();
	*rec = claim(c, handle);
	give_space(*rec);
	(*rec)->claim.client_caps = ZZAPP_CLIENT_AUDIO_AHI;
	CHECK(zzapp_svc_audio_open(22050U, 2U, &a) == AAR_OK);
	CHECK(cl.nreq[AAR_OP_AUDIO_CLOSE] == 0U);
}

static void ahi_ring_kept(zzapp_rec_t *rec)
{
	CHECK(runtime_of(rec)->audio_open == 1);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 1);
}

/* The app ends with the stream still open: the ring waits for the release. */
static void ahi_end_and_release(client_t *c, zzapp_rec_t *rec, uint32_t handle)
{
	zzapp_services_set_terminal(ZZAPP_END_EXIT);
	zzapp_services_on_app_end();
	CHECK(zzapp_rec_retire(rec->hdr.pid) == 0);
	zzapp_services_poll();
	zzapp_services_core0_poll();
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 1);
	CHECK(word(c, AAR_CB_STATE) == AAR_STATE_IDLE);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_FREE);
	CHECK(heap.live == 0U);
}

static void test_audio_ahi_close_failures(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle;
	aar_completion d;

	/* Not posted (the ids are used up): nothing is in flight, nothing held. */
	ahi_open(&c, &rec, &handle);
	rec->xport.next_id = 0U;
	CHECK(zzapp_svc_audio_close() == AAR_E_NOSPC);
	ahi_ring_kept(rec);
	CHECK(!zzapp_xport_buffer_held(rec, 0xffU));
	CHECK(cl.nreq[AAR_OP_AUDIO_CLOSE] == 0U);
	rec->xport.next_id = 0x1000U;
	CHECK(zzapp_svc_audio_close() == AAR_OK);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 0);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(heap.live == 0U);

	/* Posted, then the wait ends without an answer: the close names the ring
	 * until the client answers it, even late and with no other call. */
	ahi_open(&c, &rec, &handle);
	cl.ignore = 1;
	cl.stop_at_yield = cl.yields + 2U;
	CHECK(zzapp_svc_audio_close() == AAR_E_IO);
	ahi_ring_kept(rec);
	CHECK(zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_AUDIO));
	rec->host.stop_requested = 0U;
	cl.stop_at_yield = 0U;
	cl.ignore = 0;
	CHECK(zzapp_svc_audio_close() == AAR_E_BUSY);	/* nobody answered yet */
	ahi_ring_kept(rec);
	memset(&d, 0, sizeof(d));
	d.op = AAR_OP_AUDIO_CLOSE;
	d.id = rec->xport.req[ZZAPP_XPORT_REQ_BLOCKING].id;
	client_post(&c, &d);
	CHECK(zzapp_svc_audio_close() == AAR_OK);	/* the late answer is taken first */
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 0);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(heap.live == 0U);

	/* The client answers with an error: the stream stays open. */
	ahi_open(&c, &rec, &handle);
	cl.result = AAR_E_IO;
	CHECK(zzapp_svc_audio_close() == AAR_E_IO);
	ahi_ring_kept(rec);
	cl.result = AAR_OK;
	CHECK(zzapp_svc_audio_close() == AAR_OK);
	CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 0);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(heap.live == 0U);

	/* A corrupt CQ ends the wait: the ring stays, and the app's end and the
	 * claim's release return it. */
	ahi_open(&c, &rec, &handle);
	cl.ignore = 1;
	aar_put_be32(c.cq.tail, 0x1000U);
	CHECK(zzapp_svc_audio_close() == AAR_E_IO);
	ahi_ring_kept(rec);
	CHECK(zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_AUDIO));
	aar_put_be32(c.cq.tail, 0U);
	zzapp_xport_flag_clear(rec, ZZAPP_XPORT_F_CORRUPT);
	cl.ignore = 0;
	ahi_end_and_release(&c, rec, handle);
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
	/* A deferred load call is answered while the client's lease tick may
	 * still write the control block, so its stale reply leaves the shell;
	 * the client follows it with a RELEASE, which lets the shell go. */
	CHECK(mbox(ZZAPP_OP_LOAD_BEGIN, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_BAD_HANDLE);
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_RECLAIMED && heap.live == 1U);
	CHECK(revoke_visible_calls == 0U);
	CHECK(mbox(ZZAPP_OP_RELEASE, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_BAD_HANDLE);
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

	/* A running app whose lease ran out (the host ends it with the LEASE end
	 * reason): the shell is kept, quarantined, under a revoked handle; the end
	 * reason is what the client reads; the old handle's next request releases
	 * it and returns the app's own host-visible blocks. */
	reset_all();
	rec = claim(&c, &handle);
	pid = rec->hdr.pid;
	rec->claim.state = ZZAPP_ST_RUNNING;
	zzapp_services_set_terminal(ZZAPP_END_LEASE);
	zzapp_services_on_app_end();
	CHECK(zzapp_rec_retire(pid) == 0);
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_RECLAIMED);
	CHECK(zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_QUARANTINED));
	CHECK(zzapp_rec_by_handle(handle) == NULL && rec->hdr.handle != handle);
	CHECK(word(&c, AAR_CB_END_REASON) == ZZAPP_END_LEASE);
	CHECK(word(&c, AAR_CB_STATE) == AAR_STATE_IDLE);
	CHECK(heap.live == 1U && revoke_visible_calls == 0U);
	/* A stale KICK is posted without its status being read: it tells the
	 * client nothing and leaves the shell. */
	CHECK(mbox(ZZAPP_OP_KICK, handle, ZZAPP_KICK_CQ_POSTED, 0U, NULL, NULL) ==
	      SDK_STATUS_BAD_HANDLE);
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_RECLAIMED && heap.live == 1U);
	CHECK(revoke_visible_calls == 0U);
	CHECK(mbox(ZZAPP_OP_STOP, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_BAD_HANDLE);
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_FREE && heap.live == 0U);
	CHECK(revoke_visible_calls == 1U);

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

/* ---- Event counts --------------------------------------------------------------------- */

static uint32_t evw(unsigned word)
{
	return aar_get_be32(zzapp_xport_events_block() + 4U * word);
}

static void query_events(int z3, uint32_t *events, uint16_t *len)
{
	uint8_t reply[48];

	memset(reply, 0xEE, sizeof(reply));
	CHECK(zzapp_mailbox_dispatch(ZZAPP_OP_QUERY, NULL, 0U, reply, len, z3, 0U,
				     0U) == (z3 ? SDK_STATUS_OK : SDK_STATUS_UNSUPPORTED));
	*events = zzapp_get_be32(reply + ZZAPP_QUERY_EVENTS);
}

/* QUERY names the event block, and every end reason the services write moves
 * the counts of the record's group and the global count. */
static void test_events(void)
{
	client_t c;
	zzapp_rec_t *rec;
	uint32_t handle, before, token;
	uint16_t len;

	/* No block: the word is 0 and the reply is 48 bytes. */
	reset_all();
	CHECK(zzapp_xport_events_block() == NULL && zzapp_xport_events_token() == 0U);
	query_events(1, &token, &len);
	CHECK(token == 0U && len == ZZAPP_MAILBOX_PAYLOAD_BYTES);

	/* A block: zeroed, named by its token (a board offset), 64 bytes of
	 * words in an 8 KiB block of the heap. */
	CHECK(zzapp_xport_events_init() == 0);
	CHECK(heap.live == 1U && zzapp_xport_events_block() != NULL);
	query_events(1, &token, &len);
	CHECK(len == ZZAPP_MAILBOX_PAYLOAD_BYTES);
	CHECK(token != 0U && token == zzapp_xport_events_token());
	CHECK(map_token(token) == (void *)zzapp_xport_events_block());
	CHECK(heap.block[0].bytes == ZZAPP_EVENT_BLOCK_BYTES);
	CHECK(((uintptr_t)heap.block[0].ptr & (ZZAPP_XPORT_BLOCK_STEP - 1U)) == 0U);
	for (before = 0U; before < ZZAPP_EVENT_BYTES / 4U; before++)
		CHECK(evw(before) == 0U);
	query_events(0, &token, &len);                /* Z2: nothing */
	CHECK(token == 0U);

	/* The claim itself posts nothing; an end reason does. */
	rec = claim(&c, &handle);
	CHECK(rec && evw(0) == 0U && evw(1) == 0U);
	zzapp_services_set_terminal(ZZAPP_END_EXIT);
	CHECK(evw(0) == 1U && evw(1) == 1U && evw(2) == 0U);
	zzapp_services_set_terminal(0U);              /* a zero reason shows nothing */
	CHECK(evw(0) == 1U && evw(1) == 1U);

	/* The lease runs out on a claim that never loaded: the end reason comes
	 * with the retire. */
	reset_all();
	CHECK(zzapp_xport_events_init() == 0);
	rec = claim(&c, &handle);
	CHECK(rec && evw(0) == 0U);
	zzapp_services_expire_claims(fake_now + 31000000U);
	CHECK(word(&c, AAR_CB_END_REASON) == ZZAPP_END_LEASE);
	CHECK(evw(0) >= 1U && evw(1) == evw(0));

	/* The lease runs out during a load: the end reason is written at once. */
	reset_all();
	CHECK(zzapp_xport_events_init() == 0);
	service_test_loader_begin_hook = begin_hook;
	rec = claim(&c, &handle);
	CHECK(deferred(ZZAPP_OP_LOAD_BEGIN, handle, 0U, 0U, NULL, NULL) == SDK_STATUS_OK);
	CHECK(rec->claim.state == ZZAPP_ST_LOADING);
	before = evw(0);
	zzapp_services_expire_claims(runtime_of(rec)->load_progress + 31000000U);
	CHECK(word(&c, AAR_CB_END_REASON) == ZZAPP_END_QUARANTINED);
	CHECK(evw(0) > before && evw(1) == evw(0));

	/* The block of a test is not the next test's. */
	reset_all();
	CHECK(zzapp_xport_events_block() == NULL);
}

int main(void)
{
	test_claim_release();
	test_events();
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
	test_event_ring_full_mouse();
	test_event_ring_full_absolute_mouse();
	test_pass_wakes();
	test_display_pan();
	test_display_vblank();
	test_display_flip_front_and_timeout();
	test_display_latest();
	test_display_bad_tokens();
	test_files_v06();
	test_time_locale_env();
	test_clipboard();
	test_display_caps_v06();
	test_display_window_flip();
	test_display_present_time();
	test_display_info_modes();
	test_display_title_pointer();
	test_display_wait_vblank();
	test_display_native_vblank();
	test_display_resize();
	test_display_event_gating();
	test_audio_card();
	test_audio_ahi();
	test_audio_ring_kept_by_quarantine();
	test_audio_ahi_ring_kept_when_app_ends();
	test_audio_ahi_close_failures();
	test_lease_and_quarantine();
	test_reset();
	test_churn();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
