#ifndef AUDIO_PUMP_PRECONVERT_H
#define AUDIO_PUMP_PRECONVERT_H

#include <stdint.h>

#include "audio_convert.h"

#define AUDIO_PUMP_PRECONVERT_PERIOD_BYTES 3840U
/* Rebuild reserve (plan U3): fill stops this many periods short of the
 * staged cursor so the bytes a queued-period rebuild replays (a
 * direct-ring lease detach rebuilds future TX periods from peers'
 * staged-not-yet-credited windows, at most the TX fill target of six
 * periods below staged) can never be overwritten by the decode side.
 * Matches AUDIO_FABRIC_TARGET_AHEAD / AUDIO_FABRIC_PERIOD_BYTES. */
#define AUDIO_PUMP_PRECONVERT_REPLAY_PERIODS 6U
#define AUDIO_PUMP_PRECONVERT_REPLAY_KEEP \
	(AUDIO_PUMP_PRECONVERT_REPLAY_PERIODS * \
	 AUDIO_PUMP_PRECONVERT_PERIOD_BYTES)
#define AUDIO_PUMP_PRECONVERT_MAX_SOURCE_FRAMES 960U

struct audio_pump_preconvert_source {
	uint8_t *ring;
	uint32_t capacity;
	uint32_t produced;
	uint32_t consumed;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t sample_format;
	int done;
};

struct audio_pump_preconvert {
	uint8_t *ring;
	uint32_t capacity;
	uint32_t produced;
	uint32_t staged;
	uint32_t convert_rate;
	struct zz_audio_convert convert;
	int16_t source[AUDIO_PUMP_PRECONVERT_MAX_SOURCE_FRAMES * 2U];
	int16_t output[AUDIO_PUMP_PRECONVERT_PERIOD_BYTES / 2U];
};

void audio_pump_preconvert_reset(struct audio_pump_preconvert *state,
	uint8_t *ring, uint32_t capacity);
uint32_t audio_pump_preconvert_used(
	const struct audio_pump_preconvert *state);
int audio_pump_preconvert_fill(struct audio_pump_preconvert *state,
	const struct audio_pump_preconvert_source *source,
	uint32_t *source_consumed);
int audio_pump_preconvert_stage(struct audio_pump_preconvert *state,
	uint32_t bytes);

#endif
