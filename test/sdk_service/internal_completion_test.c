/*
 * Internal-task completion dispatch (request_id == 0) through the production
 * sdk_internal_completion.h unit that sdk_mailbox_post_deferred calls.
 *
 * Each internal producer's completion must retire exactly its own marker or
 * resource: a media close releases its session and PCM ring, a video close
 * retires its overlay session, and only an AX refill clears the refill
 * marker.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdk_internal_completion.h"

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

struct record {
	int compose;
	int compose_ok;
	int output_retired;
	int output_ok;
	uint32_t output_session;
	uint32_t output_us;
	int fw_retire;
	uint16_t fw_code;
	int fw_failed;
	int zzm_load;
	int media_retired;
	uint32_t media_session;
	int pcm_released;
	uint32_t pcm_session;
	int ax_refill;
};

static struct record rec;

static void on_compose(int ok) { rec.compose++; rec.compose_ok = ok; }
static void on_output(int ok, uint32_t session, uint32_t us)
{
	rec.output_retired++;
	rec.output_ok = ok;
	rec.output_session = session;
	rec.output_us = us;
}
static void on_fw_retire(uint16_t code) { rec.fw_retire++; rec.fw_code = code; }
static void on_fw_failed(void) { rec.fw_failed++; }
static void on_zzm_load(int ok) { (void)ok; rec.zzm_load++; }
static void on_media_retired(uint32_t s) { rec.media_retired++; rec.media_session = s; }
static void on_pcm_release(uint32_t s) { rec.pcm_released++; rec.pcm_session = s; }
static void on_ax_refill(void) { rec.ax_refill++; }

static const struct sdk_internal_completion_hooks hooks = {
	.overlay_compose_retired = on_compose,
	.media_output_retired = on_output,
	.fw_update_retire = on_fw_retire,
	.fw_update_retire_failed = on_fw_failed,
	.zzm_load_finished = on_zzm_load,
	.media_close_retired = on_media_retired,
	.pcm_ring_release = on_pcm_release,
	.ax_refill_done = on_ax_refill,
};

static void session_payload(uint8_t *payload, uint32_t session)
{
	memset(payload, 0, 16);
	payload[0] = (uint8_t)(session >> 24);
	payload[1] = (uint8_t)(session >> 16);
	payload[2] = (uint8_t)(session >> 8);
	payload[3] = (uint8_t)session;
}

static void dispatch(uint16_t opcode, uint16_t status, const uint8_t *payload,
                     uint16_t len)
{
	memset(&rec, 0, sizeof(rec));
	sdk_internal_completion_dispatch(&hooks, opcode, status, payload, len);
}

static void test_media_close_releases_session_and_ring(void)
{
	uint8_t payload[16];

	session_payload(payload, 0x00010203U);
	dispatch(SDK_OP_MEDIA_SESSION_CLOSE, SDK_STATUS_OK, payload, 16);
	CHECK(rec.media_retired == 1);
	CHECK(rec.media_session == 0x00010203U);
	CHECK(rec.pcm_released == 1);
	CHECK(rec.pcm_session == 0x00010203U);
	CHECK(rec.ax_refill == 0);
}

static void test_failed_media_close_retires_nothing(void)
{
	uint8_t payload[16];

	session_payload(payload, 7U);
	dispatch(SDK_OP_MEDIA_SESSION_CLOSE, SDK_STATUS_BUSY, payload, 16);
	CHECK(rec.media_retired == 0);
	CHECK(rec.pcm_released == 0);
	CHECK(rec.ax_refill == 0);
}

static void test_short_payload_identifies_no_session(void)
{
	uint8_t payload[2] = { 0, 1 };

	dispatch(SDK_OP_MEDIA_SESSION_CLOSE, SDK_STATUS_OK, payload, 2);
	CHECK(rec.media_retired == 0);
	CHECK(rec.pcm_released == 0);
	CHECK(rec.ax_refill == 0);
}

static void test_close_opcodes_leave_refill_marker_alone(void)
{
	uint8_t payload[16];

	session_payload(payload, 1U);
	dispatch(SDK_OP_IMAGE_SESSION_CLOSE, SDK_STATUS_OK, payload, 16);
	CHECK(rec.ax_refill == 0);
	dispatch(SDK_OP_AUDIO_STREAM_CLOSE, SDK_STATUS_OK, payload, 16);
	CHECK(rec.ax_refill == 0);
}

static void test_ax_refill_clears_its_marker(void)
{
	dispatch(SDK_OP_AUDIO_STREAM_FEED, SDK_STATUS_OK, 0, 0);
	CHECK(rec.ax_refill == 1);
	CHECK(rec.media_retired == 0);
	CHECK(rec.pcm_released == 0);
}

/* A shared-buffer conversion retires its own marker, never the overlay's
 * compose marker or the AX refill marker. */
