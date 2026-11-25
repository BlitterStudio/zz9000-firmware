/*
 * Internal-task completion dispatch: the request_id==0 branch of
 * sdk_mailbox_post_deferred, extracted so a host suite can drive the exact
 * production retirement decisions (opcode -> which producer marker or
 * resource is retired) without compiling sdk_mailbox.c.
 *
 * Header-only production unit (the sdk_client_release.h pattern): the
 * mailbox installs one hooks table wrapping its firmware calls; the host
 * tests install their own.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_INTERNAL_COMPLETION_H
#define SDK_INTERNAL_COMPLETION_H

#include <stdint.h>

#include "sdk_mailbox.h"
#include "scheduler.h"

struct sdk_internal_completion_hooks {
	void (*overlay_compose_retired)(int ok);
	void (*fw_update_retire)(uint16_t code);
	void (*fw_update_retire_failed)(void);
	void (*zzm_load_finished)(int ok);
	void (*media_close_retired)(uint32_t session);
	void (*pcm_ring_release)(uint32_t session);
	void (*ax_refill_done)(void);
};

/*
 * The session-bearing result payload (SDKMediaSessionMainResultPayload) leads
 * with the big-endian session id, so the dispatch reads it generically; a
 * payload shorter than the session word cannot identify a session and
 * retires nothing.
 */
static inline uint32_t sdk_internal_payload_session(const uint8_t *payload,
                                                    uint16_t payload_len)
{
	if (payload == 0 || payload_len < 4U)
		return 0U;
	return ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
	       ((uint32_t)payload[2] << 8) | payload[3];
}

static inline void sdk_internal_completion_dispatch(
	const struct sdk_internal_completion_hooks *hooks, uint16_t opcode,
	uint16_t status, const uint8_t *payload, uint16_t payload_len)
{
	if (opcode == (uint16_t)TASKQ_OP_VIDEO_COMPOSE) {
		hooks->overlay_compose_retired(status == SDK_STATUS_OK);
	} else if (opcode == (uint16_t)TASKQ_OP_FWUP) {
		if (status == SDK_STATUS_OK && payload != 0 && payload_len >= 2U)
			hooks->fw_update_retire(
			    (uint16_t)(((uint16_t)payload[0] << 8) | payload[1]));
		else
			hooks->fw_update_retire_failed();
	} else if (opcode == (uint16_t)TASKQ_OP_ZZM_LOAD) {
		hooks->zzm_load_finished(status == SDK_STATUS_OK);
	} else if (opcode == (uint16_t)SDK_OP_AUDIO_STREAM_CLOSE) {
		/* Background close: the audio-stream reap clears the slot. */
	} else if (opcode == (uint16_t)SDK_OP_IMAGE_SESSION_CLOSE) {
		/* Core-1 session teardown has no mailbox-side retirement. */
	} else if (opcode == (uint16_t)SDK_OP_MEDIA_SESSION_CLOSE) {
		if (status == SDK_STATUS_OK && payload_len >= 4U) {
			uint32_t session =
			    sdk_internal_payload_session(payload, payload_len);

			hooks->media_close_retired(session);
			hooks->pcm_ring_release(session);
		}
	} else {
		/* AX PCM refill: the only remaining internal producer. */
		hooks->ax_refill_done();
	}
}

#endif /* SDK_INTERNAL_COMPLETION_H */
