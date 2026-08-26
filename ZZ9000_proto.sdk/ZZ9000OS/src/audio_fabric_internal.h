/*
 * AX audio fabric compositor: private types and cross-TU seams.
 *
 * The public API lives in audio_fabric.h. This header is included only by
 * the two firmware translation units that share the slot-table layout:
 * audio_fabric.c (compositor core, ISR + main-loop producer lifecycle)
 * and audio_fabric_lease.c (the mailbox lease plane). It exists so the
 * lease TU can reach the slot table through narrow accessors instead of
 * a raw global.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIO_FABRIC_INTERNAL_H
#define AUDIO_FABRIC_INTERNAL_H

#include <stdint.h>

#include "audio_fabric.h"
#include "memorymap.h"

/* Lease-plane per-slot state (plan U3). Meaningful only for the
 * leaseable slots; ring == NULL means no lease holds the slot. */
struct audio_fabric_lease {
	uint8_t *ring;         /* card-side source ring (memorymap.h) */
	uint32_t capacity;
	uint64_t produced;     /* published bytes; SUBMIT is the writer */
	uint64_t staged;       /* compositor-staged bytes; ISR writer */
	uint32_t identity;     /* SDK_AUDIO_METER_IDENTITY_* */
	uint32_t generation;   /* slot epoch this lease runs under */
	uint8_t state;         /* AUDIO_FABRIC_SLOT_STATE_LEASED/ACTIVE */
	uint8_t tearing;       /* release quiesce mark (KTD3) */
	uint8_t peak_reset;    /* HOLD_RESET read opens a fresh window */
	uint8_t clip_open;     /* at-rail region continuity latch */
	uint32_t peak;         /* 16.16 peak-hold of source samples */
	uint32_t clips;        /* at-rail source regions, saturating */
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
	uint32_t last_dma_offset; /* active DMA period at the last ISR */
	uint32_t period_staged[AUDIO_NUM_PERIODS];
	uint32_t silence_run;     /* consecutive silent ISR periods */
	uint32_t underruns;       /* saturating, this slot only (R8) */
	uint8_t staged_real;      /* real PCM staged this ISR */
	uint16_t gain;            /* unity until the lease plane */
	/* Per-slot conversion instance (phase continuity per producer). */
	struct zz_audio_convert convert;
	uint32_t convert_rate;
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

/* Warm-reset belt (R7): zero every wired card-side lease ring so a new
 * mailbox lifetime can never read the previous one's samples. Lease TU;
 * called from audio_fabric_reset(). */
void fabric_lease_reset_rings(void);

/* The compositor ISR is the single writer of a lease's staged cursor;
 * a reader in the mailbox thread can still tear a 64-bit load across
 * its two words if an audio IRQ lands between them, so read
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
