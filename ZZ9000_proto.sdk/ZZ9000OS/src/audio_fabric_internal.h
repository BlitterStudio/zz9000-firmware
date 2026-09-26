/*
 * AX audio fabric compositor: private types and cross-TU seams.
 *
 * The public API lives in audio_fabric.h. This header is included only by
 * the two firmware translation units that share the slot-table layout:
 * audio_fabric.c (compositor core, ISR + main-loop producer lifecycle)
 * and audio_fabric_lease.c (the direct-ring lease plane). It exists so
 * the lease TU can reach the slot table through narrow accessors instead
 * of a raw global.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIO_FABRIC_INTERNAL_H
#define AUDIO_FABRIC_INTERNAL_H

#include <stdint.h>

#include "audio_fabric.h"
#include "memorymap.h"
#include "sdk_mailbox.h"

/* Lease preconvert staging (drivers#83 follow-up: the #100 discipline
 * on the lease plane). A converting lease (source rate other than
 * 48 kHz) gets its whole source periods converted on the main loop
 * (audio_fabric_lease_poll) into a private staging ring of 48-kHz
 * periods; the compositor ISR fill then only copies. Each staging
 * period carries the whole-period source-byte cost, so the
 * period-tag/credit plane stays source-denominated and the producer
 * line's cursors are untouched (the ISR's stage op still advances
 * lease.consumed by exactly that cost).
 *
 * The 32-bit staging cursors wrap after ~6.2 h of continuous
 * converted playback; the poll rebases both under an IRQ-safe
 * critical section exactly like the pump's preconvert ring (PR #88
 * review), so the ISR's single-word reads never tear and the modulo
 * ring position never jumps (2^32 is not a multiple of the staging
 * ring size). */
#define AUDIO_FABRIC_LEASE_STAGING_PERIODS 32U
/* Staging periods the queued-period rebuild may still replay: bounded
 * by the TX ring (a staged-but-unretired TX window). The poll keeps
 * this many periods behind the ISR consumed cursor intact, leaving
 * 23 periods (~460 ms) of main-loop jitter runway -- pump-preconvert
 * comparable. */
#define AUDIO_FABRIC_LEASE_REPLAY_PERIODS AUDIO_NUM_PERIODS
#define FABRIC_LEASE_STAGING_NONE UINT32_MAX

struct fabric_lease_preconvert {
	uint32_t staged;       /* staging bytes published (poll writer) */
	uint32_t consumed;     /* staging bytes staged into TX (ISR writer) */
	uint64_t src_consumed; /* source bytes pulled by the poll */
	uint32_t cost[AUDIO_FABRIC_LEASE_STAGING_PERIODS];
	uint8_t active;
	uint8_t primed;
};

uint8_t *fabric_lease_staging_ring(uint32_t slot);

/* Lease-plane per-slot state (plan U3, direct rings). ring == NULL means
 * no lease holds the slot. The granted PCM ring and the 128-byte seqlock
 * control block live in board-visible memory (memorymap.h direct-ring
 * reservations); the producer writes the producer line and the PCM, the
 * firmware writes the firmware line and consumes complete periods. */
struct audio_fabric_lease {
	uint8_t *ring;            /* granted card-side PCM ring */
	uint8_t *control;         /* control block base: producer line at
	                           * +0 (SDKAudioRingProducerLine), firmware
	                           * line at +64 (SDKAudioRingFirmwareLine) */
	uint32_t capacity;
	uint32_t identity;        /* SDK_AUDIO_METER_IDENTITY_* */
	uint32_t source_rate;     /* 48000 bypass, or the validated
	                           * conversion-bearing source rate */
	uint32_t generation;      /* slot epoch this lease runs under */
	uint64_t consumed;        /* bytes staged into the TX ring; the
	                           * compositor ISR is the single writer */
	uint64_t credited;        /* bytes retired by the DMA and published
	                           * as ring credit on the firmware line;
	                           * monotonic, never above consumed */
	uint64_t write_cursor;    /* tick-refreshed producer line view */
	uint64_t published_credits; /* last credited value pushed to the
	                             * firmware line (skip redundant
	                             * publications) */
	uint32_t heartbeat_ms;    /* ISR-tick age of the producer token */
	uint32_t heartbeat_token; /* last observed producer token */
	uint32_t published_status; /* last status pushed, same purpose */
	uint8_t state;            /* AUDIO_FABRIC_SLOT_STATE_LEASED/ACTIVE */
	uint8_t line_valid;       /* tick found a stable, in-generation,
	                           * sane-distance producer line */
	uint8_t paused;           /* producer line carries FLAG_PAUSED */
	uint8_t revoke;           /* tick decided REVOKED_*; teardown rides
	                           * the compositor drop path */
	uint8_t tearing;          /* release quiesce mark (KTD3) */
	uint8_t peak_reset;       /* HOLD_RESET read opens a fresh window */
	uint8_t clip_open;        /* at-rail region continuity latch */
	uint32_t peak;            /* 16.16 peak-hold of source samples */
	uint32_t clips;           /* at-rail source regions, saturating */
};

