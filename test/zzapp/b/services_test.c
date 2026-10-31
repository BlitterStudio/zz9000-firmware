/*
 * Slice B host tests: seqlock, chunked read, drop-on-full, audio busy,
 * mailbox payloads fit in 48 bytes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#define _Static_assert(cond, msg) typedef char zzapp_sa_##__LINE__[(cond) ? 1 : -1]
#endif

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

int main(void)
{
	test_payload_sizes();
	test_z2_query();
	test_chunked_read();
	test_drop_on_full();
	test_audio_busy();
	if (g_fails != 0) {
		fprintf(stderr, "%d failure(s)\n", g_fails);
		return 1;
	}
	puts("zzapp services: ok");
	return 0;
}