static void test_media_output_retires_its_own_marker(void)
{
	uint8_t payload[8];

	memset(payload, 0, sizeof(payload));
	payload[3] = 7U;                 /* session */
	payload[6] = 0x12U;              /* 0x1234 microseconds */
	payload[7] = 0x34U;
	dispatch((uint16_t)TASKQ_OP_MEDIA_OUTPUT, SDK_STATUS_OK, payload, 8);
	CHECK(rec.output_retired == 1 && rec.output_ok == 1);
	CHECK(rec.output_session == 7U && rec.output_us == 0x1234U);
	CHECK(rec.compose == 0 && rec.ax_refill == 0 && rec.media_retired == 0);

	/* a failed task retires too, so the in-flight marker cannot stick */
	dispatch((uint16_t)TASKQ_OP_MEDIA_OUTPUT, SDK_STATUS_BAD_REQUEST, 0, 0);
	CHECK(rec.output_retired == 1 && rec.output_ok == 0);
	CHECK(rec.output_session == 0U && rec.output_us == 0U);
	CHECK(rec.compose == 0 && rec.ax_refill == 0);

	/* a payload without the time still names its session */
	dispatch((uint16_t)TASKQ_OP_MEDIA_OUTPUT, SDK_STATUS_OK, payload, 4);
	CHECK(rec.output_session == 7U && rec.output_us == 0U);
}

static void test_other_internal_producers_keep_their_routes(void)
{
	uint8_t code[2] = { 0x12, 0x34 };

	dispatch((uint16_t)TASKQ_OP_VIDEO_COMPOSE, SDK_STATUS_OK, 0, 0);
	CHECK(rec.compose == 1 && rec.compose_ok == 1 && rec.ax_refill == 0);
	dispatch((uint16_t)TASKQ_OP_FWUP, SDK_STATUS_OK, code, 2);
	CHECK(rec.fw_retire == 1 && rec.fw_code == 0x1234U && rec.ax_refill == 0);
	dispatch((uint16_t)TASKQ_OP_FWUP, SDK_STATUS_INTERNAL_ERROR, 0, 0);
	CHECK(rec.fw_failed == 1 && rec.ax_refill == 0);
	dispatch((uint16_t)TASKQ_OP_ZZM_LOAD, SDK_STATUS_OK, 0, 0);
	CHECK(rec.zzm_load == 1 && rec.ax_refill == 0);
}

int main(void)
{
	test_media_close_releases_session_and_ring();
	test_failed_media_close_retires_nothing();
	test_short_payload_identifies_no_session();
	test_close_opcodes_leave_refill_marker_alone();
	test_ax_refill_clears_its_marker();
	test_media_output_retires_its_own_marker();
	test_other_internal_producers_keep_their_routes();
	if (failures != 0) {
		printf("internal_completion_test: %d failure(s) of %d checks\n",
		       failures, checks);
		return 1;
	}
	printf("internal_completion_test: %d checks passed\n", checks);
	return 0;
}
