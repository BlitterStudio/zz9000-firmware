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
 * direct-ring lease plane (opcodes 0x0512+: the SDK producer writes
 * PCM and a seqlock control line straight into its board-visible
 * grant; mailbox copy-submit is never on the data path). The per-slot
 * mix discipline is saturating int32 accumulate then one S16 clamp
 * per frame (KTD4); a late, absent, paused or malformed producer
 * contributes silence without stalling the others and only its own
 * underrun counter moves (R8).
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

/* Bounded slot table (R4): the pump slot plus the lease plane (U3). */
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
/* Drop one producer's queued-period tags without touching the shared TX
 * ring (pause with a live peer); pair with audio_fabric_request_rebuild. */
void audio_fabric_producer_clear(uint32_t slot);
void audio_fabric_producer_freeze(uint32_t slot);
void audio_fabric_producer_go_live(uint32_t slot);
/* Shared-frontier guard for restart callers: nonzero when any slot
 * other than `slot` is live. Re-arming the shared fill frontier under
 * a live mix would re-fill the other producers' staged periods and
 * double-count their staging, so media resume skips the re-arm while
 * this is set (the same guard the lease-submit path applies). */
int audio_fabric_others_live(uint32_t slot);
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

/*
 * Lease plane (plan U3, direct rings): generation-tagged producer
 * leases over the board-visible direct-ring grants of memorymap.h.
 * The SDK producer writes PCM and a seqlock producer line directly
 * into its granted ring; the firmware consumes complete periods and
 * publishes credits on its own line.
 */

/* Per-slot status (SDK_AUDIO_FABRIC_SLOT_* vocabulary). The pump slot
 * is FREE/LEASED/ACTIVE by playback bind state (never leased); a
 * lease is LEASED from acquire until its first valid line publication
 * and ACTIVE while the compositor consumes its ring. REVOKED reports
 * an invalidated generation (heartbeat expiry or cursor fault); the
 * slot may be re-acquired without a card reset. */
#define AUDIO_FABRIC_SLOT_STATE_FREE    0U
#define AUDIO_FABRIC_SLOT_STATE_LEASED  1U
#define AUDIO_FABRIC_SLOT_STATE_ACTIVE  2U
#define AUDIO_FABRIC_SLOT_STATE_REVOKED 3U

/* Lease admission results; the mailbox layer maps these onto the
 * SDK_STATUS_* vocabulary. */
#define AUDIO_FABRIC_LEASE_OK        0
#define AUDIO_FABRIC_LEASE_EBAD_SLOT (-1)  /* slot 0 / out of range */
#define AUDIO_FABRIC_LEASE_EBUSY     (-2)  /* slot already leased */
#define AUDIO_FABRIC_LEASE_ELEGACY   (-3)  /* legacy-exclusive output */
#define AUDIO_FABRIC_LEASE_ENO_MAP   (-4)  /* no grant map on this bus
                                            * configuration (legacy
                                            * Zorro II) */

/* One framed per-slot state snapshot (SDK_OP_AUDIO_FABRIC_STATE_GET).
 * Peak follows the scene-meter convention: unsigned 16.16 of the
 * loudest source sample read since the hold window opened, consumed
 * by a HOLD_RESET read; clips counts at-rail source regions and is
 * monotonic within a lease. written/consumed are the monotonic byte
 * cursors of the slot's ring: the lease's producer-line write cursor
 * and its credited (played-out) cursor. heartbeat_ms is the
 * ISR-measured age of the producer's liveness token
 * (SDK_AUDIO_RING_HEARTBEAT_UNKNOWN while free or unmeasured). */
struct audio_fabric_slot_state {
	uint32_t state;            /* AUDIO_FABRIC_SLOT_STATE_* */
	uint32_t identity;         /* SDK_AUDIO_METER_IDENTITY_* */
	uint32_t generation;       /* lease epoch of (the last) lease */
	uint64_t written_bytes;    /* producer-line write cursor */
	uint64_t consumed_bytes;   /* credited (played) cursor */
	uint32_t heartbeat_ms;     /* token age, or UNKNOWN */
	uint32_t underruns;        /* saturating, this slot only */
	uint32_t peak;             /* 16.16, hold-window semantics */
	uint32_t clips;            /* at-rail regions, saturating */
};

/*
 * Applied-vs-requested gain report (R11): acquire composes the
 * requested gain through the scene arbiter's ceiling-bounded
 * composition and reports what actually applied, so a bounded
 * request is never silently clamped (the I3 trim-bound pattern).
 */
