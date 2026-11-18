/*
 * Slice B host tests: seqlock, chunked read, drop-on-full, audio busy,
 * mailbox payloads fit in 48 bytes.
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

static uint8_t g_window[ZZAPP_WINDOW_BYTES];
static uint32_t g_produced;
static uint32_t g_last_seq;
static int g_bump;
static int g_fails;
static uint16_t expiry_claim_status;
static uint16_t stop_completing_status;
static uint16_t stop_completing_claim_status;
static uint32_t stop_completing_slot_state;
static uint32_t expiry_deferred_generation;
static uint32_t expiry_slot_generation;
static uint64_t loading_now;

static uint64_t loading_test_time(void)
{
	return loading_now;
}

static int loader_chunk_succeeds(uint32_t file_off, const void *src,
				 uint32_t len)
{
	(void)file_off;
	(void)src;
	(void)len;
	return AAR_OK;
}

static int start_posted_deferred(void)
{
	if (DEF(0).state != DEFER_POSTED)
		return 0;
	DEF(0).state = DEFER_RUNNING;
	return 1;
}
static int harvest_reply_hook_result;
static uint16_t harvest_reply_hook_status;

static void harvest_between_reply_and_done(void)
{
	harvest_reply_hook_result = zzapp_mailbox_harvest(
		NULL, NULL, NULL, &harvest_reply_hook_status, NULL, NULL);
}

static void fail(const char *what)
{
	fprintf(stderr, "fail: %s\n", what);
	g_fails++;
}

/* Every file OPEN the emulated launcher saw: how many, and the flags word
 * as it sat at FILE_OFF+12 when the request was published. */
static uint32_t g_open_count;
static uint32_t g_open_flags;

static void launcher(void)
{
	uint32_t seq = zzapp_get_be32(g_window + ZZAPP_FILE_OFF);
	uint32_t op;
	uint32_t len;
	uint32_t i;

	if ((seq & 1U) != 0U || seq == 0U || seq == g_last_seq)
		return;
	if (g_bump) {
		zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_GENERATION_OFF, 9U);
		return;
	}
	op = zzapp_get_be32(g_window + ZZAPP_FILE_OFF + 4U);
	len = zzapp_get_be32(g_window + ZZAPP_FILE_OFF + 28U);
	if (op == ZZAPP_FILE_OP_OPEN) {
		g_open_count++;
		g_open_flags = zzapp_get_be32(g_window + ZZAPP_FILE_OFF + 12U);
	}
	zzapp_put_be32(g_window + ZZAPP_FILE_OFF, seq + 1U);
	if (op == ZZAPP_FILE_OP_READ) {
		if (len > ZZAPP_DATA_BYTES)
			len = ZZAPP_DATA_BYTES;
		for (i = 0; i < len; i++)
			g_window[ZZAPP_DATA_OFF + i] = (uint8_t)(g_produced + i);
		g_produced += len;
		zzapp_put_be32(g_window + ZZAPP_FILE_OFF + 32U, len);
	}
	zzapp_put_be32(g_window + ZZAPP_FILE_OFF + 20U, 0U);
	zzapp_put_be32(g_window + ZZAPP_FILE_OFF, seq + 2U);
	g_last_seq = seq + 2U;
}

static void test_payload_sizes(void)
{
	if (ZZAPP_QUERY_REPLY_BYTES > 48U || ZZAPP_ALLOC_REPLY_BYTES > 48U ||
	    ZZAPP_CHUNK_REQ_BYTES > 48U || ZZAPP_START_REQ_BYTES > 48U ||
	    ZZAPP_BIND_REQ_BYTES > 48U || ZZAPP_STATUS_REPLY_BYTES > 48U)
		fail("mailbox payload exceeds 48 bytes");
	if (ZZAPP_STATUS_REPLY_BYTES != 48U)
		fail("status snapshot is not 48 bytes");
}

static void test_z2_query(void)
{
	uint8_t reply[48];
	uint16_t n = 0U;
	uint16_t status;

	memset(reply, 0x5a, sizeof(reply));
	status = zzapp_mailbox_dispatch(ZZAPP_OP_QUERY, NULL, 0U, reply, &n,
					0, 1U, 0U);
	if (status != SDK_STATUS_UNSUPPORTED)
		fail("Z2 query status");
	if (n != ZZAPP_QUERY_REPLY_BYTES)
		fail("Z2 query length");
	if (zzapp_get_be32(reply + ZZAPP_QUERY_CAPS) != 0U)
		fail("Z2 query caps");
	status = zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, NULL, 0U, reply, &n,
					0, 2U, 0U);
	if (status != SDK_STATUS_UNSUPPORTED)
		fail("Z2 load status");
}

static void test_chunked_read(void)
{
	uint8_t buf[READ_BYTES];
	uint32_t got = 0U;
	uint32_t i;
	int fd;
	int rc;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	g_produced = 0U;
	g_last_seq = 0U;
	g_bump = 0;
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_services_test_set_yield(launcher);
	zzapp_services_set_generation(1U);
	fd = zzapp_svc_file_open("id1/pak0.pak", AAR_FILE_READ);
	if (fd < 3)
		fail("open");
	rc = zzapp_svc_file_read(fd, buf, READ_BYTES, &got);
	if (rc != AAR_OK)
		fail("chunked read status");
	if (got != READ_BYTES)
		fail("chunked read length");
	for (i = 0; i < got; i++) {
		if (buf[i] != (uint8_t)i) {
			fail("chunked read order");
			break;
		}
	}
	if (g_produced != READ_BYTES)
		fail("producer cursor");
	g_bump = 1;
	rc = zzapp_svc_file_read(fd, buf, 64U, &got);
	if (rc != AAR_E_IO)
		fail("generation bump");
}

static void push_event(uint16_t code)
{
	uint32_t head = zzapp_get_be32(g_window + ZZAPP_INPUT_OFF);
	uint32_t tail = zzapp_get_be32(g_window + ZZAPP_INPUT_OFF + 4U);
	uint32_t off;

	if (head - tail >= ZZAPP_INPUT_CAP) {
		uint32_t drops = zzapp_get_be32(g_window + ZZAPP_LIFE_OFF +
						ZZAPP_LIFE_INPUT_DROPS_OFF);

		zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_INPUT_DROPS_OFF,
			       drops + 1U);
		return;
	}
	off = ZZAPP_INPUT_OFF + ZZAPP_INPUT_HDR +
	      (head & (ZZAPP_INPUT_CAP - 1U)) * ZZAPP_EVENT_BYTES;
	zzapp_put_be16(g_window + off, (uint16_t)AAR_EV_KEY_DOWN);
	zzapp_put_be16(g_window + off + 2U, code);
	zzapp_put_be32(g_window + ZZAPP_INPUT_OFF, head + 1U);
}

static void test_drop_on_full(void)
{
	aar_event ev[8];
	uint32_t n = 0U;
	uint32_t i;
	uint32_t got = 0U;
	int rc;

	for (i = 0; i < ZZAPP_INPUT_CAP + 1U; i++)
		push_event((uint16_t)i);
	if (zzapp_get_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_INPUT_DROPS_OFF) != 1U)
		fail("input drop count");
	while (got < ZZAPP_INPUT_CAP) {
		rc = zzapp_svc_event_poll(ev, 8U, &n);
		if (rc != AAR_OK || n == 0U) {
			fail("input poll");
			return;
		}
		for (i = 0; i < n; i++) {
			if (ev[i].code != (uint16_t)(got + i) ||
			    ev[i].type != AAR_EV_KEY_DOWN) {
				fail("input order");
				return;
			}
		}
		got += n;
	}
	rc = zzapp_svc_event_poll(ev, 8U, &n);
	if (rc != AAR_OK || n != 0U)
		fail("input drained");
	if (got != ZZAPP_INPUT_CAP)
		fail("input count");
}

static void test_audio_busy(void)
{
	aar_audio audio;
	int rc;

	zzapp_services_test_set_slot_busy(2U, 1);
	zzapp_services_test_set_slot_busy(1U, 1);
	rc = zzapp_svc_audio_open(22050U, 2U, &audio);
	if (rc != AAR_E_BUSY)
		fail("audio busy");
	rc = zzapp_svc_audio_open(11025U, 2U, &audio);
	if (rc != AAR_E_INVAL)
		fail("audio 11025");
	zzapp_services_test_set_slot_busy(2U, 0);
	zzapp_services_test_set_slot_busy(1U, 0);
	rc = zzapp_svc_audio_open(22050U, 2U, &audio);
	if (rc != AAR_OK || audio.rate != 22050U || audio.channels != 2U ||
	    audio.frames != 8192U)
		fail("audio grant");
	if (zzapp_svc_audio_close() != AAR_OK)
		fail("audio close");
}

