/*
 * ZZ9000AX audio fabric compositor: the single firmware writer of the
 * formatter TX ring (plan U2, R1).
 *
 * The compositor absorbs the playback pump's per-period fill loop
 * (frontier scheduling, retirement, cache discipline, silence policy)
 * that used to live in sdk_mailbox.c's core-0 TX ISR. Decode-side
 * machinery -- feeds, backpressure kicks, drain accounting -- stays
 * with the producer owners. Every SDK audio playback path binds here
 * as a producer in a bounded slot table; legacy/AHI register playback
 * keeps its exclusive window through a three-way ownership state:
 *
 *   IDLE               nobody owns the output,
 *   LEGACY_EXCLUSIVE   a register-driven (AHI) client owns the output,
 *   FABRIC_ACTIVE      the compositor owns the output; legacy entry is
 *                      rejected and the formatter is never repointed.
 *
 * Producer #1 is the SDK pump slot (audio streams and media sessions,
 * routed implicitly from their first bind); slots 2 and 3 are the
 * lease plane (opcodes 0x050f+, implemented in U3: slot 1 leases
 * from the mailbox, slot 2 stays firmware-reserved). The
 * per-slot mix discipline is saturating int32 accumulate then one S16
 * clamp per frame (KTD4); a late or absent producer contributes
 * silence without stalling the others and only its own underrun
 * counter moves (R8).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIO_FABRIC_H
#define AUDIO_FABRIC_H

#include <stdint.h>

#include "audio_convert.h"

enum audio_fabric_ownership {
	AUDIO_FABRIC_IDLE = 0,
	AUDIO_FABRIC_LEGACY_EXCLUSIVE = 1,
	AUDIO_FABRIC_ACTIVE = 2,
};

/* Bounded slot table (R4). Only the pump slot is live in U2. */
#define AUDIO_FABRIC_SLOT_PUMP     0U
#define AUDIO_FABRIC_SLOT_MAILBOX  1U
#define AUDIO_FABRIC_SLOT_RESERVED 2U
#define AUDIO_FABRIC_SLOT_COUNT    3U

/* Per-slot balance scale (SDK_AUDIO_BALANCE_* vocabulary, 0..255).
 * Every slot is unity until the lease plane programs real gains. */
#define AUDIO_FABRIC_GAIN_UNITY 128U

/*
 * Producer ring view (KTD2): one coherent snapshot of the producer's
 * published cursors. The producer is the single writer of its source
 * ring and of produced/staged; the compositor is the sole consumer-
 * side writer (it advances staging through the stage callback below).
 */
struct audio_fabric_source {
	uint8_t *ring;
	uint32_t capacity;
	uint64_t produced_bytes;
	uint64_t staged_bytes;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t sample_format;
	uint8_t done;
	uint8_t faulted;
};

/*
 * Producer callbacks. All of them may run in ISR context (the fill
 * compositor runs from the audio-formatter period interrupt); they
 * must be integer-only, bounded, and must never touch the TX ring,
 * the formatter, the scheduler or the console.
 */
struct audio_fabric_producer_ops {
	/* Publish the current source snapshot. Return 0 when the source
	 * has vanished (the compositor drops the slot and calls gone). */
	int (*snapshot)(struct audio_fabric_source *source);
	/* Advance the producer's staging cursor by bytes; return 0 when
	 * the bytes are no longer available (slot contributes silence). */
	int (*stage)(uint32_t bytes);
	/* Acknowledge source bytes whose periods the DMA retired. */
	void (*retire)(uint32_t bytes);
	/* A period the DMA actually reached was unfilled: a played
	 * underrun for a source that is neither done nor faulted. */
	void (*underrun)(void);
	/* The source vanished mid-composition: drop the session binding.
	 * Runs from the ISR; main-loop state must be plain stores. */
	void (*gone)(void);
	/* Stream play-out tail: real PCM staged this ISR, and (after a
	 * full ring of committed silence) the tail has played out. */
	void (*tail_real)(void);
	void (*tail_drained)(void);
};

/*
 * TX-fill compositor tick: call from the audio formatter period
 * interrupt (isr_audio) every 20 ms. No-op unless the fabric is
 * active with at least one live producer.
 */
void audio_fabric_isr(void);

/*
 * Producer lifecycle (main-loop context). attach registers the ops and
 * leaves the slot FROZEN so the caller can rebuild cursors (restart)
 * before publishing with go_live; entering from IDLE claims the output
 * for the fabric (audio_set_tx_buffer back to the standard TX ring plus
 * a conditional audio_init_i2s when a legacy session repointed the
 * DMA -- review decision 2). Re-attaching an attached slot just
 * freezes it (producer restart inside fabric-active: no formatter
 * transactions). Returns 0 for a bad slot/ops or a legacy-exclusive
 * output. detach drops the slot; when the LAST producer releases, the
 * ring is silenced (KTD8) and ownership returns to IDLE.
 */
