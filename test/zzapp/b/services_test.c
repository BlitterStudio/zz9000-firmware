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

#include <stdio.h>
#include <string.h>

#define READ_BYTES (300u * 1024u)

static uint8_t g_window[ZZAPP_WINDOW_BYTES];
static uint32_t g_produced;
static uint32_t g_last_seq;
static int g_bump;
static int g_fails;

static void fail(const char *what)
{
	fprintf(stderr, "fail: %s\n", what);
	g_fails++;
}

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
	fd = zzapp_svc_file_open("id1/pak0.pak", ZZ_FILE_READ);
	if (fd < 3)
		fail("open");
	rc = zzapp_svc_file_read(fd, buf, READ_BYTES, &got);
	if (rc != ZZ_OK)
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
	if (rc != ZZ_EIO)
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
	zzapp_put_be16(g_window + off, (uint16_t)ZZAPP_EV_KEY_DOWN);
	zzapp_put_be16(g_window + off + 2U, code);
	zzapp_put_be32(g_window + ZZAPP_INPUT_OFF, head + 1U);
}

static void test_drop_on_full(void)
{
	zzapp_event ev[8];
	uint32_t n = 0U;
	uint32_t i;
	uint32_t got = 0U;
	int rc;

	for (i = 0; i < ZZAPP_INPUT_CAP + 1U; i++)
		push_event((uint16_t)i);
	if (zzapp_get_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_INPUT_DROPS_OFF) != 1U)
		fail("input drop count");
	while (got < ZZAPP_INPUT_CAP) {
		rc = zzapp_svc_input_poll(ev, 8U, &n);
		if (rc != ZZ_OK || n == 0U) {
			fail("input poll");
			return;
		}
		for (i = 0; i < n; i++) {
			if (ev[i].code != (uint16_t)(got + i) ||
			    ev[i].type != ZZ_EV_KEY_DOWN) {
				fail("input order");
				return;
			}
		}
		got += n;
	}
	rc = zzapp_svc_input_poll(ev, 8U, &n);
	if (rc != ZZ_OK || n != 0U)
		fail("input drained");
	if (got != ZZAPP_INPUT_CAP)
		fail("input count");
}

static void test_audio_busy(void)
{
	zzapp_audio audio;
	int rc;

	zzapp_services_test_set_slot_busy(2U, 1);
	zzapp_services_test_set_slot_busy(1U, 1);
	rc = zzapp_svc_audio_open(22050U, 2U, &audio);
	if (rc != ZZ_EBUSY)
		fail("audio busy");
	rc = zzapp_svc_audio_open(11025U, 2U, &audio);
	if (rc != ZZ_EINVAL)
		fail("audio 11025");
	zzapp_services_test_set_slot_busy(2U, 0);
	zzapp_services_test_set_slot_busy(1U, 0);
	rc = zzapp_svc_audio_open(22050U, 2U, &audio);
	if (rc != ZZ_OK || audio.rate != 22050U || audio.channels != 2U ||
	    audio.frames != 8192U)
		fail("audio grant");
	if (zzapp_svc_audio_close() != ZZ_OK)
		fail("audio close");
}