static void test_display_refresh_rotation_and_layout(void)
{
	uint8_t payload[ZZAPP_BIND_REQ_BYTES];
	uint8_t reply[48];
	uint16_t reply_len = 0U;
	aar_surface surface;
	uint32_t front = 0x00200000U;
	uint32_t back = 0x00210000U;
	int rc;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	/* aar_display_open raises the request; the bind must clear it or the
	 * launcher and the app both treat the display as unbound. */
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLAGS_OFF,
		       ZZAPP_F_DISPLAY_UNBOUND);
	zzapp_build_display_bind_request_layout(payload, 320U, 200U, 640U,
						 AAR_FMT_RGB565, front, back,
						 AAR_LAYOUT_RGB565_LE);
	rc = zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, payload, sizeof(payload),
				    reply, &reply_len, 1, 1U, 0U);
	if (rc != SDK_STATUS_OK ||
	    (zzapp_services_flags() & ZZAPP_F_DISPLAY_UNBOUND) != 0U)
		fail("display bind");
	memset(&surface, 0, sizeof(surface));
	surface.id = 1U;
	if (zzapp_svc_display_refresh(&surface) != AAR_OK ||
	    PHYS(surface.front) != front ||
	    PHYS(surface.back) != back ||
	    surface.layout != AAR_LAYOUT_RGB565_LE)
		fail("display initial surface");
	/* The generation counter is core-1-private state: raise it through
	 * the kernel so its cache and the life page stay in step.  An old
	 * launcher does not negotiate scanout acknowledgement, so its
	 * established lifecycle ack remains authoritative. */
	if (zzapp_display_flip_bound() != AAR_OK)
		fail("display gen bump");
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLIP_ADDRESS_OFF,
		       back);
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLIP_SUBMIT_OFF,
		       1U);
	zzapp_services_display_vblank(back);
	if (zzapp_get_be32(g_window + ZZAPP_LIFE_OFF +
			   ZZAPP_LIFE_FLIP_ACK_OFF) != 0U)
		fail("display old launcher compatibility");
	if (zzapp_svc_display_refresh(&surface) != AAR_E_BUSY)
		fail("display pending refresh");
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLIP_ACK_OFF, 1U);
	if (zzapp_svc_display_refresh(&surface) != AAR_OK ||
	    PHYS(surface.front) != back ||
	    PHYS(surface.back) != front ||
	    surface.layout != AAR_LAYOUT_RGB565_LE)
		fail("display refreshed rotation");
}

static void test_display_scanout_ack(void)
{
	uint8_t payload[ZZAPP_BIND_EXT_REQ_BYTES];
	uint8_t reply[48];
	uint16_t reply_len = 0U;
	aar_surface surface;
	uint32_t front = 0x00200000U;
	uint32_t back = 0x00210000U;
	uint32_t spare = 0x00220000U;
	int rc;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLAGS_OFF,
		       ZZAPP_F_LAUNCHER_SCANOUT_ACK);
	zzapp_build_display_bind_request_triple(payload, 320U, 200U, 640U,
						AAR_FMT_RGB565, front, back,
						AAR_LAYOUT_RGB565_LE, spare);
	rc = zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, payload, sizeof(payload),
				    reply, &reply_len, 1, 1U, 0U);
	if (rc != SDK_STATUS_OK ||
	    (zzapp_services_flags() & ZZAPP_F_SCANOUT_ACK) == 0U)
		fail("display scanout-ack bind");
	memset(&surface, 0, sizeof(surface));
	surface.id = 1U;
	if (zzapp_svc_display_flip_async(surface.id) != AAR_OK ||
	    zzapp_get_be32(g_window + ZZAPP_LIFE_OFF +
			   ZZAPP_LIFE_FLIP_ADDRESS_OFF) != back)
		fail("display scanout-ack queue");
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLIP_SUBMIT_OFF, 1U);
	zzapp_services_display_vblank(front);
	if (zzapp_get_be32(g_window + ZZAPP_LIFE_OFF +
			   ZZAPP_LIFE_FLIP_ACK_OFF) != 0U)
		fail("display scanout-ack wrong address");
	zzapp_services_display_vblank(back);
	if (zzapp_get_be32(g_window + ZZAPP_LIFE_OFF +
			   ZZAPP_LIFE_FLIP_ACK_OFF) != 1U)
		fail("display scanout-ack vblank");
	if (zzapp_svc_display_refresh(&surface) != AAR_OK ||
	    PHYS(surface.front) != back ||
	    PHYS(surface.back) != spare)
		fail("display scanout-ack recycle");
}

static uint64_t g_flip_us;
static unsigned g_flip_yields;
static unsigned g_flip_unregistered_yields;

static uint64_t flip_time(void)
{
	return g_flip_us;
}

/* Model a parked app: it resumes only after the service pass wakes its
 * registered wait, first at the 50 ms diagnostic deadline and then at ack. */
static void flip_yield(void)
{
	if (!zzapp_services_wait_registered(service_pid()))
		g_flip_unregistered_yields++;
	g_flip_yields++;
	if (current_runtime()->wait_deadline != 0U) {
		g_flip_us = current_runtime()->wait_deadline;
	} else {
		g_flip_us = 80000U;
		zzapp_put_be32(g_window + ZZAPP_LIFE_OFF +
			       ZZAPP_LIFE_FLIP_ACK_OFF, 1U);
	}
	zzapp_services_poll();
}

static void test_display_wait_flip_idle(void)
{
	uint8_t payload[ZZAPP_BIND_EXT_REQ_BYTES];
	uint8_t reply[48];
	uint16_t reply_len = 0U;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLAGS_OFF,
		       ZZAPP_F_LAUNCHER_SCANOUT_ACK);
	zzapp_build_display_bind_request_triple(payload, 320U, 200U, 640U,
						AAR_FMT_RGB565, 0x00200000U,
						0x00210000U, AAR_LAYOUT_RGB565_LE,
						0x00220000U);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, payload, sizeof(payload),
				   reply, &reply_len, 1, 1U, 0U) != SDK_STATUS_OK)
		fail("flip wait display bind");
	g_flip_us = 0U;
	g_flip_yields = 0U;
	g_flip_unregistered_yields = 0U;
	flip_wait_reports = 0U;
	zzapp_services_test_set_time(flip_time);
	zzapp_services_test_set_yield(flip_yield);
	/* Nothing queued: idle at once, no wait. */
	if (zzapp_display_wait_flip_idle() != AAR_OK || g_flip_yields != 0U)
		fail("flip wait while idle");
	if (zzapp_svc_display_flip_async(1U) != AAR_OK)
		fail("flip wait queue");
	/* The first pass wakes at the 50 ms diagnostic deadline. The second
	 * remains parked until VBlank acknowledges the queued flip at 80 ms. */
	if (zzapp_display_wait_flip_idle() != AAR_OK)
		fail("flip wait result");
	if (g_flip_yields != 2U || g_flip_unregistered_yields != 0U)
		fail("flip wait registers with the service pass");
	if (zzapp_services_wait_registered(service_pid()))
		fail("flip wait ended");
	if (flip_wait_reports != 1U)
		fail("slow flip reported once");
	zzapp_services_test_set_time(NULL);
	zzapp_services_test_set_yield(NULL);
}

static void test_clut8_palette_publication(void)
{
	uint8_t payload[ZZAPP_BIND_REQ_BYTES];
	uint8_t reply[48];
	uint16_t reply_len = 0U;
	uint32_t rgb[] = { 0x11223344U, 0x00a0b0c0U };

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLAGS_OFF,
		       ZZAPP_F_LAUNCHER_CLUT8);
	zzapp_build_display_bind_request_layout(payload, 320U, 200U, 320U,
						AAR_FMT_CLUT8, 0x00200000U,
						0x00210000U, AAR_LAYOUT_CLUT8);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, payload, sizeof(payload),
				   reply, &reply_len, 1, 1U, 0U) != SDK_STATUS_OK)
		fail("CLUT8 display bind");
	if (zzapp_svc_display_set_palette(1U, 4U, 2U, rgb) != AAR_OK ||
	    zzapp_get_be32(g_window + ZZAPP_PALETTE_OFF + 4U * 4U) != 0x223344U ||
	    zzapp_get_be32(g_window + ZZAPP_PALETTE_OFF + 5U * 4U) != 0xa0b0c0U ||
	    zzapp_get_be32(g_window + ZZAPP_LIFE_OFF +
			   ZZAPP_LIFE_PALETTE_FIRST_OFF) != 4U ||
	    zzapp_get_be32(g_window + ZZAPP_LIFE_OFF +
			   ZZAPP_LIFE_PALETTE_COUNT_OFF) != 2U ||
	    zzapp_get_be32(g_window + ZZAPP_LIFE_OFF +
			   ZZAPP_LIFE_PALETTE_GEN_OFF) != 1U)
		fail("CLUT8 palette publish");
}