int audio_fabric_producer_attach(uint32_t slot,
	const struct audio_fabric_producer_ops *ops);
void audio_fabric_producer_detach(uint32_t slot);
void audio_fabric_producer_freeze(uint32_t slot);
void audio_fabric_producer_go_live(uint32_t slot);
/* Re-arm the fill frontier at the current DMA position and drop the
 * slot's pending retirement tags, conversion state and silence run
 * (bind and media resume; no formatter reprogramming). */
void audio_fabric_producer_restart(uint32_t slot);
/* Media pause-confirm: drop the slot's pending retirement tags and
 * wipe the whole TX ring (the historical pause semantics). */
void audio_fabric_ring_silence(uint32_t slot);
/* Mailbox teardown: drop every slot, no formatter transactions. */
void audio_fabric_reset(void);

/*
 * The widened ownership gate for legacy register entry points
 * (REG_ZZ_AUDIO_CONFIG play bit, REG_ZZ_AUDIO_SWAB,
 * AP_TX_BUF_OFFS): nonzero exactly while the fabric owns the output,
 * so legacy/AHI paths reject instead of retargeting live fabric
 * audio. The three-way ownership view derives LEGACY_EXCLUSIVE from
 * the register-fed playback state.
 */
int audio_fabric_output_busy(void);
enum audio_fabric_ownership audio_fabric_ownership(void);

/* Saturating per-slot underrun counter and the slot's attach epoch
 * (STATE_GET generation vocabulary; the epoch survives a detach). */
uint32_t audio_fabric_slot_underruns(uint32_t slot);
uint32_t audio_fabric_slot_epoch(uint32_t slot);

/*
 * Lease plane (plan U3): generation-tagged producer leases over the
 * reserved opcodes 0x050f..0x0512. The mailbox handlers are a thin
 * protocol bridge; everything below runs in main-loop context on
 * core 0 (SHORT dispatch).
 */

/* Per-slot status (SDK_AUDIO_FABRIC_SLOT_* vocabulary). The pump slot
 * is FREE/LEASED/ACTIVE by playback bind state (never leased); a
 * lease is LEASED from BEGIN until its first accepted SUBMIT and
 * ACTIVE while the compositor consumes its ring. */
#define AUDIO_FABRIC_SLOT_STATE_FREE    0U
#define AUDIO_FABRIC_SLOT_STATE_LEASED  1U
#define AUDIO_FABRIC_SLOT_STATE_ACTIVE  2U

/* Lease admission results; the mailbox layer maps these onto the
 * SDK_STATUS_* vocabulary. */
#define AUDIO_FABRIC_LEASE_OK        0
#define AUDIO_FABRIC_LEASE_EBAD_SLOT (-1)  /* slot 0 / reserved 2 / range */
#define AUDIO_FABRIC_LEASE_EBUSY     (-2)  /* slot already leased */
#define AUDIO_FABRIC_LEASE_ELEGACY   (-3)  /* legacy-exclusive output */
#define AUDIO_FABRIC_LEASE_EHANDLE   (-4)  /* stale/unknown handle */

/* Opaque generation-tagged lease handle: low 4 bits carry the slot,
 * the upper 28 bits the slot's lease epoch, so a handle is never 0
 * and never collides across lease generations of the same slot. */
#define AUDIO_FABRIC_LEASE_HANDLE(slot, epoch) \
	((((uint32_t)(epoch)) << 4) | ((uint32_t)(slot) & 0x0FU))
#define AUDIO_FABRIC_LEASE_HANDLE_SLOT(handle) ((handle) & 0x0FU)
#define AUDIO_FABRIC_LEASE_HANDLE_EPOCH(handle) ((handle) >> 4)

/* One framed per-slot state snapshot (SDK_OP_AUDIO_FABRIC_STATE_GET).
 * Peak follows the scene-meter convention: unsigned 16.16 of the
 * loudest source sample read since the hold window opened, consumed
 * by a HOLD_RESET read; clips counts at-rail source regions and is
 * monotonic within a lease. written/consumed are the monotonic byte
 * cursors of the slot's source ring (truncated to 32 bits by the ABI
 * payload words). */
struct audio_fabric_slot_state {
	uint32_t state;            /* AUDIO_FABRIC_SLOT_STATE_* */
	uint32_t identity;         /* SDK_AUDIO_METER_IDENTITY_* */
	uint32_t lease;            /* handle, or 0xffffffffU while free */
	uint32_t generation;       /* lease epoch of (the last) lease */
	uint64_t written_bytes;    /* published by SUBMIT */
	uint64_t consumed_bytes;   /* staged by the compositor */
	uint32_t underruns;        /* saturating, this slot only */
	uint32_t peak;             /* 16.16, hold-window semantics */
	uint32_t clips;            /* at-rail regions, saturating */
};