struct audio_fabric_slot {
	const struct audio_fabric_producer_ops *ops;
	uint32_t epoch;          /* attach generation (STATE_GET vocab) */
	uint8_t attached;
	uint8_t live;            /* attached and filling/retiring */
	uint8_t frozen;          /* attached, parked by the owner */
	uint8_t tail_pending;    /* real PCM staged, tail not played out */
	/* Cached producer snapshot (KTD2): ring, capacity, cursors. */
	struct audio_fabric_source source;
	uint32_t admission_rate;  /* negotiated rate published by the owner
	                           * before a newly attached slot goes live */
	uint32_t last_dma_offset; /* active DMA period at the last ISR */
	uint32_t period_staged[AUDIO_NUM_PERIODS];
	uint32_t silence_run;     /* consecutive silent ISR periods */
	uint32_t underruns;       /* saturating, this slot only (R8) */
	uint8_t staged_real;      /* real PCM staged this ISR */
	uint16_t gain;            /* unity until the lease plane */
	/* Staging period sequence number staged into each TX period
	 * (FABRIC_LEASE_STAGING_NONE when that TX period carries none of
	 * this slot's audio). Set by the compositor fill; read by the
	 * queued-period rebuild; cleared wholesale by
	 * audio_fabric_producer_restart / ring_silence / producer_clear,
	 * which run from both the compositor tick (ISR) and the owner's
	 * main-loop setup paths. */
	uint32_t staged_seq[AUDIO_NUM_PERIODS];
	struct zz_audio_convert convert;
	uint32_t convert_rate;
	/* Lease preconvert staging; active only for converting leases. */
	struct fabric_lease_preconvert preconvert;
	/* Lease-plane state; zero (ring NULL) for the pump slot. */
	struct audio_fabric_lease lease;
};

/* Narrow seams between the two translation units. The compositor core
 * owns the slot table; the lease plane reaches it only through these. */

/* Range-checked slot accessor (NULL for out-of-range slots). Core TU. */
struct audio_fabric_slot *fabric_slot(uint32_t slot);

/* Meter one committed lease period against its identity. Lease TU; the
 * core fill loop calls it instead of touching lease metering state. */
void fabric_lease_meter(struct audio_fabric_slot *s,
	const int16_t *pcm, uint32_t bytes);

/* Warm-reset belt (R7): zero every mapped direct-ring grant (PCM ring
 * plus both control lines) so a new mailbox lifetime can never read the
 * previous one's samples. Lease TU; called from audio_fabric_reset(). */
void fabric_lease_reset_rings(void);

/* One direct-ring scan per compositor ISR, before the retire loop (the
 * lease TU): seqlock-stable read of every wired producer line with
 * generation, flag and write-distance validation, heartbeat aging, the
 * LEASED -> ACTIVE transition on the first valid publication (frontier
 * re-arm when the fabric is otherwise idle) and REVOKED_* teardown for
 * heartbeat expiry and cursor faults. Malformed lines isolate the slot
 * to silence for the pass; peers are untouched. */
void fabric_lease_isr_tick(void);
/* Convert published source the main loop has not staged yet. The ISR
 * calls this before fill so a late poll cannot play silence over PCM
 * the host already wrote. No-op when staging is already ahead. */
void fabric_lease_catchup(void);

/* Request the queued-contribution rebuild (core TU): after this slot
 * detaches, the next compositor ISR rebuilds every TX period strictly
 * ahead of the DMA position from the remaining live producers, so only
 * the current 20-ms period may still carry the detached lease's audio.
 * Main-loop and ISR callers; plain store, ISR-consumed. */
void audio_fabric_request_rebuild(uint32_t slot);

/* The compositor ISR is the single writer of a lease's consumed/credited
 * cursors; a reader in the mailbox thread can still tear a 64-bit load
 * across its two words if an audio IRQ lands between them, so read
 * high-low-high and retry while the high word moves. */
static inline uint64_t fabric_lease_read_cursor(
	const volatile uint64_t *cursor)
{
	const volatile uint32_t *words =
		(const volatile uint32_t *)(const volatile void *)cursor;
	uint32_t high_before;
	uint32_t low;
	uint32_t high_after;

	do {
		high_before = words[1];
		low = words[0];
		high_after = words[1];
	} while (high_before != high_after);
	return ((uint64_t)high_before << 32) | (uint64_t)low;
}

#endif /* AUDIO_FABRIC_INTERNAL_H */