static void test_display_mailbox(void)
{
	uint8_t payload[ZZAPP_BIND_EXT_REQ_BYTES];
	uint8_t reply[48];
	uint16_t reply_len = 0U;
	aar_surface surface;
	uint32_t scanout;
	uint32_t front = 0x00200000U;
	uint32_t back = 0x00240000U;
	uint32_t spare = 0x00280000U;
	int rc;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLAGS_OFF,
		       ZZAPP_F_LAUNCHER_MAILBOX | ZZAPP_F_MAILBOX_REQUEST |
		       ZZAPP_F_DISPLAY_UNBOUND);
	zzapp_build_display_bind_request_triple(payload, 320U, 200U, 640U,
						AAR_FMT_RGB565, front, back,
						AAR_LAYOUT_RGB565_LE, spare);
	rc = zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, payload, sizeof(payload),
				    reply, &reply_len, 1, 1U, 0U);
	if (rc != SDK_STATUS_OK ||
	    (zzapp_services_flags() & ZZAPP_F_MAILBOX) == 0U ||
	    (zzapp_services_flags() & ZZAPP_F_DISPLAY_UNBOUND) != 0U)
		fail("display mailbox bind");
	if (zzapp_services_display_mailbox_begin(0x00230000U, &scanout))
		fail("display mailbox rejects buffer gap");
	memset(&surface, 0, sizeof(surface));
	surface.id = 1U;
	if (zzapp_svc_display_flip_async(surface.id) != AAR_OK ||
	    zzapp_svc_display_refresh(&surface) != AAR_OK ||
	    PHYS(surface.back) != spare)
		fail("display mailbox first queue");
	if (zzapp_svc_display_flip_async(surface.id) != AAR_OK ||
	    zzapp_svc_display_refresh(&surface) != AAR_OK ||
	    PHYS(surface.back) != back ||
	    zzapp_svc_display_latest_dropped() != 1U)
		fail("display mailbox replace");
	if (!zzapp_services_display_mailbox_begin(front, &scanout) ||
	    scanout != spare)
		fail("display mailbox select latest");
	zzapp_services_display_mailbox_end(1);
	if (zzapp_svc_display_refresh(&surface) != AAR_OK ||
	    PHYS(surface.front) != spare ||
	    PHYS(surface.back) != back ||
	    zzapp_get_be32(g_window + ZZAPP_LIFE_OFF +
			   ZZAPP_LIFE_CARD_SPARE_OFF) != front)
		fail("display mailbox latch reuse");
	if (zzapp_svc_display_flip_async(surface.id) != AAR_OK)
		fail("display mailbox second queue");
	if (zzapp_services_display_mailbox_begin(0x00300000U, &scanout))
		fail("display mailbox hidden screen");
	if (zzapp_svc_display_refresh(&surface) != AAR_OK ||
	    PHYS(surface.back) != front)
		fail("display mailbox hidden ownership");
	display_mailbox_lock_acquire();
	zzapp_services_reset_core1_lock();
	if (!zzapp_services_display_mailbox_begin(front, &scanout))
		fail("display mailbox restart unlock");
	else
		zzapp_services_display_mailbox_end(0);

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLAGS_OFF,
		       ZZAPP_F_MAILBOX_REQUEST);
	rc = zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, payload, sizeof(payload),
				    reply, &reply_len, 1, 1U, 0U);
	if (rc != SDK_STATUS_OK ||
	    (zzapp_services_flags() & ZZAPP_F_MAILBOX) != 0U)
		fail("display mailbox old launcher fallback");
}
static void test_display_mailbox_uses_scanout_owner(void)
{
	uint8_t win_mem[2][ZZAPP_WINDOW_BYTES];
	uint32_t scanout;

	zzapp_services_test_reset();
	memset(win_mem, 0, sizeof(win_mem));
	zzapp_services_test_set_window_slot(0U, win_mem[0], sizeof(win_mem[0]));
	zzapp_services_test_set_window_slot(1U, win_mem[1], sizeof(win_mem[1]));
	RT(1).display_mailbox_active = 1;
	RT(1).display_mailbox_shown = 0x00200000U;
	RT(1).display_mailbox_pending = 0x00210000U;
	RT(1).display_mailbox_buffers[0] = 0x00200000U;
	RT(1).display_mailbox_buffers[1] = 0x00210000U;
	RT(1).display_mailbox_buffers[2] = 0x00220000U;
	RT(1).display_mailbox_bytes = 0x10000U;
	zzapp_services_test_set_core(0);
	zzapp_services_select_slot(0U);
	if (!zzapp_services_display_mailbox_begin(0x00200000U, &scanout) ||
	    scanout != 0x00210000U)
		fail("display mailbox scanout owner");
	zzapp_services_display_mailbox_end(1);
	if (RT(1).display_mailbox_shown != 0x00210000U ||
	    RT(1).display_mailbox_pending != 0U ||
	    zzapp_get_be32(win_mem[1] + ZZAPP_LIFE_CARD_FRONT_OFF) !=
		0x00210000U)
		fail("display mailbox owner completion");
	zzapp_services_test_set_core(1);
}


static uint64_t file_now;
static int file_phase;
static int late_injected;
static uint32_t abandoned_done;
static uint32_t live_req;

static uint64_t file_time(void)
{
	return file_now;
}

static void file_yield(void)
{
	uint32_t seq = zzapp_get_be32(g_window + ZZAPP_FILE_OFF);

	if (file_phase == 0) {
		if ((seq & 1U) == 0U && seq != 0U)
			abandoned_done = seq + 2U;
		file_now += ZZAPP_FILE_TIMEOUT_US;
		return;
	}
	if (!late_injected) {
		if ((seq & 1U) == 0U && seq != 0U)
			live_req = seq;
		/* Stale ZZRun completion: request_seq + 2, not this request. */
		zzapp_put_be32(g_window + ZZAPP_FILE_OFF + 20U, 99U);
		zzapp_put_be32(g_window + ZZAPP_FILE_OFF, abandoned_done);
		late_injected = 1;
		return;
	}
	if (live_req != 0U) {
		zzapp_put_be32(g_window + ZZAPP_FILE_OFF, live_req + 1U);
		zzapp_put_be32(g_window + ZZAPP_FILE_OFF + 20U, 0U);
		zzapp_put_be32(g_window + ZZAPP_FILE_OFF, live_req + 2U);
		live_req = 0U;
		return;
	}
	file_now += ZZAPP_FILE_TIMEOUT_US;
}

static void test_late_file_reply(void)
{
	int fd;
	uint32_t seq;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_services_set_generation(1U);
	file_now = 0U;
	file_phase = 0;
	late_injected = 0;
	abandoned_done = 0U;
	live_req = 0U;
	zzapp_services_test_set_time(file_time);
	zzapp_services_test_set_yield(file_yield);
	fd = zzapp_svc_file_open("pak0.pak", AAR_FILE_READ);
	if (fd != AAR_E_TIMEDOUT)
		fail("first open timeout");
	seq = zzapp_get_be32(g_window + ZZAPP_FILE_OFF);
	if ((seq & 1U) == 0U)
		fail("timeout looked like a new launcher request");
	file_phase = 1;
	file_now = 0U;
	fd = zzapp_svc_file_open("pak1.pak", AAR_FILE_READ);
	if (fd != ZZAPP_FD_BASE + 1)
		fail("late reply applied or timed-out fd reused");
}

static void test_audio_teardown_handoff(void)
{
	aar_audio audio;
	int rc;

	zzapp_services_test_reset();
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	rc = zzapp_svc_audio_open(22050U, 2U, &audio);
	if (rc != AAR_OK)
		fail("audio open before end");
	zzapp_services_on_app_end();
	if (zzapp_services_test_audio_detach_count() != 0U)
		fail("app end reclaimed audio");
	if (zzapp_services_test_audio_request() != AUDIO_REQUEST_CLOSE)
		fail("app end did not hand off close");
	if (zzapp_svc_audio_open(22050U, 2U, &audio) != AAR_E_BUSY)
		fail("open reused audio before handoff");
	zzapp_services_poll();
	if (zzapp_services_test_audio_request() != AUDIO_REQUEST_CLOSE_QUIESCED)
		fail("core 1 did not quiesce audio close");
	if (zzapp_svc_audio_open(22050U, 2U, &audio) != AAR_E_BUSY)
		fail("open during audio quiesce");
	zzapp_services_core0_poll();
	if (zzapp_services_test_audio_detach_count() != 1U)
		fail("core 0 did not reclaim audio");
	if (zzapp_services_test_audio_request() != AUDIO_REQUEST_NONE)
		fail("audio handoff still pending");
	if (zzapp_svc_audio_open(22050U, 2U, &audio) != AAR_OK)
		fail("open after audio reclaim");
	if (zzapp_svc_audio_close() != AAR_OK)
		fail("close after reclaim");
}
static void test_reset_preserves_audio_teardown(void)
{
	aar_audio audio;

	zzapp_services_test_reset();
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	if (zzapp_svc_audio_open(22050U, 2U, &audio) != AAR_OK)
		fail("audio open before reset");
	zzapp_services_on_reset();
	if (zzapp_services_test_audio_request() != AUDIO_REQUEST_CLOSE)
		fail("reset discarded audio close request");
	zzapp_services_poll();
	if (zzapp_services_test_audio_request() != AUDIO_REQUEST_CLOSE_QUIESCED)
		fail("reset did not quiesce audio close");
	zzapp_services_core0_poll();
	if (zzapp_services_test_audio_detach_count() != 1U ||
	    zzapp_services_test_audio_request() != AUDIO_REQUEST_NONE)
		fail("reset did not complete audio detach");
}


static void test_audio_write_cursor_wrap(void)
{
	/* The app's write cursor is 32-bit and wraps after ~54 h at 22050 Hz.
	 * Produced must stay >= staged across the wrap, or the fabric producer
	 * stalls. */
	uint64_t staged = 0xFFFFFFFCull * 4u; /* staged up to frame 2^32 - 4 */

	if (audio_produced_frames(0xFFFFFFFEu, staged) != 0xFFFFFFFEull)
		fail("cursor before the wrap");
	if (audio_produced_frames(0x00000003u, staged) != 0x100000003ull)
		fail("cursor across the wrap");
	staged = 0x100000001ull * 4u; /* staging itself crossed the wrap */
	if (audio_produced_frames(0x00000005u, staged) != 0x100000005ull)
		fail("cursor after staging crossed the wrap");
	if (audio_produced_frames(0x00000001u, staged) != 0x100000001ull)
		fail("cursor equal to staged");
}

static uint8_t g_slot_windows[2][ZZAPP_WINDOW_BYTES];

static void slot_launcher(void)
{
	uint8_t *slot_window = service_win()->window;
	uint32_t seq;

	if (!slot_window)
		return;
	seq = zzapp_get_be32(slot_window + ZZAPP_FILE_OFF);
	if ((seq & 1U) != 0U || seq == 0U)
		return;
	zzapp_put_be32(slot_window + ZZAPP_FILE_OFF, seq + 1U);
	zzapp_put_be32(slot_window + ZZAPP_FILE_OFF + 20U, 0U);
	zzapp_put_be32(slot_window + ZZAPP_FILE_OFF, seq + 2U);
}

