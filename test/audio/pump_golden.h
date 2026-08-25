/*
 * Characterization golden for the pre-fabric AX playback pump.
 *
 * Captured VERBATIM from ZZ9000_proto.sdk/ZZ9000OS/src/sdk_mailbox.c at
 * commit 9b3654f (the audio-fabric U2 baseline: "feat(audio): reserve
 * fabric lease ABI surface"), before the compositor refactor moved this
 * fill loop into audio_fabric.c. Every computation line is unchanged;
 * only these seams were mechanical:
 *
 *   - the TX ring base, the DMA transfer count and the producer
 *     callbacks (snapshot/stage/retire/underrun, stream-tail state)
 *     arrive through the instance instead of firmware globals,
 *   - audio_silence()'s ring effect (zero the whole TX ring) is applied
 *     to the injected ring; the formatter/I2C half of
 *     audio_playback_start (audio_set_tx_buffer + conditional
 *     audio_init_i2s) is transport, not ring content, and stays out of
 *     this characterization,
 *   - audio_scene_meter_output_identity() publishes metering state
 *     outside the ring and is dropped.
 *
 * Do not "fix" anything here: divergence from the original pump is
 * exactly what the parity test has to catch.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PUMP_GOLDEN_H
#define PUMP_GOLDEN_H

#include <stdint.h>

#include "audio_convert.h"
#include "memorymap.h"

#define PUMP_GOLDEN_PERIOD_BYTES  AUDIO_BYTES_PER_PERIOD
#define PUMP_GOLDEN_RING_BYTES    AUDIO_TX_BUFFER_SIZE
#define PUMP_GOLDEN_TARGET_AHEAD \
	(PUMP_GOLDEN_RING_BYTES - 2U * PUMP_GOLDEN_PERIOD_BYTES)
#define PUMP_GOLDEN_SOURCE_NONE   0U
#define PUMP_GOLDEN_SOURCE_STREAM 1U
#define PUMP_GOLDEN_SOURCE_MEDIA  2U

/* Mirror of the pump's struct SDKAudioPumpSource snapshot. */
struct pump_golden_source {
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

/* Producer seams the golden calls in place of the sdk_mailbox helpers. */
struct pump_golden_ops {
	int  (*snapshot)(struct pump_golden_source *source);
	int  (*stage)(uint32_t bytes);
	void (*retire)(uint32_t bytes);
	void (*underrun)(void);
	/* Stream play-out tail (pump_tail_pending) lives in the model. */
	void (*tail_real)(void);
	void (*tail_drained)(void);
	int  (*tail_pending)(void);
};

struct pump_golden {
	/* seams */
	uint8_t *tx;
	uint32_t (*dma_count)(void);
	const struct pump_golden_ops *ops;
	/* mirrored g_audio_playback state */
	uint32_t session;         /* 0 = unbound */
	uint32_t source_kind;
	uint32_t paused;
	uint32_t fill_offset;     /* next TX-ring byte to fill, period-aligned */
	uint32_t last_dma_offset; /* active DMA period at the last ISR */
	uint32_t period_source_bytes[AUDIO_NUM_PERIODS];
	uint32_t silence_run;     /* consecutive silent pump ISR periods */
	/* mirrored g_pump_convert / g_pump_src / g_pump_stereo */
	struct zz_audio_convert convert;
	uint32_t convert_rate;
	int16_t src[PUMP_GOLDEN_PERIOD_BYTES / 2];
	int16_t stereo[PUMP_GOLDEN_PERIOD_BYTES / 2];
};

/* audio_playback_start() state half (cursor/tag rebuild + publish). */
void pump_golden_start(struct pump_golden *g, uint32_t source_kind,
                       uint32_t session);
/* audio_playback_stop() including the audio_silence() ring wipe. */
void pump_golden_stop(struct pump_golden *g);
/* Media pause-confirm: paused gate + clear tags + ring wipe. */
void pump_golden_pause(struct pump_golden *g);
/* sdk_mailbox_audio_playback_pump_isr(), TX-fill half. */
void pump_golden_isr(struct pump_golden *g);

#endif /* PUMP_GOLDEN_H */