/*
 * Applied-vs-requested gain report (R11): LEASE_BEGIN composes the
 * requested gain through the scene arbiter's ceiling-bounded
 * composition and reports what actually applied, so a bounded
 * request is never silently clamped (the I3 trim-bound pattern).
 */
struct audio_fabric_lease_grant {
	uint8_t bounded; /* nonzero when the requested gain was reduced */
	uint8_t gain;    /* applied 0..255 mixer scale */
};

/*
 * Grant a producer lease. slot 0 (the pump) is never leaseable and
 * slot 2 stays firmware-reserved for the AHI/synthesis follow-on;
 * both complete AUDIO_FABRIC_LEASE_EBAD_SLOT (the mailbox layer
 * answers SDK_STATUS_BAD_REQUEST -- an admission-policy rejection,
 * not exhaustion). A leased slot completes AUDIO_FABRIC_LEASE_EBUSY
 * (mailbox: SDK_STATUS_BUSY), as does a legacy-exclusive output.
 * gain is the single requested 0..255 mixer scale
 * (AUDIO_FABRIC_GAIN_UNITY is unity); the scene arbiter composes it
 * against the enforced ceiling under the active scene (R11) and the
 * applied value -- possibly bounded, never silently -- is reported
 * through *grant when non-NULL. On success the ring is zeroed (the
 * ghost-period bound of R5 depends on this) and *lease carries the
 * generation-tagged handle.
 */
int audio_fabric_lease_begin(uint32_t slot, uint32_t identity,
	uint32_t gain, uint32_t *lease,
	struct audio_fabric_lease_grant *grant);

/*
 * Copy length bytes from the client's resolved staging address into
 * the slot's source ring at the write cursor (wrap-safe), flush, and
 * publish. Backpressure matches the lease result contract: a partial
 * accept returns the space actually taken in *bytes_consumed and the
 * producer resubmits the remainder; zero accepted space with a
 * nonzero request returns AUDIO_FABRIC_LEASE_OK with
 * *bytes_consumed == 0 (the mailbox layer answers SDK_STATUS_BUSY).
 * A stale handle (epoch mismatch, released or reset lease) returns
 * AUDIO_FABRIC_LEASE_EHANDLE and writes nothing. src must reference
 * S16LE stereo frames at 48 kHz (4-byte aligned lengths); the
 * byte-order/rate-intent lease flags are required zero and rejected
 * by the mailbox layer.
 */
int audio_fabric_lease_submit(uint32_t handle, const uint8_t *src,
	uint32_t length, uint32_t *bytes_consumed);

/*
 * Surrender a lease, quiesce-then-bump (KTD3): the slot is marked
 * tearing-down so the compositor zero-contributes after at most one
 * residual period (the ghost bound), the epoch bumps so stale
 * handles die immediately, the ring cursors zero and the slot frees.
 * Releasing the handle of the slot's immediately-previous lease is
 * idempotent (SDK contract) and returns AUDIO_FABRIC_LEASE_OK;
 * anything else stale returns AUDIO_FABRIC_LEASE_EHANDLE.
 */
int audio_fabric_lease_release(uint32_t handle);

/*
 * One per-slot state snapshot. pump_identity names the pump slot's
 * producer while a playback bind exists (SDK_AUDIO_METER_IDENTITY_*
 * vocabulary; other slots take the lease identity). hold_reset
 * consumes the lease peak-hold window on the next compositor read
 * (the scene-meter read-and-clear convention). Returns 0 for a bad
 * slot. The pump slot reports 0 peak/clip: its telemetry lives with
 * the scene meter, which already scans the mixed output.
 */
int audio_fabric_slot_state(uint32_t slot, uint32_t pump_identity,
	int hold_reset, struct audio_fabric_slot_state *out);

/* Host-test seam (scheduler.c TASKQ_HOST_TEST discipline): redirect
 * the TX ring the compositor fills. Firmware builds keep the standard
 * AUDIO_TX_BUFFER_ADDRESS. */
#ifdef AUDIO_FABRIC_HOST_TEST
/* Lease-plane host-test seam: redirect the card-side slot rings. */
void audio_fabric_host_set_lease_rings(uint8_t *slot1, uint8_t *slot2);
void audio_fabric_host_set_tx_base(uint8_t *base);
#endif

#ifdef AUDIO_FABRIC_BENCH
/*
 * Instrument-build report (plan U5, docs/audio-fabric.md): call from
 * the main loop every pass; it prints one aggregate block per second
 * on the firmware console. Firmware-only -- defining this together
 * with AUDIO_FABRIC_HOST_TEST is a compile error in audio_fabric.c.
 */
void audio_fabric_bench_poll(void);
#endif

#endif /* AUDIO_FABRIC_H */