static void test_per_slot_file_and_audio_state(void)
{
	aar_audio first;
	aar_audio second;
	int first_fd;
	int second_fd;
	uint32_t written;

	zzapp_services_test_reset();
	memset(g_slot_windows, 0, sizeof(g_slot_windows));
	zzapp_services_test_set_window_slot(0U, g_slot_windows[0],
					     sizeof(g_slot_windows[0]));
	zzapp_services_test_set_window_slot(1U, g_slot_windows[1],
					     sizeof(g_slot_windows[1]));
	zzapp_services_test_set_yield(slot_launcher);
	zzapp_services_select_slot(0U);
	first_fd = zzapp_svc_file_open("slot0", AAR_FILE_READ);
	zzapp_services_select_slot(1U);
	second_fd = zzapp_svc_file_open("slot1", AAR_FILE_READ);
	if (first_fd != ZZAPP_FD_BASE || second_fd != ZZAPP_FD_BASE)
		fail("per-slot file handles");
	zzapp_services_select_slot(0U);
	if (zzapp_svc_console_write("a", 1U, &written) != AAR_OK ||
	    written != 1U ||
	    g_slot_windows[0][ZZAPP_CONSOLE_OUT_OFF + ZZAPP_CONSOLE_HDR] != 'a' ||
	    zzapp_get_be32(g_slot_windows[1] + ZZAPP_CONSOLE_OUT_OFF) != 0U)
		fail("slot zero console data");
	zzapp_services_select_slot(1U);
	if (zzapp_svc_console_write("b", 1U, &written) != AAR_OK ||
	    written != 1U ||
	    g_slot_windows[1][ZZAPP_CONSOLE_OUT_OFF + ZZAPP_CONSOLE_HDR] != 'b' ||
	    g_slot_windows[0][ZZAPP_CONSOLE_OUT_OFF + ZZAPP_CONSOLE_HDR] != 'a')
		fail("slot one console data");
	zzapp_services_select_slot(0U);
	if (zzapp_svc_audio_open(22050U, 2U, &first) != AAR_OK)
		fail("first slot audio open");
	zzapp_services_select_slot(1U);
	if (zzapp_svc_audio_open(22050U, 2U, &second) != AAR_OK ||
	    RT(0).audio_block == RT(1).audio_block)
		fail("per-slot audio state");
	zzapp_services_select_slot(0U);
	if (zzapp_svc_audio_close() != AAR_OK)
		fail("first slot audio close");
	zzapp_services_select_slot(1U);
	if (zzapp_svc_audio_close() != AAR_OK)
		fail("second slot audio close");
}

static void test_app_window_ignores_mailbox_selection(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;

	zzapp_services_test_reset();
	memset(g_slot_windows, 0, sizeof(g_slot_windows));
	zzapp_services_test_set_window_slot(1U, g_slot_windows[0],
					     sizeof(g_slot_windows[0]));
	zzapp_services_test_set_window_slot(2U, g_slot_windows[1],
					     sizeof(g_slot_windows[1]));
	zzapp_services_test_set_core(0);
	zzapp_services_select_slot(1U);
	zzapp_services_set_generation(1U);
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 1U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, 1U);
	reply_len = 0U;
	if (zzapp_mailbox_dispatch(ZZAPP_OP_STATUS, request, sizeof(request),
				   reply, &reply_len, 1, 1U, 0U) != SDK_STATUS_OK)
		fail("slot one mailbox selection");
	zzapp_services_test_set_core(1);
	service_test_current_thread = (int)PID_OF(2U);
	if (zzapp_services_window() != g_slot_windows[1])
		fail("app window ignores mailbox selection");
	service_test_current_thread = -1;
}

static void test_core1_wakes_ready_waiter(void)
{

	zzapp_services_test_reset();
	memset(g_slot_windows, 0, sizeof(g_slot_windows));
	zzapp_services_test_set_window_slot(2U, g_slot_windows[1],
					     sizeof(g_slot_windows[1]));
	RT(2).wait_kind = ZZAPP_WAIT_CONSOLE_READ;
	RT(2).wait_value = 0U;
	zzapp_put_be32(g_slot_windows[1] + ZZAPP_CONSOLE_IN_OFF, 1U);
	TH(2).state = ZZAPP_THREAD_BLOCKED;
	zzapp_services_test_set_core(0);
	zzapp_services_core0_poll();
	if (TH(2).state != ZZAPP_THREAD_BLOCKED)
		fail("core zero woke waiter");
	if (service_test_wake_count != 0U)
		fail("core zero called wake");
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	if (TH(2).state != ZZAPP_THREAD_RUNNABLE)
		fail("core one did not wake waiter");
	if (service_test_wake_count != 1U)
		fail("core one wake count");
}
static void test_deferred_waits_for_system_thread(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;

	zzapp_services_test_reset();
	memset(g_slot_windows, 0, sizeof(g_slot_windows));
	zzapp_services_test_set_window_slot(0U, g_slot_windows[0],
					     sizeof(g_slot_windows[0]));
	zzapp_services_test_set_window_slot(1U, g_slot_windows[1],
					     sizeof(g_slot_windows[1]));
	zzapp_services_test_set_core(0);
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 1U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, 0U);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request),
				   reply, &reply_len, 1, 1U, 0U) != SDK_STATUS_QUEUED)
		fail("deferred request queued");
	zzapp_services_test_set_core(1);
	service_test_current_thread = (int)PID_OF(0U);
	zzapp_services_poll();
	if (__atomic_load_n(&DEF(1).state, __ATOMIC_ACQUIRE) != DEFER_POSTED)
		fail("app thread ran deferred request");
	service_test_current_thread = -1;
	zzapp_services_poll();
	if (__atomic_load_n(&DEF(1).state, __ATOMIC_ACQUIRE) != DEFER_DONE)
		fail("system thread did not run deferred request");
}
static void test_load_uses_claim_generation(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;
	uint32_t generation;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_services_test_set_core(0);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request),
				   reply, &reply_len, 1, 1U, 0U) != SDK_STATUS_OK)
		fail("first window claim");
	zzapp_services_expire_claims(ZZAPP_CLAIM_LEASE_US);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request),
				   reply, &reply_len, 1, 2U, 0U) != SDK_STATUS_OK)
		fail("second window claim");
	generation = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, NULL, 0U, reply,
				   &reply_len, 1, 3U, 0U) != SDK_STATUS_QUEUED)
		fail("empty load begin queued");
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	if (service_loader_generation != generation)
		fail("load generation differs from claim");
	if (zzapp_get_be32(g_window + ZZAPP_LIFE_GENERATION_OFF) != generation)
		fail("empty load changed slots generation");
}
static void loader_begin_succeeds(void);
static void test_mixed_legacy_and_slots_load_generation(void)
{
	uint8_t slots_request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;
	uint16_t status;
	uint32_t legacy_generation;
	uint32_t slots_generation;

	zzapp_services_test_reset();
	memset(g_slot_windows, 0, sizeof(g_slot_windows));
	zzapp_services_test_set_window_slot(0U, g_slot_windows[0],
					     sizeof(g_slot_windows[0]));
	zzapp_services_test_set_window_slot(1U, g_slot_windows[1],
					     sizeof(g_slot_windows[1]));
	zzapp_services_test_set_core(0);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, NULL, 0U, reply,
				   &reply_len, 1, 1U, 0U) != SDK_STATUS_OK)
		fail("legacy claim before mixed load");
	legacy_generation = zzapp_get_be32(g_slot_windows[0] +
					   ZZAPP_LIFE_GENERATION_OFF);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, slots_request,
				   sizeof(slots_request), reply, &reply_len, 1,
				   2U, 0U) != SDK_STATUS_OK ||
	    zzapp_get_be32(reply + ZZAPP_ALLOC_SLOT) != 1U)
		fail("slots claim before mixed load");
	slots_generation = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
	zzapp_put_be32(slots_request + ZZAPP_SEL_SLOT, 1U);
	zzapp_put_be32(slots_request + ZZAPP_SEL_GENERATION, slots_generation);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, NULL, 0U, reply,
				   &reply_len, 1, 3U, 0U) != SDK_STATUS_QUEUED ||
	    zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, slots_request,
				   sizeof(slots_request), reply, &reply_len, 1,
				   4U, 0U) != SDK_STATUS_QUEUED)
		fail("mixed load begins queued");
	service_test_loader_begin_hook = loader_begin_succeeds;
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	if (!zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, &reply_len) ||
	    status != SDK_STATUS_OK)
		fail("legacy mixed load completion");
	zzapp_mailbox_harvest_ack();
	if (zzapp_get_be32(g_slot_windows[0] + ZZAPP_LIFE_GENERATION_OFF) ==
	    legacy_generation)
		fail("legacy mixed load did not advance generation");
	zzapp_services_poll();
	if (!zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, &reply_len) ||
	    status != SDK_STATUS_OK)
		fail("slots mixed load completion");
	zzapp_mailbox_harvest_ack();
	service_test_loader_begin_hook = NULL;
	if (zzapp_get_be32(g_slot_windows[1] + ZZAPP_LIFE_GENERATION_OFF) !=
	    slots_generation)
		fail("slots mixed load changed allocation generation");
	if (zzapp_mailbox_dispatch(ZZAPP_OP_STOP, slots_request,
				   sizeof(slots_request), reply, &reply_len, 1,
				   5U, 0U) != SDK_STATUS_OK ||
	    zzapp_get_be32(g_slot_windows[1] + ZZAPP_LIFE_STOP_OFF) != 1U)
		fail("slots stop rejected allocation generation");
}
static void test_returned_buffers_are_user_granted(void)
{
	uint8_t payload[ZZAPP_BIND_REQ_BYTES];
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;
	aar_audio audio;
	uint32_t grants;
	uint32_t revokes;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_build_display_bind_request_layout(payload, 320U, 200U, 640U,
						AAR_FMT_RGB565, 0x00200000U,
						0x00210000U, AAR_LAYOUT_RGB565_LE);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, payload, sizeof(payload),
				   reply, &reply_len, 1, 1U, 0U) != SDK_STATUS_OK)
		fail("surface bind for grant");
	/* The address space is real: each grant is an alias extent of the
	 * app's space, and a revoke removes exactly it. */
	if (service_proc()->as.alias_count != 2U)
		fail("surface buffers were not granted");
	grants = service_proc()->as.alias_count;
	if (zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, payload, sizeof(payload),
				   reply, &reply_len, 1, 2U, 0U) != SDK_STATUS_BUSY)
		fail("second surface bind was accepted");
	if (service_proc()->as.alias_count != 2U)
		fail("second surface bind created aliases");
	if (zzapp_svc_display_close(1U) != AAR_OK)
		fail("surface close for revoke");
	if (service_proc()->as.alias_count != 0U)
		fail("surface close left a display alias");
	revokes = service_proc()->as.alias_count;
	if (zzapp_svc_audio_open(22050U, 2U, &audio) != AAR_OK)
		fail("audio open for grant");
	if (service_proc()->as.alias_count != revokes + 1U)
		fail("audio buffer was not granted");
	(void)grants;
	if (zzapp_svc_audio_close() != AAR_OK)
		fail("audio close for revoke");
	if (service_proc()->as.alias_count != revokes)
		fail("audio buffer was not revoked");
}
static void test_failed_starts_release_admission(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;
	uint32_t slot;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; ++slot)
		zzapp_services_test_set_window_slot(slot, g_window, sizeof(g_window));
	zzapp_services_test_set_core(0);
	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; ++slot) {
		if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
					   sizeof(request), reply, &reply_len,
					   1, slot + 1U, 0U) != SDK_STATUS_OK)
			fail("claim before failed start");
		zzapp_services_on_app_end();
		(void)zzapp_rec_retire(zzapp_services_current_pid());
	}
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
				   sizeof(request), reply, &reply_len,
				   1, 9U, 0U) != SDK_STATUS_OK)
		fail("failed starts exhausted admission");
}