static void test_display_refresh_rotation_and_layout(void)
{
	uint8_t payload[ZZAPP_BIND_REQ_BYTES];
	uint8_t reply[48];
	uint16_t reply_len = 0U;
	zzapp_surface surface;
	uint32_t front = 0x00200000U;
	uint32_t back = 0x00210000U;
	int rc;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	/* zz_display_open raises the request; the bind must clear it or the
	 * launcher and the app both treat the display as unbound. */
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLAGS_OFF,
		       ZZAPP_F_DISPLAY_UNBOUND);
	zzapp_build_display_bind_request_layout(payload, 320U, 200U, 640U,
						 ZZ_FMT_RGB565, front, back,
						 ZZ_LAYOUT_RGB565_LE);
	rc = zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, payload, sizeof(payload),
				    reply, &reply_len, 1, 1U, 0U);
	if (rc != SDK_STATUS_OK ||
	    (zzapp_services_flags() & ZZAPP_F_DISPLAY_UNBOUND) != 0U)
		fail("display bind");
	memset(&surface, 0, sizeof(surface));
	surface.id = 1U;
	if (zzapp_svc_display_refresh(&surface) != ZZ_OK ||
	    surface.front != (void *)(uintptr_t)front ||
	    surface.back != (void *)(uintptr_t)back ||
	    surface.layout != ZZ_LAYOUT_RGB565_LE)
		fail("display initial surface");
	/* An old launcher does not negotiate scanout acknowledgement, so its
	 * established lifecycle ack remains authoritative. */
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLIP_GEN_OFF, 1U);
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLIP_ADDRESS_OFF,
		       back);
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLIP_SUBMIT_OFF,
		       1U);
	zzapp_services_display_vblank(back);
	if (zzapp_get_be32(g_window + ZZAPP_LIFE_OFF +
			   ZZAPP_LIFE_FLIP_ACK_OFF) != 0U)
		fail("display old launcher compatibility");
	if (zzapp_svc_display_refresh(&surface) != ZZ_EBUSY)
		fail("display pending refresh");
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLIP_ACK_OFF, 1U);
	if (zzapp_svc_display_refresh(&surface) != ZZ_OK ||
	    surface.front != (void *)(uintptr_t)back ||
	    surface.back != (void *)(uintptr_t)front ||
	    surface.layout != ZZ_LAYOUT_RGB565_LE)
		fail("display refreshed rotation");
}

static void test_display_scanout_ack(void)
{
	uint8_t payload[ZZAPP_BIND_EXT_REQ_BYTES];
	uint8_t reply[48];
	uint16_t reply_len = 0U;
	zzapp_surface surface;
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
						ZZ_FMT_RGB565, front, back,
						ZZ_LAYOUT_RGB565_LE, spare);
	rc = zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, payload, sizeof(payload),
				    reply, &reply_len, 1, 1U, 0U);
	if (rc != SDK_STATUS_OK ||
	    (zzapp_services_flags() & ZZAPP_F_SCANOUT_ACK) == 0U)
		fail("display scanout-ack bind");
	memset(&surface, 0, sizeof(surface));
	surface.id = 1U;
	if (zzapp_svc_display_flip_async(surface.id) != ZZ_OK ||
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
	if (zzapp_svc_display_refresh(&surface) != ZZ_OK ||
	    surface.front != (void *)(uintptr_t)back ||
	    surface.back != (void *)(uintptr_t)spare)
		fail("display scanout-ack recycle");
}

