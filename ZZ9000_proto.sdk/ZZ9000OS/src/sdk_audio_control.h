/*
 * Firmware-authoritative audio control-plane dispatch (plan U4):
 * opcode handlers for SDK_OP_AUDIO_SCENE_SELECT ..
 * SDK_OP_AUDIO_CONTROL_STATE_GET (0x0509..0x050e), shared by the
 * core-0 mailbox dispatch (sdk_mailbox.c handle_request) and the host
 * test suite (test/audio/audio_control_test.c).
 *
 * This layer owns the wire protocol only -- big-endian payload
 * unpack/pack per the 48-byte inline convention -- and delegates all
 * state and DSP sequencing to the scene module (audio_scene.c),
 * including the single KTD7 fade-commit-restore path.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_AUDIO_CONTROL_H
#define SDK_AUDIO_CONTROL_H

#include <stdint.h>

/*
 * Run one control-plane audio opcode. params points at the request's
 * inline payload bytes (big-endian, exactly as the mailbox entry
 * carries them and the SDK client packed them); payload_len must
 * cover the opcode's full payload struct. result_payload must have
 * room for the full 48-byte inline payload; on SDK_STATUS_OK it holds
 * *result_len reply bytes (the corresponding SDK result struct,
 * big-endian, zero elsewhere). Unknown opcodes complete
 * SDK_STATUS_UNSUPPORTED, matching the mailbox default case.
 *
 * Runs inline on the calling core (SHORT task on core 0, KTD1);
 * never defers to core 1, so these opcodes do not reserve
 * request_id 0.
 */
uint16_t sdk_audio_control_run(uint16_t opcode, const uint8_t *params,
	uint16_t payload_len, uint8_t *result_payload,
	uint16_t *result_len);

#endif /* SDK_AUDIO_CONTROL_H */