static void expire_running_load(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;

	expiry_deferred_generation = DEF(0).generation;
	expiry_slot_generation = WGEN(0);
	zzapp_services_expire_claims(ZZAPP_LOAD_LEASE_US);
	expiry_claim_status = zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW,
						     request, 0U, reply,
						     &reply_len, 1, 3U, 0U);
}

static void loader_begin_succeeds(void)
{
}

static void stop_completing_load(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;

	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, WGEN(0));
	stop_completing_status = zzapp_mailbox_dispatch(ZZAPP_OP_STOP, request,
							sizeof(request), reply,
							&reply_len, 1, 3U, 0U);
	zzapp_services_core0_poll();
	stop_completing_slot_state = WSTATE(0);
	memset(request, 0, sizeof(request));
	stop_completing_claim_status = zzapp_mailbox_dispatch(
		ZZAPP_OP_ALLOC_WINDOW, request, 0U, reply, &reply_len, 1, 4U, 0U);
}

static void test_stop_cancels_completing_load(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;
	uint32_t generation;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_services_test_set_core(0);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
				   sizeof(request), reply, &reply_len,
				   1, 1U, 0U) != SDK_STATUS_OK)
		fail("claim before completing stop");
	generation = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, generation);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request),
				   reply, &reply_len, 1, 2U, 0U) != SDK_STATUS_QUEUED ||
	    !start_posted_deferred())
		fail("queue load before completing stop");
	service_test_loader_begin_hook = loader_begin_succeeds;
	service_test_deferred_completing_hook = stop_completing_load;
	run_deferred();
	service_test_deferred_completing_hook = NULL;
	service_test_loader_begin_hook = NULL;
	if (stop_completing_status != SDK_STATUS_OK)
		fail("stop did not take completing load");
	if (stop_completing_slot_state != ZZAPP_ST_STOPPING)
		fail("audio poll released completing load");
	if (stop_completing_claim_status != SDK_STATUS_BUSY)
		fail("replacement claim raced completing cancellation");
	if (WSTATE(0) != ZZAPP_ST_IDLE ||
	    __atomic_load_n(&DEF(0).state, __ATOMIC_ACQUIRE) != DEFER_IDLE ||
	    service_test_loader_abort_count != 1U)
		fail("completing stop was published after cancellation");
	memset(request, 0, sizeof(request));
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
				   sizeof(request), reply, &reply_len,
				   1, 5U, 0U) != SDK_STATUS_OK)
		fail("claim after completing stop");
}

static void test_claim_expiry_cancels_running_load(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;
	uint32_t generation;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_services_test_set_core(0);
	loading_now = 0U;
	zzapp_services_test_set_time(loading_test_time);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
				   sizeof(request), reply, &reply_len,
				   1, 1U, 0U) != SDK_STATUS_OK)
		fail("claim before expiry");
	generation = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, generation);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request),
				   reply, &reply_len, 1, 2U, 0U) != SDK_STATUS_QUEUED)
		fail("queue load before expiry");
	if (!start_posted_deferred())
		fail("start load before expiry");
	service_test_deferred_completion_hook = expire_running_load;
	run_deferred();
	service_test_deferred_completion_hook = NULL;
	if (WSTATE(0) != ZZAPP_ST_STOPPING ||
	    __atomic_load_n(&DEF(0).state, __ATOMIC_ACQUIRE) != DEFER_IDLE ||
	    service_test_loader_abort_count != 1U)
		fail("expired running load was not quarantined");
	if (expiry_claim_status != SDK_STATUS_BUSY)
		fail("expiry admitted a replacement before running load finished");
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_CHUNK, request, sizeof(request),
				   reply, &reply_len, 1, 3U, 0U) !=
	    SDK_STATUS_BAD_HANDLE)
		fail("stale running loader did not release quarantine");
	memset(request, 0, sizeof(request));
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
				   sizeof(request), reply, &reply_len,
				   1, 4U, 0U) != SDK_STATUS_OK)
		fail("claim after running expiry");
	generation = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, generation);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request),
				   reply, &reply_len, 1, 5U, 0U) != SDK_STATUS_QUEUED ||
	    WSTATE(0) != ZZAPP_ST_CLAIMED ||
	    __atomic_load_n(&DEF(0).state, __ATOMIC_ACQUIRE) != DEFER_POSTED)
		fail("replacement load was affected by expiry");
	zzapp_services_test_set_time(NULL);
}
static void start_loading(uint8_t *request, uint8_t *reply, uint16_t *reply_len,
			  int legacy, int harvest)
{
	uint32_t generation;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_services_test_set_core(0);
	zzapp_services_test_set_time(loading_test_time);
	loading_now = 0U;
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, legacy ? NULL : request,
				   legacy ? 0U : ZZAPP_MAILBOX_PAYLOAD_BYTES, reply,
				   reply_len, 1, 1U, 0U) != SDK_STATUS_OK)
		fail("claim before loading deadline");
	generation = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
	memset(request, 0, ZZAPP_MAILBOX_PAYLOAD_BYTES);
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, generation);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, legacy ? NULL : request,
				   legacy ? 0U : ZZAPP_MAILBOX_PAYLOAD_BYTES, reply,
				   reply_len, 1, 2U, 0U) != SDK_STATUS_QUEUED ||
	    !start_posted_deferred())
		fail("queue loading deadline");
	service_test_loader_begin_hook = loader_begin_succeeds;
	run_deferred();
	if (harvest) {
		if (!zzapp_mailbox_harvest(NULL, NULL, NULL, NULL, NULL, NULL))
			fail("finish loading deadline");
		zzapp_mailbox_harvest_ack();
	}
	if (WSTATE(0) != ZZAPP_ST_LOADING)
		fail("enter loading deadline");
}

static void test_abandoned_loading_is_quarantined(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;

	start_loading(request, reply, &reply_len, 1, 0);
	loading_now = ZZAPP_LOAD_LEASE_US;
	zzapp_services_expire_claims(loading_now);
	if (WSTATE(0) != ZZAPP_ST_STOPPING ||
	    service_test_loader_abort_count != 0U)
		fail("expired load was not quarantined");
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LIST, request, sizeof(request), reply,
				   &reply_len, 1, 2U, 0U) != SDK_STATUS_OK ||
	    zzapp_get_be32(reply + ZZAPP_LIST_STATE) != ZZAPP_ST_STOPPING ||
	    reply[ZZAPP_LIST_REASONS + 1U] != ZZAPP_END_QUARANTINED)
		fail("status did not report quarantined load");
	zzapp_build_chunk_request(request, 0U, 1U);
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, 0U);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_CHUNK, request, sizeof(request),
				   reply, &reply_len, 1, 4U, 0U) !=
	    SDK_STATUS_BAD_HANDLE)
		fail("stale launcher contact did not receive terminal error");
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, NULL, 0U, reply,
				   &reply_len, 1, 5U, 0U) != SDK_STATUS_BUSY)
		fail("stale contact released a live quarantined window");
	/* The cancelled loader finishes: the host's abort and retire reclaim the
	 * quarantined claim and stale contact releases it. */
	(void)zzapp_loader_abort();
	(void)zzapp_rec_retire(zzapp_services_current_pid());
	if (windows[0].quarantined || service_test_loader_abort_count != 1U)
		fail("stale contact did not release after loader teardown");
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
				   ZZAPP_MAILBOX_PAYLOAD_BYTES, reply, &reply_len,
				   1, 6U, 0U) != SDK_STATUS_OK)
		fail("stale launcher contact did not release window");
	zzapp_services_test_set_time(NULL);
}