static void test_display_mailbox(void)
{
	uint8_t payload[ZZAPP_BIND_EXT_REQ_BYTES];
	uint8_t reply[48];
	uint16_t reply_len = 0U;
	zzapp_surface surface;
	uint32_t scanout;
	uint32_t front = 0x00200000U;
	uint32_t back = 0x00210000U;
	uint32_t spare = 0x00220000U;
	int rc;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLAGS_OFF,
		       ZZAPP_F_LAUNCHER_MAILBOX | ZZAPP_F_MAILBOX_REQUEST |
		       ZZAPP_F_DISPLAY_UNBOUND);
	zzapp_build_display_bind_request_triple(payload, 320U, 200U, 640U,
						ZZ_FMT_RGB565, front, back,
						ZZ_LAYOUT_RGB565_LE, spare);
	rc = zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, payload, sizeof(payload),
				    reply, &reply_len, 1, 1U, 0U);
	if (rc != SDK_STATUS_OK ||
	    (zzapp_services_flags() & ZZAPP_F_MAILBOX) == 0U ||
	    (zzapp_services_flags() & ZZAPP_F_DISPLAY_UNBOUND) != 0U)
		fail("display mailbox bind");
	memset(&surface, 0, sizeof(surface));
	surface.id = 1U;
	if (zzapp_svc_display_flip_async(surface.id) != ZZ_OK ||
	    zzapp_svc_display_refresh(&surface) != ZZ_OK ||
	    surface.back != (void *)(uintptr_t)spare)
		fail("display mailbox first queue");
	if (zzapp_svc_display_flip_async(surface.id) != ZZ_OK ||
	    zzapp_svc_display_refresh(&surface) != ZZ_OK ||
	    surface.back != (void *)(uintptr_t)back ||
	    zzapp_svc_display_mailbox_replaced() != 1U)
		fail("display mailbox replace");
	if (!zzapp_services_display_mailbox_begin(front, &scanout) ||
	    scanout != front)
		fail("display mailbox waits for vblank flush");
	zzapp_services_display_mailbox_end(1);
	zzapp_services_display_flush_begin();
	zzapp_services_display_flush_done();
	if (!zzapp_services_display_mailbox_begin(front, &scanout) ||
	    scanout != spare)
		fail("display mailbox select latest");
	zzapp_services_display_mailbox_end(1);
	if (zzapp_svc_display_refresh(&surface) != ZZ_OK ||
	    surface.front != (void *)(uintptr_t)spare ||
	    surface.back != (void *)(uintptr_t)back ||
	    zzapp_get_be32(g_window + ZZAPP_LIFE_OFF +
			   ZZAPP_LIFE_CARD_SPARE_OFF) != front)
		fail("display mailbox latch reuse");
	if (zzapp_svc_display_flip_async(surface.id) != ZZ_OK)
		fail("display mailbox second queue");
	if (zzapp_services_display_mailbox_begin(0x00300000U, &scanout))
		fail("display mailbox hidden screen");
	if (zzapp_svc_display_refresh(&surface) != ZZ_OK ||
	    surface.back != (void *)(uintptr_t)front)
		fail("display mailbox hidden ownership");

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

/* A frame queued while core 0's L2 flush is already running may have lines
 * that flush passed over; only a flush that starts after the submit makes
 * it safe to scan. */
static void test_display_mailbox_flush_race(void)
{
	uint8_t payload[ZZAPP_BIND_EXT_REQ_BYTES];
	uint8_t reply[48];
	uint16_t reply_len = 0U;
	uint32_t scanout;
	uint32_t front = 0x00200000U;
	uint32_t back = 0x00210000U;
	uint32_t spare = 0x00220000U;
	int rc;

	zzapp_services_test_reset();
	memset(g_window, 0, sizeof(g_window));
	zzapp_services_test_set_window(g_window, sizeof(g_window));
	zzapp_put_be32(g_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLAGS_OFF,
		       ZZAPP_F_LAUNCHER_MAILBOX | ZZAPP_F_MAILBOX_REQUEST |
		       ZZAPP_F_DISPLAY_UNBOUND);
	zzapp_build_display_bind_request_triple(payload, 320U, 200U, 640U,
						ZZ_FMT_RGB565, front, back,
						ZZ_LAYOUT_RGB565_LE, spare);
	rc = zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, payload, sizeof(payload),
				    reply, &reply_len, 1, 1U, 0U);
	if (rc != SDK_STATUS_OK)
		fail("display mailbox race bind");
	zzapp_services_display_flush_begin();
	if (zzapp_svc_display_flip_async(1U) != ZZ_OK)
		fail("display mailbox race queue");
	zzapp_services_display_flush_done();
	if (!zzapp_services_display_mailbox_begin(front, &scanout) ||
	    scanout != front)
		fail("display mailbox latched after a flush that began before it");
	zzapp_services_display_mailbox_end(1);
	zzapp_services_display_flush_begin();
	zzapp_services_display_flush_done();
	if (!zzapp_services_display_mailbox_begin(front, &scanout) ||
	    scanout != back)
		fail("display mailbox not latched after a full later flush");
	zzapp_services_display_mailbox_end(1);
}

int main(void)
{
	test_payload_sizes();
	test_z2_query();
	test_chunked_read();
	test_drop_on_full();
	test_audio_busy();
	test_display_refresh_rotation_and_layout();
	test_display_scanout_ack();
	test_display_mailbox();
	test_display_mailbox_flush_race();
	if (g_fails != 0) {
		fprintf(stderr, "%d failure(s)\n", g_fails);
		return 1;
	}
	puts("zzapp services: ok");
	return 0;
}
