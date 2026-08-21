/*
 * Firmware-authoritative audio control plane: scene state, split
 * authority, gain staging against the enforced saturation boundary,
 * and the legacy register-path authority gate (plan U2, KTD2).
 *
 * This module is the single arbiter of the master DSP chain. ax.c
 * stays a stateless transport: every master-chain write reaches the
 * ADAU1701 through a call from this module. Host tests link
 * audio_scene.c without ax.c and provide their own definitions of the
 * ax.h setters (link-time seam), recording writes for order and
 * completeness assertions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIO_SCENE_H
#define AUDIO_SCENE_H

#include <stdint.h>

/*
 * Enforced DAC saturation boundary, in summed-mixer units (each mixer
 * leg 0..255 where 127 = 0 dB, matching audio_adau_set_mixer_vol).
 *
 * This is the SINGLE named definition of the enforced boundary; U8's
 * bench measurement (R6) replaces this value in one place and the
 * staging host tests re-run against it. Provisional derivation: both
 * drivers document that summed mixer values above ~0x100 (256)
 * saturate the DAC (zz9000ax-ahi.c, mhizz9000.c); the conservative
 * figure scaled by 3/4 gives ~2.5 dB of stated headroom below it.
 */
#define AUDIO_SCENE_ENFORCED_BOUNDARY 192.0

/* Fixed scene slots; labels are the slot numbers themselves. */
#define AUDIO_SCENE_COUNT 8
#define AUDIO_SCENE_EQ_BANDS 10

/* Stream-owner identities for the per-owner source trim (R3). */
enum {
	AUDIO_SCENE_OWNER_NONE = 0,
	AUDIO_SCENE_OWNER_AHI,
	AUDIO_SCENE_OWNER_MHI,
	AUDIO_SCENE_OWNER_SDK,
	AUDIO_SCENE_OWNER_SLOTS
};

/*
 * A complete master-chain assignment (R1). Field ranges match the
 * ax.h setters: EQ/prefactor 0 = -12 dB .. 50 = 0 dB .. 100 = +12 dB,
 * volume 0 = muted .. 100 = 0 dB, pan 0 = left .. 50 = center ..
 * 100 = right, LPF cutoff in Hz (1 .. 23900).
 */
struct audio_scene_def {
	uint16_t lpf_hz;
	uint8_t eq[AUDIO_SCENE_EQ_BANDS];
	uint8_t prefactor;
	uint8_t volume;
	uint8_t pan;
};

/*
 * Result of a trim submission, reported back to the requester so a
 * bounded request is never silently clamped (R3).
 */
struct audio_scene_trim_result {
	uint8_t bounded;    /* nonzero when the request was reduced */
	uint8_t mixer_paula; /* applied mixer legs (absolute values) */
	uint8_t mixer_ax;
	double trim_bound;  /* remaining summed-trim headroom for the
	                     * current scene and baseline */
};

/* One gain-reduction telemetry event (R7). */
struct audio_scene_gain_event {
	double requested; /* composed level that exceeded the boundary */
	double applied;   /* composed level after the reduction */
	double boundary;  /* enforced boundary in force */
};

/* Boot: install default scenes, neutral baseline and trims, and
 * claim master-chain authority. Idempotent; call once before the
 * Zorro request loop starts. */
void audio_scene_init(void);

/* Re-apply the active scene after audio_adau_init() has rewritten the
 * DSP with its defaults (cold boot via main -> handle_amiga_reset and
 * every Amiga warm reset). Resets telemetry counters (they describe a
 * DSP instance that no longer exists) and drops all owner trims (the
 * reset tore their sessions down), then writes the scene and the
 * staged mixer legs before any owner can be serviced (R10).
 * Returns 0 on success, -1 if a verified DSP write failed. */
int audio_scene_apply_after_dsp_init(void);

/* Scene operations (the only accepted path to master-chain writes). */
int audio_scene_select(uint8_t index);
int audio_scene_write(uint8_t index, const struct audio_scene_def *def);
const struct audio_scene_def *audio_scene_get(uint8_t index);
uint8_t audio_scene_active_index(void);

/* Operator baseline Paula/AX balance (R17): applied under every
 * owner's playback, on top of the active scene. Re-stages the mixer. */
void audio_scene_set_baseline(uint8_t paula, uint8_t ax);
uint8_t audio_scene_baseline_paula(void);
uint8_t audio_scene_baseline_ax(void);

/* Source-trim lifecycle (R3, R4): submit composes with the baseline
 * under the enforced boundary; release resets that owner's trim to
 * neutral. submit returns 0 and fills *result (when non-NULL); owner
 * must be one of AUDIO_SCENE_OWNER_AHI/MHI/SDK. */
int audio_scene_trim_submit(uint8_t owner, int16_t paula, int16_t ax,
	struct audio_scene_trim_result *result);
void audio_scene_trim_release(uint8_t owner);

/* Authority gate for the legacy register path (R2): nonzero when a
 * REG_ZZ_AUDIO_VAL write to this AP_* parameter index must be
 * rejected. Master-chain indices (AP_DSP_SET_LOWPASS ..
 * AP_DSP_SET_STEREO_VOLUME) are always rejected -- a register write is
 * never "issued by the scene module". AP_DSP_UPLOAD is rejected while
 * the control plane holds authority; buffer-offset parameters stay on
 * the legacy path. */
int audio_scene_register_write_blocked(uint32_t audio_param);
int audio_scene_authority_active(void);

/* Gain-reduction telemetry: count since the last DSP (re)init plus
 * the most recent event (NULL until the first one). */
uint32_t audio_scene_gain_reduction_events(void);
const struct audio_scene_gain_event *audio_scene_last_gain_reduction(void);

#endif /* AUDIO_SCENE_H */