static void test_explicit_quarantine_release_and_reset(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;

	start_loading(request, reply, &reply_len, 1, 0);
	loading_now = ZZAPP_LOAD_LEASE_US;
	zzapp_services_expire_claims(loading_now);
	(void)zzapp_loader_abort();
	(void)zzapp_rec_retire(zzapp_services_current_pid());
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, WGEN(0));
	if (zzapp_mailbox_dispatch(ZZAPP_OP_RELEASE, request, sizeof(request),
				   reply, &reply_len, 1, 4U, 0U) != SDK_STATUS_OK)
		fail("explicit release did not free quarantine");
	if (zzapp_mailbox_dispatch(ZZAPP_OP_RELEASE, request, sizeof(request),
				   reply, &reply_len, 1, 5U, 0U) != SDK_STATUS_BUSY)
		fail("explicit release accepted non-quarantined slot");
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, NULL, 0U, reply,
				   &reply_len, 1, 6U, 0U) != SDK_STATUS_OK)
		fail("explicit release did not restore admission");

	start_loading(request, reply, &reply_len, 1, 0);
	loading_now = ZZAPP_LOAD_LEASE_US;
	zzapp_services_expire_claims(loading_now);
	(void)zzapp_loader_abort();
	(void)zzapp_rec_retire(zzapp_services_current_pid());
	zzapp_services_on_reset();
	if (windows[0].quarantined || WSTATE(0) != ZZAPP_ST_IDLE)
		fail("reset did not clear quarantine");
	zzapp_services_test_set_time(NULL);
}
static void test_loading_chunk_renews_deadline(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t payload[ZZAPP_CHUNK_REQ_BYTES];
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;

	start_loading(request, reply, &reply_len, 0, 1);
	loading_now = ZZAPP_LOAD_LEASE_US - 1U;
	zzapp_build_chunk_request(payload, 0U, 1U);
	zzapp_services_test_set_core(0);
	service_test_loader_chunk_hook = loader_chunk_succeeds;
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_CHUNK, payload, sizeof(payload),
				   reply, &reply_len, 1, 3U, 0U) != SDK_STATUS_QUEUED ||
	    !start_posted_deferred())
		fail("queue slow loading chunk");
	zzapp_services_test_set_core(1);
	zzapp_services_select_slot(0U);
	run_deferred();
	service_test_loader_chunk_hook = NULL;
	if (!zzapp_mailbox_harvest(NULL, NULL, NULL, NULL, NULL, NULL))
		fail("finish slow loading chunk");
	zzapp_mailbox_harvest_ack();
	loading_now += ZZAPP_LOAD_LEASE_US - 1U;
	zzapp_services_expire_claims(loading_now);
	if (WSTATE(0) != ZZAPP_ST_LOADING ||
	    service_test_loader_abort_count != 0U)
		fail("slow loading chunk did not renew deadline");
	loading_now++;
	zzapp_services_expire_claims(loading_now);
	if (WSTATE(0) != ZZAPP_ST_STOPPING ||
	    service_test_loader_abort_count != 0U)
		fail("renewed loading deadline was not quarantined");
	zzapp_services_test_set_time(NULL);
}

static void test_loading_heartbeat_renews_deadline(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;

	start_loading(request, reply, &reply_len, 0, 1);
	loading_now = ZZAPP_LOAD_LEASE_US;
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_HEARTBEAT_OFF, 1U);
	zzapp_services_expire_claims(loading_now);
	if (WSTATE(0) != ZZAPP_ST_LOADING)
		fail("launcher heartbeat did not renew loading deadline");
	loading_now += ZZAPP_LOAD_LEASE_US - 1U;
	zzapp_services_expire_claims(loading_now);
	if (WSTATE(0) != ZZAPP_ST_LOADING)
		fail("active slow load was reclaimed");
	loading_now++;
	zzapp_services_expire_claims(loading_now);
	if (WSTATE(0) != ZZAPP_ST_STOPPING)
		fail("heartbeat-renewed loading deadline did not expire");
	zzapp_services_test_set_time(NULL);
}

static void test_claim_heartbeat_renews_deadline(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_services_test_set_time(loading_test_time);
	loading_now = 0U;
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request),
				   reply, &reply_len, 1, 1U, 0U) != SDK_STATUS_OK)
		fail("claim before claim heartbeat");
	loading_now = ZZAPP_CLAIM_LEASE_US;
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_HEARTBEAT_OFF, 1U);
	zzapp_services_expire_claims(loading_now);
	if (WSTATE(0) != ZZAPP_ST_CLAIMED)
		fail("launcher heartbeat did not renew claimed slot");
	loading_now += ZZAPP_CLAIM_LEASE_US;
	zzapp_services_expire_claims(loading_now);
	if (WSTATE(0) != ZZAPP_ST_IDLE)
		fail("renewed claimed slot did not expire");
	zzapp_services_test_set_time(NULL);
}

static void test_preload_cancellation_releases_deferred_record(void)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;
	uint32_t generation;
	uint32_t load_generation;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_services_test_set_core(0);
	loading_now = 0U;
	zzapp_services_test_set_time(loading_test_time);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, NULL, 0U, reply,
				   &reply_len, 1, 1U, 0U) != SDK_STATUS_OK)
		fail("claim before pre-load cancellation");
	generation = zzapp_get_be32(g_window + ZZAPP_LIFE_GENERATION_OFF);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, NULL, 0U, reply,
				   &reply_len, 1, 2U, 0U) != SDK_STATUS_QUEUED)
		fail("queue load before pre-load cancellation");
	if (!start_posted_deferred())
		fail("start pre-load cancellation");
	load_generation = generation + 1U;
	if (load_generation == 0U)
		load_generation = 1U;
	service_test_loader_set_generation_hook = expire_running_load;
	run_deferred();
	service_test_loader_set_generation_hook = NULL;
	if (expiry_deferred_generation != load_generation ||
	    expiry_deferred_generation != expiry_slot_generation ||
	    service_loader_generation != load_generation)
		fail("cancelled load did not keep its bumped generation");
	if (WSTATE(0) != ZZAPP_ST_STOPPING ||
	    __atomic_load_n(&DEF(0).state, __ATOMIC_ACQUIRE) != DEFER_IDLE ||
	    service_test_loader_abort_count != 1U ||
	    WGEN(0) == 0U ||
	    zzapp_get_be32(g_window + ZZAPP_LIFE_GENERATION_OFF) !=
		    WGEN(0))
		fail("pre-load cancellation did not quarantine lifecycle state");
	zzapp_services_test_set_time(NULL);
}

static void test_harvest_racing_completion_reads_reply(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;
	uint16_t status;
	uint32_t generation;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_services_test_set_core(0);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
				   sizeof(request), reply, &reply_len,
				   1, 1U, 0U) != SDK_STATUS_OK)
		fail("claim before harvest race");
	generation = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, generation);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request),
				   reply, &reply_len, 1, 2U, 0U) != SDK_STATUS_QUEUED ||
	    !start_posted_deferred())
		fail("queue load before harvest race");
	service_test_deferred_reply_hook = harvest_between_reply_and_done;
	run_deferred();
	service_test_deferred_reply_hook = NULL;
	if (harvest_reply_hook_result != 0)
		fail("harvest posted completion before DONE");
	if (!zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, NULL) ||
	    status != SDK_STATUS_UNSUPPORTED)
		fail("harvest posted stale completion status");
	zzapp_mailbox_harvest_ack();
}

static uint64_t replacement_wait_time(void)
{
	return 1U;
}

static void test_new_generation_does_not_inherit_wait(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_services_test_set_core(0);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
				   sizeof(request), reply, &reply_len,
				   1, 1U, 0U) != SDK_STATUS_OK)
		fail("claim before parked teardown");
	current_runtime()->wait_kind = ZZAPP_WAIT_FILE;
	current_runtime()->wait_value = 0U;
	current_runtime()->wait_deadline = 1U;
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(zzapp_services_current_pid());
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
				   sizeof(request), reply, &reply_len,
				   1, 2U, 0U) != SDK_STATUS_OK)
		fail("claim after parked teardown");
	TH(0).state = ZZAPP_THREAD_SLEEPING;
	zzapp_services_test_set_time(replacement_wait_time);
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	zzapp_services_test_set_time(NULL);
	if (TH(0).state != ZZAPP_THREAD_SLEEPING)
		fail("old file wait woke replacement sleep");
}


static void test_deferred_slots_queue_independently(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;

	zzapp_services_test_reset();
	memset(g_slot_windows, 0, sizeof(g_slot_windows));
	zzapp_services_test_set_window_slot(0U, g_slot_windows[0],
					     sizeof(g_slot_windows[0]));
	zzapp_services_test_set_window_slot(1U, g_slot_windows[1],
					     sizeof(g_slot_windows[1]));
	WGEN(0) = 1U;
	WGEN(1) = 1U;
	test_force_state(0U, ZZAPP_ST_CLAIMED);
	test_force_state(1U, ZZAPP_ST_CLAIMED);
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, 1U);
	zzapp_services_test_set_core(0);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request),
				   reply, &reply_len, 1, 1U, 0U) != SDK_STATUS_QUEUED)
		fail("first deferred slot request");
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 1U);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request),
				   reply, &reply_len, 1, 2U, 0U) != SDK_STATUS_QUEUED)
		fail("second deferred slot request");
}

static void test_stop_releases_claimed_slot(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;
	uint32_t generation[ZZAPP_SLOT_COUNT];
	uint32_t slot;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++)
		zzapp_services_test_set_window_slot(slot, g_window, sizeof(g_window));
	zzapp_services_test_set_core(0);
	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
					   sizeof(request), reply, &reply_len,
					   1, slot + 1U, 0U) != SDK_STATUS_OK)
			fail("claim slot before stop");
		generation[slot] = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
	}
	memset(request, 0, sizeof(request));
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, generation[0]);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_STOP, request, sizeof(request), reply,
				   &reply_len, 1, 5U, 0U) != SDK_STATUS_OK)
		fail("stop claimed slot");
	memset(request, 0, sizeof(request));
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
				   sizeof(request), reply, &reply_len,
				   1, 6U, 0U) != SDK_STATUS_OK ||
	    zzapp_get_be32(reply + ZZAPP_ALLOC_SLOT) != 0U ||
	    zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION) != generation[0] + 1U)
		fail("claimed slot was not released");
}
static void test_stop_cancels_queued_claim_load(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;
	uint32_t generation;

	zzapp_services_test_reset();
	memset(g_slot_windows, 0, sizeof(g_slot_windows));
	zzapp_services_test_set_window_slot(0U, g_slot_windows[0],
					     sizeof(g_slot_windows[0]));
	zzapp_services_test_set_core(0);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
				   sizeof(request), reply, &reply_len,
				   1, 1U, 0U) != SDK_STATUS_OK)
		fail("claim slot before queued load");
	generation = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, generation);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request),
				   reply, &reply_len, 1, 2U, 0U) != SDK_STATUS_QUEUED)
		fail("queue claimed load");
	if (zzapp_mailbox_dispatch(ZZAPP_OP_STOP, request, sizeof(request), reply,
				   &reply_len, 1, 3U, 0U) != SDK_STATUS_OK)
		fail("stop queued claimed load");
	memset(request, 0, sizeof(request));
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
				   sizeof(request), reply, &reply_len,
				   1, 4U, 0U) != SDK_STATUS_OK)
		fail("reclaim stopped slot");
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	if (service_loader_generation != 0U)
		fail("stale claimed load reached loader");
	if (WSTATE(0) != ZZAPP_ST_CLAIMED)
		fail("stale claimed load changed replacement state");
}

