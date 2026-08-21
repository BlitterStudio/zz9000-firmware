/*
 * Firmware-authoritative audio control-plane dispatch (plan U4):
 * opcode handlers for SDK_OP_AUDIO_SCENE_SELECT ..
 * SDK_OP_AUDIO_CONTROL_STATE_GET (0x0509..0x050e).
 *
 * Protocol layer only: payloads are unpacked and packed big-endian
 * per the 48-byte inline mailbox convention (mirroring the SDK
 * ZZ9KAudio*Payload structs byte-for-byte); all state and DSP
 * sequencing -- staging accumulation, the KTD7 fade-commit-restore
 * path, serialization, gain staging, meter snapshots, save validation
 * -- belongs to the scene module (audio_scene.c). The core-0 mailbox
 * handler in sdk_mailbox.c routes these opcodes here (SHORT task,
 * KTD1); the host suite (test/audio/audio_control_test.c) drives the
 * same entry point.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include <stdint.h>

#include "audio_scene.h"
#include "sdk_audio_control.h"
#include "sdk_mailbox.h"

static uint32_t ctrl_get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void ctrl_put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* Scene select: switch the active scene through the single
 * glitch-free commit path (F3). */
static uint16_t ctrl_scene_select(const uint8_t *params,
	uint16_t payload_len)
{
	const struct SDKAudioSceneSelectPayload *p =
		(const struct SDKAudioSceneSelectPayload *)(const void *)
			params;
	uint32_t scene;
	int rc;

	if (payload_len < sizeof(*p))
		return SDK_STATUS_BAD_REQUEST;
	if (ctrl_get_be32(p->flags) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	scene = ctrl_get_be32(p->scene);
	if (scene >= SDK_AUDIO_SCENE_COUNT)
		return SDK_STATUS_BAD_REQUEST;

	rc = audio_scene_select((uint8_t)scene);
	if (rc == AUDIO_SCENE_COMMIT_BUSY)
		return SDK_STATUS_BUSY;
	return (rc == 0) ? SDK_STATUS_OK : SDK_STATUS_IO_ERROR;
}

/* Staged scene write: accumulate one parameter per call into the
 * firmware-side draft; the COMMIT flag asks for one atomic
 * glitch-free commit of everything staged (plus a staged baseline). */
static uint16_t ctrl_scene_write(const uint8_t *params,
	uint16_t payload_len)
{
	const struct SDKAudioSceneWritePayload *p =
		(const struct SDKAudioSceneWritePayload *)(const void *)
			params;
	uint32_t scene;
	uint32_t param;
	uint32_t flags;
	int rc;

	if (payload_len < sizeof(*p))
		return SDK_STATUS_BAD_REQUEST;
	scene = ctrl_get_be32(p->scene);
	param = ctrl_get_be32(p->param);
	flags = ctrl_get_be32(p->flags);
	if ((flags & ~SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if ((flags & SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT) != 0U &&
			scene >= SDK_AUDIO_SCENE_COUNT)
		return SDK_STATUS_BAD_REQUEST; /* commit needs a slot */

	rc = audio_scene_stage_param((uint8_t)scene, param,
		ctrl_get_be32(p->value));
	if (rc != 0)
		return SDK_STATUS_BAD_REQUEST;
	if ((flags & SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT) == 0U)
		return SDK_STATUS_OK;

	rc = audio_scene_commit_staged((uint8_t)scene);
	if (rc == AUDIO_SCENE_COMMIT_BUSY)
		return SDK_STATUS_BUSY;
	return (rc == 0) ? SDK_STATUS_OK : SDK_STATUS_IO_ERROR;
}

/* Owner source trim (R3). The ABI balance word is the requested
 * absolute mixer pair (two 0..255 volumes, 127 = 0 dB each,
 * AP_DSP_SET_VOLUMES packing); the arbiter composes trims on top of
 * the baseline, so the request is converted to legs-relative deltas.
 * The payload carries no owner identity -- the single-owner model
 * holds until I1 -- so every mailbox trim targets the SDK slot and a
 * driver releases by submitting the neutral balance. */
static uint16_t ctrl_trim_submit(const uint8_t *params,
	uint16_t payload_len, uint8_t *result_payload,
	uint16_t *result_len)
{
	const struct SDKAudioTrimSubmitPayload *p =
		(const struct SDKAudioTrimSubmitPayload *)(const void *)
			params;
	struct SDKAudioTrimResultPayload *out =
		(struct SDKAudioTrimResultPayload *)(void *)result_payload;
	struct audio_scene_trim_result result;
	uint32_t balance;
	int16_t paula;
	int16_t ax;

	if (payload_len < sizeof(*p))
		return SDK_STATUS_BAD_REQUEST;
	if (ctrl_get_be32(p->flags) != 0U)
		return SDK_STATUS_BAD_REQUEST;

	balance = ctrl_get_be32(p->balance);
	paula = (int16_t)((int32_t)SDK_AUDIO_BALANCE_CH1(balance) -
		(int32_t)audio_scene_baseline_paula());
	ax = (int16_t)((int32_t)SDK_AUDIO_BALANCE_CH2(balance) -
		(int32_t)audio_scene_baseline_ax());
	if (audio_scene_trim_submit(AUDIO_SCENE_OWNER_SDK, paula, ax,
			&result) != 0)
		return SDK_STATUS_IO_ERROR;

	ctrl_put_be32(out->balance_applied,
		SDK_AUDIO_BALANCE_PACK(result.mixer_paula, result.mixer_ax));
	/* Bounded requests report the bound that was applied (R3): a
	 * bounded request is never silently clamped. */
	ctrl_put_be32(out->balance_bound, result.bounded ?
		SDK_AUDIO_BALANCE_PACK(result.mixer_paula, result.mixer_ax)
		: 0U);
	ctrl_put_be32(out->flags, result.bounded ?
		SDK_AUDIO_TRIM_RESULT_BOUNDED : 0U);
	*result_len = (uint16_t)sizeof(*out);
	return SDK_STATUS_OK;
}

/* Meter read (R8/R9): one framed, non-tearing snapshot of one
 * direction; SDK_AUDIO_METER_RESULT_HOLD_RESET opts into
 * read-and-clear peak-hold semantics. */
static uint16_t ctrl_meter_read(const uint8_t *params,
	uint16_t payload_len, uint8_t *result_payload,
	uint16_t *result_len)
{
	const struct SDKAudioMeterReadPayload *p =
		(const struct SDKAudioMeterReadPayload *)(const void *)
			params;
	struct audio_meter_snapshot snapshot;
	uint32_t direction;
	uint32_t flags;

	if (payload_len < sizeof(*p))
		return SDK_STATUS_BAD_REQUEST;
	direction = ctrl_get_be32(p->direction);
	flags = ctrl_get_be32(p->flags);
	if ((flags & ~SDK_AUDIO_METER_RESULT_HOLD_RESET) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (audio_scene_meter_read(direction, flags, &snapshot) != 0)
		return SDK_STATUS_BAD_REQUEST; /* unknown direction */

	sdk_audio_meter_result_pack(
		(volatile struct SDKAudioMeterResultPayload *)
			(void *)result_payload,
		&snapshot, flags);
	*result_len = (uint16_t)sizeof(struct SDKAudioMeterResultPayload);
	return SDK_STATUS_OK;
}

/* Scene save (F5): validation here; the CFG writer is the U5 seam. */
static uint16_t ctrl_scene_save(const uint8_t *params,
	uint16_t payload_len, uint8_t *result_payload,
	uint16_t *result_len)
{
	const struct SDKAudioSceneSavePayload *p =
		(const struct SDKAudioSceneSavePayload *)(const void *)
			params;
	struct SDKAudioSceneSaveResultPayload *out =
		(struct SDKAudioSceneSaveResultPayload *)(void *)
			result_payload;
	uint32_t scene;
	int status;

	if (payload_len < sizeof(*p))
		return SDK_STATUS_BAD_REQUEST;
	if (ctrl_get_be32(p->flags) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	scene = ctrl_get_be32(p->scene);
	if (scene >= SDK_AUDIO_SCENE_COUNT)
		return SDK_STATUS_BAD_REQUEST;

	status = audio_scene_save((uint8_t)scene);
	if (status < 0)
		return SDK_STATUS_BAD_REQUEST;
	ctrl_put_be32(out->status, (uint32_t)status);
	ctrl_put_be32(out->scene, scene);
	ctrl_put_be32(out->flags, 0U);
	*result_len = (uint16_t)sizeof(*out);
	return SDK_STATUS_OK;
}

/* Control state get: active scene, scene count, baseline and applied
 * trim pairs, the enforced ceiling, and the bounded-trim flag. */
static uint16_t ctrl_state_get(const uint8_t *params,
	uint16_t payload_len, uint8_t *result_payload,
	uint16_t *result_len)
{
	const struct SDKAudioControlStateGetPayload *p =
		(const struct SDKAudioControlStateGetPayload *)
			(const void *)params;
	struct SDKAudioControlStateResultPayload *out =
		(struct SDKAudioControlStateResultPayload *)(void *)
			result_payload;
	struct audio_scene_control_state state;

	if (payload_len < sizeof(*p))
		return SDK_STATUS_BAD_REQUEST;
	if (ctrl_get_be32(p->flags) != 0U)
		return SDK_STATUS_BAD_REQUEST;

	audio_scene_control_state(&state);
	ctrl_put_be32(out->active_scene, state.active_scene);
	ctrl_put_be32(out->scene_count, state.scene_count);
	ctrl_put_be32(out->baseline, SDK_AUDIO_BALANCE_PACK(
		state.baseline_paula, state.baseline_ax));
	ctrl_put_be32(out->trim, SDK_AUDIO_BALANCE_PACK(state.trim_paula,
		state.trim_ax));
	ctrl_put_be32(out->ceiling, state.ceiling);
	ctrl_put_be32(out->flags, state.trim_bounded ?
		SDK_AUDIO_CONTROL_FLAG_TRIM_BOUNDED : 0U);
	*result_len = (uint16_t)sizeof(*out);
	return SDK_STATUS_OK;
}

uint16_t sdk_audio_control_run(uint16_t opcode, const uint8_t *params,
	uint16_t payload_len, uint8_t *result_payload,
	uint16_t *result_len)
{
	if (result_len != NULL)
		*result_len = 0;
	if (params == NULL || result_payload == NULL || result_len == NULL)
		return SDK_STATUS_BAD_REQUEST;

	switch (opcode) {
	case SDK_OP_AUDIO_SCENE_SELECT:
		return ctrl_scene_select(params, payload_len);
	case SDK_OP_AUDIO_SCENE_WRITE:
		return ctrl_scene_write(params, payload_len);
	case SDK_OP_AUDIO_TRIM_SUBMIT:
		return ctrl_trim_submit(params, payload_len, result_payload,
			result_len);
	case SDK_OP_AUDIO_METER_READ:
		return ctrl_meter_read(params, payload_len, result_payload,
			result_len);
	case SDK_OP_AUDIO_SCENE_SAVE:
		return ctrl_scene_save(params, payload_len, result_payload,
			result_len);
	case SDK_OP_AUDIO_CONTROL_STATE_GET:
		return ctrl_state_get(params, payload_len, result_payload,
			result_len);
	default:
		/* Unknown opcodes complete UNSUPPORTED exactly as the
		 * mailbox default case does (KTD1, R16 fallback). */
		return SDK_STATUS_UNSUPPORTED;
	}
}
