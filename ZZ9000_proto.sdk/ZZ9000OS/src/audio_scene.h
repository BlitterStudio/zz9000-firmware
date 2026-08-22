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

/* Fixed scene slots; the name is a user label carried alongside the
 * master-chain assignment (it never reaches the DSP). */
#define AUDIO_SCENE_COUNT 8
#define AUDIO_SCENE_EQ_BANDS 10
#define AUDIO_SCENE_NAME_MAX 16
#define AUDIO_SCENE_NAME_CHUNKS (AUDIO_SCENE_NAME_MAX / 2)

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
 * 100 = right, LPF cutoff in Hz (1 .. 23900). name is a printable
 * ASCII label (NUL-terminated, at most AUDIO_SCENE_NAME_MAX chars)
 * that never joins the DSP write set.
 */
struct audio_scene_def {
	uint16_t lpf_hz;
	uint8_t eq[AUDIO_SCENE_EQ_BANDS];
	uint8_t prefactor;
	uint8_t volume;
	uint8_t pan;
	char name[AUDIO_SCENE_NAME_MAX + 1];
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
 * staged mixer legs before any owner can be serviced (R10). The
 * reset path has no client waiting, so the commit machine runs here
 * synchronously to completion. Returns 0 on success, -1 if a
 * verified DSP write failed or the module is not initialized. */
int audio_scene_apply_after_dsp_init(void);

/*
 * Compatibility status from the synchronous-commit era (KTD7
 * serialization): entry points re-entered mid-commit used to fail
 * fast with this. Since the commit became an incremental machine
 * driven by audio_scene_poll(), a request that arrives mid-commit is
 * coalesced instead and the entry points return 0; the define stays
 * for ABI stability.
 */
#define AUDIO_SCENE_COMMIT_BUSY (-2)

/*
 * Advance each active incremental machine by at most one hardware unit:
 * one verified DSP/I2C substep and one FatFs call. Returns nonzero while
 * either is active (more steps remain, or a coalesced follow-up machine
 * just started), 0 when both are idle. Cheap when idle. Call once per
 * main-loop iteration so scene commits and CFG saves interleave with
 * per-period AHI traffic and Zorro register service.
 */
int audio_scene_poll(void);

/* Scene operations (the only accepted path to master-chain writes).
 * Those that change the applied master chain join the single
 * glitch-free commit path. Validation is synchronous; the commit
 * itself starts the machine and returns 0 immediately -- a request
 * arriving while a machine is mid-flight is coalesced (applied by a
 * fresh machine at its completion), never rejected as busy. Returns
 * -1 only for an invalid request. */
int audio_scene_select(uint8_t index);
int audio_scene_write(uint8_t index, const struct audio_scene_def *def);
const struct audio_scene_def *audio_scene_get(uint8_t index);
uint8_t audio_scene_active_index(void);

/*
 * Set the operator baseline (R17): applied under every owner's
 * playback, on top of the active scene. The immediate variant joins
 * the single glitch-free commit path (started, not completed --
 * audio_scene_poll drives it); the mailbox path stages it
 * through audio_scene_stage_param and commits.
 */
int audio_scene_set_baseline(uint8_t paula, uint8_t ax);
uint8_t audio_scene_baseline_paula(void);
uint8_t audio_scene_baseline_ax(void);

/* Source-trim lifecycle (R3, R4): submit composes with the baseline
 * under the enforced boundary; release resets that owner's trim to
 * neutral. submit returns 0 and fills *result (when non-NULL); owner
 * must be one of AUDIO_SCENE_OWNER_AHI/MHI/SDK. */
int audio_scene_trim_submit(uint8_t owner, int16_t paula, int16_t ax,
	struct audio_scene_trim_result *result);
void audio_scene_trim_release(uint8_t owner);

/* ---- staged scene edits and the single commit path (U4, KTD7) ---- */

/*
 * Stage one parameter of one scene into a firmware-side draft
 * (KTD1: no client draft buffer -- SCENE_WRITE accumulates here and
 * nothing touches the DSP until the matching commit). The draft is
 * seeded from the scene's current definition, so partial edits
 * compose. param is one of the SDK_AUDIO_SCENE_PARAM_* ids (the ABI
 * vocabulary); value range checks match the scene definition. The
 * BASELINE param stages the operator balance instead (R17) and
 * ignores the scene index. The NAME param stages one two-char chunk
 * of the scene label (SDK_AUDIO_SCENE_PARAM_NAME grammar in
 * sdk_mailbox.h): a rename stages the complete name, then commits.
 * Returns 0 on success, -1 on an invalid
 * request (unknown param id, out-of-range value or scene slot).
 */
int audio_scene_stage_param(uint8_t index, uint32_t param,
	uint32_t value);

/*
 * Commit everything staged for this scene (plus a staged baseline)
 * as one atomic glitch-free master-chain assignment: fade down
 * through verified writes, commit params in fixed order (the EQ
 * bands as one contiguous safeload group), restore volume (KTD7).
 * One machine serves scene select, live edit commit, and baseline
 * changes. A commit of an inactive scene stores the staged
 * definition without touching the DSP. Returns 0 once the staged
 * edits are taken into the live tables and the machine is started
 * (or, for a mid-flight machine, the re-apply is coalesced); the
 * staging is consumed only when the applying machine ultimately
 * succeeds -- a failing machine restores the pre-commit tables so a
 * retry re-issues the whole sequence.
 */
int audio_scene_commit_staged(uint8_t index);

/* ---- control-plane state report (SDK_OP_AUDIO_CONTROL_STATE_GET) ---- */

struct audio_scene_control_state {
	uint8_t active_scene;
	uint8_t scene_count;
	uint8_t baseline_paula; /* operator baseline legs (R17) */
	uint8_t baseline_ax;
	uint8_t trim_paula;     /* last applied composed mixer legs */
	uint8_t trim_ax;
	uint8_t trim_bounded;   /* last composition was reduced (R3) */
	uint32_t ceiling;       /* enforced boundary, mixer-value units */
	uint32_t save_status;   /* AUDIO_SCENE_SAVE_QUEUED while a save
	                         * runs, else the last settled outcome */
};

void audio_scene_control_state(struct audio_scene_control_state *out);

/* ---- scene save (F5; the writer itself is U5, KTD5) ---- */

/* Status words; mirror SDK_AUDIO_SCENE_SAVE_* one-to-one. */
#define AUDIO_SCENE_SAVE_OK       0
#define AUDIO_SCENE_SAVE_REJECTED 1 /* failed boundary validation */
#define AUDIO_SCENE_SAVE_IO_ERROR 2 /* temp-then-replace failed */
#define AUDIO_SCENE_SAVE_QUEUED   3 /* started; the machine is running */
#define AUDIO_SCENE_SAVE_BUSY     4 /* refused: a save is already running */

/*
 * Start the non-blocking save. With an idle DSP commit machine,
 * validation and serialization run immediately; with a live commit,
 * the save queues and takes its snapshot only after that commit
 * settles, so the mailbox request never drains I2C synchronously.
 * The temp-then-replace sequence runs as one FatFs call per
 * audio_scene_poll() step; the SDPS layer independently deadlines
 * each hardware poll. Returns QUEUED on start, BUSY while a previous
 * save runs, an immediate REJECTED / IO_ERROR when preparation can run
 * synchronously, or -1 for an invalid request. A deferred preparation
 * reports REJECTED / IO_ERROR through audio_scene_save_status().
 */
int audio_scene_save_start(uint8_t index);

/*
 * AUDIO_SCENE_SAVE_QUEUED while the machine is running, otherwise the
 * outcome of the most recent save (OK after boot -- nothing has
 * failed). This is what the control-state report carries.
 */
int audio_scene_save_status(void);


/*
 * U5 (KTD4/KTD5): fold the parsed ZZ9000.CFG audio keys into scene
 * state (R10) -- absent keys keep the built-in defaults, a scene that
 * no longer validates degrades to its default, and the active scene
 * and operator baseline are restored. Call after audio_scene_init()
 * and before the first apply (boot reads the file before the scene
 * module exists, so state connects here, ahead of the shared
 * boot/warm-reset apply).
 */
void audio_scene_load_config(void);


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

/* ---- metering accumulation and coherent snapshots (U3, KTD3) ---- */

/*
 * One coherent per-direction meter reading (R8/R9). Peaks are unsigned
 * 16.16 (0x00010000 = digital full scale); counters saturate, never
 * wrap. generation comes from the firmware-side seqlock: all frames of
 * one logical read carry the same generation.
 */
struct audio_meter_snapshot {
	uint32_t direction;
	uint32_t generation;
	uint32_t identity;
	uint32_t clip_count;
	uint32_t underrun_count;
	uint32_t overrun_count;
	uint32_t gain_reduction_events;
	uint32_t peak_hold_ch1;
	uint32_t peak_hold_ch2;
};

/*
 * Bounded per-period peak/clip scans, called from the audio formatter
 * ISRs (single-writer context per direction; R9 reads never block
 * these). frame_count is exactly the completed period's frame count
 * (AUDIO_BYTES_PER_PERIOD / 4): the scan is bounded by construction,
 * never a per-sample conversion. Clips are at-rail runs: the counter
 * increments once per saturating region, not per sample, which also
 * surfaces the qualified converter's zz_audio_convert_clips events
 * (saturating writes are exactly the at-rail samples in the scanned
 * bytes) for every reachable instance -- pump, legacy playback, and
 * capture -- without double counting. Capture periods are the
 * published S16BE bytes (what zz_audio_capture_convert wrote back).
 */
void audio_scene_meter_output_period(const int16_t *frames,
	uint32_t frame_count);
void audio_scene_meter_capture_period(const uint8_t *period_be,
	uint32_t frame_count);

/* Event counters (saturating): output underruns feed from both the
 * session pump path (sdk_mailbox audio_pump_source_underrun) and the
 * register-fed producer-stagnation detector below; capture overruns
 * feed from the RX ISR's full-ring-wrap case. */
void audio_scene_meter_output_underrun(void);
void audio_scene_meter_capture_overrun(void);

/*
 * Register-fed (legacy AHI) playback underrun detection: the producer
 * advances a sequence once per refilled period (audio_swab); arm() is
 * called when playback is enabled, tick() from every TX DMA-complete
 * path with the current producer sequence. A completed period whose
 * producer sequence is unchanged since the previous completed period
 * played unfilled audio: one underrun. Startup pre-fills never count
 * (the producer must advance once after arming before stagnation
 * counts).
 */
void audio_scene_meter_output_producer_arm(uint32_t producer_seq);
void audio_scene_meter_output_producer_tick(uint32_t producer_seq);

/*
 * Active output source identity (R8), written by the control-plane
 * session owner (sdk_mailbox pump bind/unbind): one of
 * SDK_AUDIO_METER_IDENTITY_*. Register-fed legacy playback stays
 * METER_IDENTITY_UNKNOWN; a participating AHI owner (one holding a
 * submitted trim) is named while no session owns the output.
 */
void audio_scene_meter_output_identity(uint32_t identity);

/*
 * Coherent snapshot read (R9): retries the copy while the direction's
 * single ISR writer updates underneath it, so a returned snapshot
 * never tears. flags follows the ABI request: passing
 * SDK_AUDIO_METER_RESULT_HOLD_RESET consumes the peak-hold window
 * (the next completed period opens a fresh hold -- R8
 * peak-hold-since-last-read); without it the read peeks. Returns 0 on
 * success, -1 for an unknown direction or NULL snapshot. The read
 * never mutates audio-path state.
 */
int audio_scene_meter_read(uint32_t direction, uint32_t flags,
	struct audio_meter_snapshot *snapshot);

#endif /* AUDIO_SCENE_H */