static void test_list_name_uses_start_argv(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	WGEN(0) = 1U;
	test_force_state(0U, ZZAPP_ST_LOADING);
	zzapp_services_set_state(ZZAPP_ST_LOADING);
	zzapp_put_be32(g_window + ZZAPP_ARGV_OFF + ZZAPP_ARGV_ARGC_OFF, 1U);
	zzapp_put_be32(g_window + ZZAPP_ARGV_OFF + ZZAPP_ARGV_OFFS_OFF,
		       ZZAPP_ARGV_OFF + ZZAPP_ARGV_STR_OFF);
	memcpy(g_window + ZZAPP_ARGV_OFF + ZZAPP_ARGV_STR_OFF,
	       "PROGDIR:bin/demo", sizeof("PROGDIR:bin/demo"));
	memset(request, 0, sizeof(request));
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, 1U);
	zzapp_put_be32(request + ZZAPP_START_ARGC, 1U);
	zzapp_services_test_set_core(0);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_START, request, sizeof(request), reply,
				   &reply_len, 1, 1U, 0U) != SDK_STATUS_QUEUED)
		fail("start queued for list name");
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	if (!zzapp_services_take_start(NULL))
		fail("start pending for list name");
	if (zzapp_services_start_entered(WGEN(0)) != AAR_OK)
		fail("start completion for list name");
	zzapp_services_test_set_core(0);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_LIST, request, sizeof(request), reply,
				   &reply_len, 1, 2U, 0U) != SDK_STATUS_OK ||
	    memcmp(reply + ZZAPP_LIST_NAME, "demo", sizeof("demo")) != 0)
		fail("list name from argv");
}

static void test_display_state_ignores_window_tampering(void)
{
	uint8_t payload[ZZAPP_BIND_REQ_BYTES];
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len;
	uint32_t front;
	uint32_t back;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_build_display_bind_request_layout(payload, 320U, 200U, 640U,
						AAR_FMT_RGB565, 0x00200000U,
						0x00210000U, AAR_LAYOUT_RGB565_LE);
	if (zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, payload, sizeof(payload),
				   reply, &reply_len, 1, 1U, 0U) != SDK_STATUS_OK)
		fail("display bind for private state");
	zzapp_put_be32(g_window + ZZAPP_LIFE_CARD_FRONT_OFF, 0x01000000U);
	zzapp_put_be32(g_window + ZZAPP_LIFE_CARD_BACK_OFF, 0x01010000U);
	if (zzapp_display_bound(NULL, NULL, NULL, NULL, &front, &back) != AAR_OK ||
	    front != 0x00200000U || back != 0x00210000U)
		fail("display trusted launcher addresses");
}

static void test_audio_teardown_quiesces_every_slot(void)
{
	aar_audio audio;

	zzapp_services_test_reset();
	memset(g_slot_windows, 0, sizeof(g_slot_windows));
	zzapp_services_test_set_window_slot(0U, g_slot_windows[0],
					     sizeof(g_slot_windows[0]));
	zzapp_services_test_set_window_slot(1U, g_slot_windows[1],
					     sizeof(g_slot_windows[1]));
	zzapp_services_select_slot(0U);
	if (zzapp_svc_audio_open(22050U, 2U, &audio) != AAR_OK)
		fail("audio open for cross-slot teardown");
	zzapp_services_on_app_end();
	zzapp_services_select_slot(1U);
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	zzapp_services_test_set_core(0);
	zzapp_services_core0_poll();
	zzapp_services_select_slot(0U);
	if (zzapp_services_test_audio_request() != AUDIO_REQUEST_NONE ||
	    zzapp_services_test_audio_detach_count() != 1U)
		fail("audio teardown stranded by another slot");
}

static void test_audio_handoff_blocks_reclaim(void)
{
	aar_audio audio;
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len = 0U;
	void *block;
	zzapp_rec_t *r0;
	uint32_t handed;
	uint32_t slot;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++)
		zzapp_services_test_set_window_slot(slot, g_window, sizeof(g_window));
	zzapp_services_test_set_core(0);
	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
					   sizeof(request), reply, &reply_len,
					   1, slot + 1U, 0U) != SDK_STATUS_OK)
			fail("claim before audio handoff");
		give_space(slot);
	}
	zzapp_services_select_slot(0U);
	if (zzapp_svc_audio_open(22050U, 2U, &audio) != AAR_OK)
		fail("audio open before blocked reclaim");
	block = current_runtime()->audio_block;
	r0 = service_rec();
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(zzapp_services_current_pid());
	handed = zzapp_services_test_audio_request();
	if (WSTATE(0) == ZZAPP_ST_IDLE)
		fail("slot idle before audio handoff finished");
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request),
				   reply, &reply_len, 1, 8U, 0U) == SDK_STATUS_OK)
		fail("alloc reclaimed a slot during audio handoff");
	zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
	zzapp_put_be32(request + ZZAPP_SEL_GENERATION, 1U);
	(void)zzapp_mailbox_dispatch(ZZAPP_OP_STOP, request, sizeof(request),
				     reply, &reply_len, 1, 9U, 0U);
	zzapp_services_select_slot(0U);
	if (current_runtime()->audio_block != block ||
	    zzapp_services_test_audio_request() != handed ||
	    zzapp_services_test_audio_detach_count() != 0U)
		fail("stop zeroed live audio during handoff");
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	zzapp_services_test_set_core(0);
	zzapp_services_core0_poll();
	zzapp_services_select_slot(0U);
	if (WSTATE(0) != ZZAPP_ST_IDLE ||
	    runtime_of(r0)->audio_detach_count != 1U ||
	    runtime_of(r0)->audio_request != AUDIO_REQUEST_NONE)
		fail("slot stayed busy after audio handoff");
	memset(request, 0, sizeof(request));
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request),
				   reply, &reply_len, 1, 10U, 0U) != SDK_STATUS_OK ||
	    zzapp_get_be32(reply + ZZAPP_ALLOC_SLOT) != 0U)
		fail("alloc after audio handoff");
}
static void test_cancelled_pending_open_releases_slot(void)
{
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len = 0U;
	uint32_t slot;
	zzapp_rec_t *r0;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++)
		zzapp_services_test_set_window_slot(slot, g_window, sizeof(g_window));
	zzapp_services_test_set_core(0);
	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
					   sizeof(request), reply, &reply_len,
					   1, slot + 1U, 0U) != SDK_STATUS_OK)
			fail("claim before cancelled open");
	}
	zzapp_services_select_slot(0U);
	r0 = service_rec();
	zzapp_services_test_set_audio_request(AUDIO_REQUEST_OPEN);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(zzapp_services_current_pid());
	if (WSTATE(0) == ZZAPP_ST_IDLE)
		fail("slot idle while open was still pending teardown");
	zzapp_services_core0_poll();
	zzapp_services_select_slot(0U);
	if (runtime_of(r0)->audio_teardown != 0U ||
	    runtime_of(r0)->audio_request != AUDIO_REQUEST_NONE ||
	    WSTATE(0) != ZZAPP_ST_IDLE)
		fail("cancelled open left slot unreleased");
	memset(request, 0, sizeof(request));
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request),
				   reply, &reply_len, 1, 8U, 0U) != SDK_STATUS_OK ||
	    zzapp_get_be32(reply + ZZAPP_ALLOC_SLOT) != 0U)
		fail("alloc failed after cancelled open release");
}

static void test_reclaim_halted_audio_reclaims_every_slot(void)
{
	aar_audio audio[2];
	uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t reply_len = 0U;
	uint32_t slot;
	zzapp_rec_t *rec0;
	zzapp_rec_t *rec1;
	uint32_t pid0;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++)
		zzapp_services_test_set_window_slot(slot, g_window, sizeof(g_window));
	zzapp_services_test_set_core(0);
	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
					   sizeof(request), reply, &reply_len,
					   1, slot + 1U, 0U) != SDK_STATUS_OK)
			fail("claim before multi-producer open");
		give_space(slot);
	}
	zzapp_services_select_slot(0U);
	rec0 = service_rec();
	pid0 = zzapp_services_current_pid();
	if (zzapp_svc_audio_open(22050U, 2U, &audio[0]) != AAR_OK)
		fail("producer 0 open");
	zzapp_services_select_slot(1U);
	rec1 = service_rec();
	if (zzapp_svc_audio_open(22050U, 2U, &audio[1]) != AAR_OK)
		fail("producer 1 open");
	zzapp_services_select_slot(0U);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(zzapp_services_current_pid());
	zzapp_services_select_slot(1U);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(zzapp_services_current_pid());
	if (WSTATE(0) == ZZAPP_ST_IDLE || WSTATE(1) == ZZAPP_ST_IDLE)
		fail("producer idle before halted reclaim");
	/* Core 1 is halted: no services_poll() runs to quiesce CLOSE.
	 * Select slot 0 and invoke reclaim once: both slots must be freed
	 * and slot 0 must remain selected. */
	zzapp_services_select_slot(0U);
	zzapp_services_reclaim_halted_audio();
	if (zzapp_services_current_pid() != pid0)
		fail("halted reclaim did not preserve slot selection");
	if (WSTATE(0) != ZZAPP_ST_IDLE || WSTATE(1) != ZZAPP_ST_IDLE)
		fail("halted reclaim left a producer slot unreleased");
	if (runtime_of(rec0)->audio_block != NULL ||
	    runtime_of(rec1)->audio_block != NULL)
		fail("halted reclaim left an audio block allocated");
	if (runtime_of(rec0)->audio_detach_count != 1U ||
	    runtime_of(rec1)->audio_detach_count != 1U)
		fail("halted reclaim did not detach both producers");
	memset(request, 0, sizeof(request));
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request),
				   reply, &reply_len, 1, 9U, 0U) != SDK_STATUS_OK ||
	    zzapp_get_be32(reply + ZZAPP_ALLOC_SLOT) != 0U)
		fail("alloc slot 0 after halted reclaim");
	memset(request, 0, sizeof(request));
	if (zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request),
				   reply, &reply_len, 1, 10U, 0U) != SDK_STATUS_OK ||
	    zzapp_get_be32(reply + ZZAPP_ALLOC_SLOT) != 1U)
		fail("alloc slot 1 after halted reclaim");
}
static uint64_t frozen_file_time(void)
{
	return 0U;
}

