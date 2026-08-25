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
	if (sdk_get_be32(p->flags) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	scene = sdk_get_be32(p->scene);
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
	scene = sdk_get_be32(p->scene);
	param = sdk_get_be32(p->param);
	flags = sdk_get_be32(p->flags);
	if ((flags & ~SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	/* The stage call takes a slot index, so the full 32-bit scene
	 * must be range-checked here for every scene-scoped param: a
	 * truncating value (256 wraps to slot 0) would silently stage
	 * into the wrong draft. BASELINE ignores the scene entirely. */
	if (param != SDK_AUDIO_SCENE_PARAM_BASELINE &&
			scene >= SDK_AUDIO_SCENE_COUNT)
		return SDK_STATUS_BAD_REQUEST;
	if ((flags & SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT) != 0U &&
			scene >= SDK_AUDIO_SCENE_COUNT)
		return SDK_STATUS_BAD_REQUEST; /* commit needs a slot */

	rc = audio_scene_stage_param((uint8_t)scene, param,
		sdk_get_be32(p->value));
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
 * AP_DSP_SET_VOLUMES packing) -- except the reserved neutral word
 * 0x7f7f, which means "no trim from this owner": it releases the SDK
 * trim slot, then the reply reports the operator baseline pair as
 * applied, unbounded (an absolute 127/127 request is therefore not
 * expressible); with no trim held the release is a write-free no-op.
 * Any other word is converted to legs-relative deltas against the
 * baseline. The payload carries no owner identity -- the
 * single-owner model holds until I1 -- so every mailbox trim targets
 * the SDK slot and a driver releases by submitting the neutral
 * balance. */
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
	if (sdk_get_be32(p->flags) != 0U)
		return SDK_STATUS_BAD_REQUEST;

	balance = sdk_get_be32(p->balance);
	if (balance == SDK_AUDIO_BALANCE_NEUTRAL) {
		/* Keep-baseline release: the reserved word is this
		 * owner's documented release path, so a trim it
		 * previously submitted is dropped before the operator
		 * baseline is reported -- the reply then tells the
		 * truth about the applied legs. Release is idempotent
		 * at the scene layer: an owner that never trimmed (or
		 * already released) stays write-free and the baseline
		 * pair stands verbatim, unbounded. A failed release
		 * write (review 3855833169) reports IO error with no
		 * result payload: the scene layer kept the held trim,
		 * the DSP still applies it, and claiming the baseline
		 * here would lie about both. */
		if (audio_scene_trim_release(AUDIO_SCENE_OWNER_SDK) != 0)
			return SDK_STATUS_IO_ERROR;
		sdk_put_be32(out->balance_applied,
			SDK_AUDIO_BALANCE_PACK(audio_scene_baseline_paula(),
				audio_scene_baseline_ax()));
		sdk_put_be32(out->balance_bound, 0U);
		sdk_put_be32(out->flags, 0U);
		*result_len = (uint16_t)sizeof(*out);
		return SDK_STATUS_OK;
	}
	paula = (int16_t)((int32_t)SDK_AUDIO_BALANCE_CH1(balance) -
		(int32_t)audio_scene_baseline_paula());
	ax = (int16_t)((int32_t)SDK_AUDIO_BALANCE_CH2(balance) -
		(int32_t)audio_scene_baseline_ax());
	if (audio_scene_trim_submit(AUDIO_SCENE_OWNER_SDK, paula, ax,
			&result) != 0)
		return SDK_STATUS_IO_ERROR;

	sdk_put_be32(out->balance_applied,
		SDK_AUDIO_BALANCE_PACK(result.mixer_paula, result.mixer_ax));
	/* Bounded requests report the bound that was applied (R3): a
	 * bounded request is never silently clamped. */
	sdk_put_be32(out->balance_bound, result.bounded ?
		SDK_AUDIO_BALANCE_PACK(result.mixer_paula, result.mixer_ax)
		: 0U);
	sdk_put_be32(out->flags, result.bounded ?
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
	direction = sdk_get_be32(p->direction);
	flags = sdk_get_be32(p->flags);
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

/* Scene save (F5): the SD sequence never runs in dispatch. Validation
 * is immediate when no DSP commit is active, otherwise the save queues
 * behind that commit and reports its eventual outcome through the
 * control-state save_status word. */
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
	if (sdk_get_be32(p->flags) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	scene = sdk_get_be32(p->scene);
	if (scene >= SDK_AUDIO_SCENE_COUNT)
		return SDK_STATUS_BAD_REQUEST;

	status = audio_scene_save_start((uint8_t)scene);
	if (status < 0)
		return SDK_STATUS_BAD_REQUEST;
	sdk_put_be32(out->status, (uint32_t)status);
	sdk_put_be32(out->scene, scene);
	sdk_put_be32(out->flags, 0U);
	*result_len = (uint16_t)sizeof(*out);
	return SDK_STATUS_OK;
}

/* Control state get: active scene, baseline/applied pairs, calibrated
 * ceilings, derived AX-equivalent boundary and bounded-trim flag. */
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
	if (sdk_get_be32(p->flags) != 0U)
		return SDK_STATUS_BAD_REQUEST;

	audio_scene_control_state(&state);
	sdk_put_be32(out->active_scene, state.active_scene);
	sdk_put_be32(out->scene_count, state.scene_count);
	sdk_put_be32(out->baseline, SDK_AUDIO_BALANCE_PACK(
		state.baseline_paula, state.baseline_ax));
	sdk_put_be32(out->trim, SDK_AUDIO_BALANCE_PACK(state.trim_paula,
		state.trim_ax));
	sdk_put_be32(out->ceiling, state.ceiling);
	sdk_put_be32(out->flags, state.trim_bounded ?
		SDK_AUDIO_CONTROL_FLAG_TRIM_BOUNDED : 0U);
	sdk_put_be32(out->ceiling_paula, state.ceiling_paula);
	sdk_put_be32(out->ceiling_ax, state.ceiling_ax);
	sdk_put_be32(out->save_status, state.save_status);
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
