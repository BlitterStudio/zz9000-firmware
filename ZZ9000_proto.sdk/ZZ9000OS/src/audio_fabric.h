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
 * routed implicitly from their first bind); slots 2 and 3 are reserved
 * for the lease plane (opcodes 0x050f+, still ABI-reserved). The
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

/* Host-test seam (scheduler.c TASKQ_HOST_TEST discipline): redirect
 * the TX ring the compositor fills. Firmware builds keep the standard
 * AUDIO_TX_BUFFER_ADDRESS. */
#ifdef AUDIO_FABRIC_HOST_TEST
void audio_fabric_host_set_tx_base(uint8_t *base);
#endif

#endif /* AUDIO_FABRIC_H */