static void scheduler_file_timeout(void)
{
	service_rec()->thread.wake_reason =
		ZZAPP_WAKE_TIMEOUT;
}

static void test_scheduler_file_timeout_cancels_seqlock(void)
{
	int fd;
	uint32_t seq;
	uint32_t op;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_services_set_generation(1U);
	zzapp_services_test_set_time(frozen_file_time);
	zzapp_services_test_set_yield(scheduler_file_timeout);
	fd = zzapp_svc_file_open("late.pak", AAR_FILE_READ);
	if (fd != AAR_E_TIMEDOUT)
		fail("scheduler file timeout");
	seq = zzapp_get_be32(g_window + ZZAPP_FILE_OFF);
	op = zzapp_get_be32(g_window + ZZAPP_FILE_OFF + 4U);
	if ((seq & 1U) == 0U)
		fail("scheduler timeout left an even file sequence");
	if (op != 0U)
		fail("scheduler timeout left the file op armed");
	if (current_runtime()->file_seq < 2U ||
	    (current_runtime()->file_seq & 1U) != 0U)
		fail("scheduler timeout did not advance file_seq");
}

static void console_eof_yield(void)
{
	zzapp_put_be32(g_window + ZZAPP_CONSOLE_IN_OFF + ZZAPP_CONSOLE_EOF_OFF, 1U);
}

static void test_console_eof_ends_wait(void)
{
	uint8_t buf[4];
	uint32_t n = 99U;

	zzapp_services_test_reset();
	memset(&service_test_kernel, 0, sizeof(service_test_kernel));
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_services_set_generation(1U);
	zzapp_services_test_set_yield(console_eof_yield);
	if (zzapp_svc_console_read(buf, sizeof(buf), &n) != AAR_OK || n != 0U)
		fail("console eof read");
	if (zzapp_services_wait_registered(PID_OF(0U)))
		fail("console eof left the wait registered");
}





static unsigned loader_end_passes;
static unsigned loader_end_complete_after;

static int loader_end_in_passes(void)
{
	loader_end_passes++;
	return loader_end_passes < loader_end_complete_after ?
		ZZAPP_LOADER_IN_PROGRESS : AAR_OK;
}

static void queue_loading_end(void)
{
	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	test_force_state(0U, ZZAPP_ST_LOADING);
	WGEN(0) = 1U;
	service_loader_state = ZZAPP_ST_LOADING;
	DEF(0).state = DEFER_POSTED;
	DEF(0).opcode = ZZAPP_OP_LOAD_END;
	DEF(0).generation = 1U;
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
	if (loader_end_passes != loader_end_complete_after ||
	    DEF(0).state != DEFER_DONE)
		fail("load end did not finish after bounded zero batches");

	queue_loading_end();
	loader_end_complete_after = UINT32_MAX;
	zzapp_services_poll();
	if (DEF(0).state != DEFER_RUNNING || loader_end_passes != 1U)
		fail("load end did not begin bounded zero batch");
	DEF(0).cancelled = 1U;
	zzapp_services_poll();
	if (DEF(0).state != DEFER_IDLE ||
	    service_test_loader_abort_count != 1U)
		fail("cancelled load end did not abort");
}

_Static_assert(ZZAPP_FILE_READ == AAR_FILE_READ &&
	       ZZAPP_FILE_WRITE == AAR_FILE_WRITE &&
	       ZZAPP_FILE_CREATE == AAR_FILE_CREATE &&
	       ZZAPP_FILE_TRUNCATE == AAR_FILE_TRUNCATE &&
	       ZZAPP_FILE_EXCL == AAR_FILE_EXCL,
	       "wire file flags are the AAR flags");

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

/* Every valid combination reaches the launcher with the exact value at
 * FILE_OFF+12; every invalid one is AAR_E_INVAL and publishes nothing: no
 * request, and neither the file block nor the path buffer changes. */
static void test_file_open_flags(void)
{
	static const uint32_t unknown[] = {
		32U, 64U, 0x80U, 0x100U, 0x8000U, 0x80000000U, 0xffffffffU,
		0xffffffe3U, 32U | AAR_FILE_READ, 32U | AAR_FILE_READ | AAR_FILE_WRITE,
		0x80000000U | AAR_FILE_READ,
	};
	uint8_t before[ZZAPP_FILE_BYTES + ZZAPP_PATH_MAX + 1U];
	uint32_t flags;
	uint32_t valid = 0U;
	uint32_t invalid = 0U;
	unsigned i;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	g_last_seq = 0U;
	g_bump = 0;
	g_open_count = 0U;
	g_open_flags = 0U;
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_services_test_set_yield(launcher);
	zzapp_services_set_generation(1U);
	for (i = 0U; i < 64U + sizeof(unknown) / sizeof(unknown[0]); i++) {
		uint32_t opens = g_open_count;
		int fd;

		flags = i < 64U ? i : unknown[i - 64U];
		memcpy(before, g_window + ZZAPP_FILE_OFF, ZZAPP_FILE_BYTES);
		memcpy(before + ZZAPP_FILE_BYTES, g_window + ZZAPP_PATH_OFF,
		       ZZAPP_PATH_MAX + 1U);
		fd = zzapp_svc_file_open("flags.bin", flags);
		if (open_flags_valid(flags)) {
			valid++;
			if (fd < ZZAPP_FD_BASE) {
				fail("valid open flags refused");
				continue;
			}
			if (g_open_count != opens + 1U)
				fail("valid open flags were not published");
			if (g_open_flags != flags)
				fail("open flags changed on the wire");
			if (zzapp_svc_file_close(fd) != AAR_OK)
				fail("close after open");
		} else {
			invalid++;
			if (fd != AAR_E_INVAL)
				fail("invalid open flags were not AAR_E_INVAL");
			if (g_open_count != opens)
				fail("invalid open flags reached the launcher");
			if (memcmp(before, g_window + ZZAPP_FILE_OFF,
				   ZZAPP_FILE_BYTES) != 0 ||
			    memcmp(before + ZZAPP_FILE_BYTES,
				   g_window + ZZAPP_PATH_OFF,
				   ZZAPP_PATH_MAX + 1U) != 0)
				fail("invalid open flags published to the window");
		}
	}
	/* READ alone (1), and the writers: READ or not (2) times TRUNCATE or
	 * not (2) times none, CREATE, CREATE|EXCL (3). */
	if (valid != 13U ||
	    invalid != 64U - 13U + (uint32_t)(sizeof(unknown) / sizeof(unknown[0])))
		fail("count of valid flag combinations");
}

int main(void)
{
	svc_env_init(ZZAPP_SLOT_COUNT < 8U ? 8U : ZZAPP_SLOT_COUNT + 4U, 0U, NULL);
	test_payload_sizes();
	test_z2_query();
	test_chunked_read();
	test_drop_on_full();
	test_audio_busy();
	test_late_file_reply();
	test_audio_teardown_handoff();
	test_reset_preserves_audio_teardown();
	test_audio_write_cursor_wrap();
	test_display_refresh_rotation_and_layout();
	test_display_scanout_ack();
	test_display_wait_flip_idle();
	test_clut8_palette_publication();
	test_display_mailbox();
	test_display_mailbox_uses_scanout_owner();
	test_per_slot_file_and_audio_state();
	test_app_window_ignores_mailbox_selection();
	test_core1_wakes_ready_waiter();
	test_deferred_waits_for_system_thread();
	test_load_uses_claim_generation();
	test_mixed_legacy_and_slots_load_generation();
	test_load_end_resumes_and_cancels();
	test_returned_buffers_are_user_granted();
	test_failed_starts_release_admission();
	test_claim_expiry_cancels_running_load();
	test_abandoned_loading_is_quarantined();
	test_explicit_quarantine_release_and_reset();
	test_loading_chunk_renews_deadline();
	test_loading_heartbeat_renews_deadline();
	test_claim_heartbeat_renews_deadline();
	test_stop_cancels_completing_load();
	test_preload_cancellation_releases_deferred_record();
	test_new_generation_does_not_inherit_wait();
	test_deferred_slots_queue_independently();
	test_harvest_racing_completion_reads_reply();
	test_stop_releases_claimed_slot();
	test_stop_cancels_queued_claim_load();
	test_list_name_uses_start_argv();
	test_display_state_ignores_window_tampering();
	test_audio_teardown_quiesces_every_slot();
	test_audio_handoff_blocks_reclaim();
	test_cancelled_pending_open_releases_slot();
	test_reclaim_halted_audio_reclaims_every_slot();
	test_scheduler_file_timeout_cancels_seqlock();
	test_console_eof_ends_wait();
	test_file_open_flags();
	if (g_fails != 0) {
		fprintf(stderr, "%d failure(s)\n", g_fails);
		return 1;
	}
	puts("zzapp services: ok");
	return 0;
}