struct audio_fabric_ring_grant {
	uint32_t generation;     /* revocation token: release carries it */
	uint32_t ring_offset;    /* board-visible PCM ring offset */
	uint32_t ring_capacity;  /* whole periods of 3840 bytes */
	uint32_t control_offset; /* board-visible control-block offset */
	uint32_t gain_applied;   /* composed 0..255 mixer scale */
	uint8_t bounded;         /* nonzero when the request was reduced */
	uint8_t slot_count;      /* leaseable slots on this bus mode */
	uint8_t bus_zorro2;      /* compact single-slot geometry */
};

/*
 * Acquire a direct-ring lease. slot 0 (the pump) is never leaseable;
 * a slot beyond the active bus mode's grant map (two leaseable slots
 * on Zorro III, one on Zorro II under an acknowledged generation-2
 * aperture layout) completes AUDIO_FABRIC_LEASE_EBAD_SLOT, and a bus
 * configuration with no grant map at all (legacy Zorro II) completes
 * AUDIO_FABRIC_LEASE_ENO_MAP -- the mailbox layer answers BAD_REQUEST
 * and UNSUPPORTED respectively. A leased slot completes EBUSY
 * (mailbox: SDK_STATUS_BUSY), as does a legacy-exclusive output. gain
 * is the single requested 0..255 mixer scale
 * (AUDIO_FABRIC_GAIN_UNITY is unity); the scene arbiter composes it
 * against the enforced ceiling under the active scene (R11) and the
 * applied value -- possibly bounded, never silently -- is reported
 * through *grant. On success the PCM ring and both control lines are
 * zeroed (the ghost-period bound of R5 depends on this), the firmware
 * line publishes the generation with zero credits, and the lease runs
 * LEASED until the producer's first valid line publication.
 */
int audio_fabric_ring_acquire(uint32_t slot, uint32_t identity,
	uint32_t gain, uint32_t source_rate,
	struct audio_fabric_ring_grant *grant);

/*
 * Surrender a direct-ring lease, quiesce-then-rebuild (KTD3): the
 * slot stops contributing after at most the current 20-ms period (the
 * next compositor ISR rebuilds every queued future TX period from the
 * remaining producers), the epoch bumps so the dying generation never
 * validates again, and the final credits are published before the
 * slot frees. Releasing a generation other than the live lease's is
 * idempotent (the SDK contract makes release retry-safe); a free
 * slot accepts any generation the same way.
 */
int audio_fabric_ring_release(uint32_t slot, uint32_t generation);

/*
 * One per-slot state snapshot. pump_identity names the pump slot's
 * producer while a playback bind exists (SDK_AUDIO_METER_IDENTITY_*
 * vocabulary; other slots take the lease identity). hold_reset
 * consumes the lease peak-hold window on the next compositor read
 * (the scene-meter read-and-clear convention). Returns 0 for a bad
 * slot. The pump slot is never leased: it reports 0 peak/clip and
 * UNKNOWN heartbeat -- its telemetry lives with the scene meter,
 * which already scans the mixed output.
 */
int audio_fabric_slot_state(uint32_t slot, uint32_t pump_identity,
	int hold_reset, struct audio_fabric_slot_state *out);

/* Host-test seam (scheduler.c TASKQ_HOST_TEST discipline): redirect
 * the TX ring the compositor fills. Firmware builds keep the standard
 * AUDIO_TX_BUFFER_ADDRESS. */
#ifdef AUDIO_FABRIC_HOST_TEST
/* Lease-plane host-test seam: redirect the card-side grants (PCM
 * ring, 128-byte control block, capacity in bytes) and shorten the
 * heartbeat timeout so revocation scenarios run in ISR ticks. */
void audio_fabric_host_set_ring_grant(uint32_t slot, uint8_t *ring,
	uint8_t *control, uint32_t capacity);
void audio_fabric_host_set_heartbeat_timeout(uint32_t ms);
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

#ifndef AUDIO_FABRIC_HOST_TEST
/*
 * Lease open-loop diagnostics (silence-after-start session): call from
 * the main loop every pass; one line per ~50 compositor ISRs while a
 * lease is wired. Firmware-only.
 */
void audio_fabric_lease_diag_poll(void);
void audio_fabric_diag_trace_arm(void);
#endif

#endif /* AUDIO_FABRIC_H */
