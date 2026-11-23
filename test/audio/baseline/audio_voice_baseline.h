/*
 * FROZEN COPY of the voice engine at master dfa9b767, for the exact
 * comparison in test/audio/audio_voice_baseline_test.c. Never edit it to
 * follow engine changes: the test is the proof that the engine still renders
 * the same PCM. Made with
 *   git show dfa9b767:ZZ9000_proto.sdk/ZZ9000OS/src/audio_voice.{c,h} |
 *     sed -e 's/audio_voice/audio_voice_baseline/g' \
 *         -e 's/AUDIO_VOICE/AUDIO_VOICE_BASELINE/g'
 * so that both engines link into one test binary. Only the names differ.
 */

/*
 * ZZ9000AX voice engine core.
 *
 * Mixes up to 32 wavetable voices at 48 kHz into interleaved stereo int16
 * (the audio fabric TX format) from a card-side sample bank. Voices are
 * driven by decoded commands that carry the engine frame they apply at;
 * a render call splits the period at each stamp so a command takes effect
 * on exactly that output frame.
 *
 * Integer only, no globals and no allocation: the caller owns the engine
 * struct and the bank, so the same code runs host-side in test/audio and
 * target-side on the main loop.
 *
 * Voice position math is libpt4's WIDE path: an integer index plus a 16-bit
 * fraction, step = (hz << 16) / 48000, forward wrap into the loop by the
 * overshoot, backward wrap by end - under - 1.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ_AUDIO_VOICE_BASELINE_H
#define ZZ_AUDIO_VOICE_BASELINE_H

#include <stddef.h>
#include <stdint.h>

#define AUDIO_VOICE_BASELINE_MAX_VOICES  32U
#define AUDIO_VOICE_BASELINE_MAX_SAMPLES 256U
#define AUDIO_VOICE_BASELINE_OUT_HZ      48000U
#define AUDIO_VOICE_BASELINE_MAX_HZ      192000U
/* Periods below this are ignored. */
#define AUDIO_VOICE_BASELINE_MIN_PERIOD  5U
#define AUDIO_VOICE_BASELINE_MASTER_UNITY 256U
#define AUDIO_VOICE_BASELINE_MASTER_MAX   0xFFFFU

#define AUDIO_VOICE_BASELINE_CLOCK_PAL  3546895U
#define AUDIO_VOICE_BASELINE_CLOCK_NTSC 3579545U

/*
 * Sample formats. 16-bit data is big-endian, as the Amiga wrote it. The
 * stereo formats hold interleaved frames, the left sample then the right
 * one: a frame is 2 bytes in S8S and 4 bytes in S16S. A DEFINE accepts
 * formats below AUDIO_VOICE_BASELINE_FMT_COUNT. The numbers match ZZ9K_VOICE_FORMAT_*
 * in the SDK's zz9k/voice.h.
 */
#define AUDIO_VOICE_BASELINE_FMT_S8    0U
#define AUDIO_VOICE_BASELINE_FMT_S16   1U
#define AUDIO_VOICE_BASELINE_FMT_S8S   2U
#define AUDIO_VOICE_BASELINE_FMT_S16S  3U
#define AUDIO_VOICE_BASELINE_FMT_COUNT 4U

#define AUDIO_VOICE_BASELINE_INTERP_NEAREST 0U
#define AUDIO_VOICE_BASELINE_INTERP_LINEAR  1U

/*
 * Command ops. A command is one decoded record (host endian; the wire
 * format is the session layer's concern):
 *
 *   START  flags: sample id.
 *          arg0: start offset in bytes from the sample start.
 *          arg1: first-pass play length in bytes (the first pass ends at
 *                offset + length, clamped to the sample end); 0 plays to
 *                the sample end.
 *          The voice starts forward at the offset with a zero fraction and
 *          its loop cleared; send LOOP after START. A START whose sample
 *          id is undefined, or whose offset is at or beyond the sample
 *          length, silences the voice and counts as rejected. Rate and
 *          volume are kept from earlier SET commands.
 *   LOOP   arg0: loop start in bytes from the sample start.
 *          arg1: loop length in bytes; 0 means no loop.
 *          Takes effect on the running voice at once without moving its
 *          position or its current pass end; the loop is used when the pass
 *          reaches its end. Without a loop, the loop start is the lower
 *          bound of backward play. A loop outside the voice's sample (loop
 *          start beyond the sample length, or start + length beyond it) is
 *          rejected and the voice keeps its previous loop.
 *   SET    flags: AUDIO_VOICE_BASELINE_SET_RATE and/or AUDIO_VOICE_BASELINE_SET_VOLUME;
 *          AUDIO_VOICE_BASELINE_RATE_HZ makes the rate a frequency in Hz instead
 *          of a Paula period.
 *          arg0: rate. arg1: (left volume << 16) | right volume, each
 *          0..65535. A period below 5, or 0 Hz, is ignored and the voice
 *          keeps its previous step. A voice that never received a rate has
 *          step 0: it outputs nothing and does not advance.
 *   STOP   silences the voice.
 *   DIRECTION flags: AUDIO_VOICE_BASELINE_DIR_BACKWARD sets backward play, clear
 *          sets forward play, on the voice as it is. No-op on a silent
 *          voice.
 *   MASTER arg0: master gain Q8.8 (256 = unity, capped at 0xFFFF); the
 *          voice field is ignored.
 *
 * A voice number of 32 or more, or an unknown op, is rejected.
 * Every byte quantity (offsets and lengths of START and LOOP) is divided
 * into frames, rounding down: by 1 for S8, 2 for S16 and S8S, and 4 for
 * S16S. A stereo sample plays its left sample at the left volume and its
 * right sample at the right volume; a mono sample feeds both sides.
 */
#define AUDIO_VOICE_BASELINE_OP_START     1U
#define AUDIO_VOICE_BASELINE_OP_LOOP      2U
#define AUDIO_VOICE_BASELINE_OP_SET       3U
#define AUDIO_VOICE_BASELINE_OP_STOP      4U
#define AUDIO_VOICE_BASELINE_OP_MASTER    5U
#define AUDIO_VOICE_BASELINE_OP_DIRECTION 6U

#define AUDIO_VOICE_BASELINE_DIR_BACKWARD 0x0001U

#define AUDIO_VOICE_BASELINE_SET_RATE   0x0001U
#define AUDIO_VOICE_BASELINE_SET_VOLUME 0x0002U
#define AUDIO_VOICE_BASELINE_RATE_HZ    0x0004U

struct audio_voice_baseline_cmd {
	uint32_t stamp; /* engine frame to apply at */
	uint8_t voice;
	uint8_t op;
	uint16_t flags;
	uint32_t arg0;
	uint32_t arg1;
};

struct audio_voice_baseline_sample {
	uint32_t offset; /* bytes into the bank */
	uint32_t length; /* bytes */
	uint8_t format;  /* AUDIO_VOICE_BASELINE_FMT_* */
	uint8_t defined;
};

struct audio_voice_baseline {
	uint8_t active;
	uint8_t backward;
	uint8_t shift;      /* log2 of the frame size in bytes */
	uint8_t wide;       /* 16-bit samples */
	uint8_t stereo;     /* interleaved left and right samples */
	uint32_t base;      /* sample byte offset in the bank */
	uint32_t len;       /* sample length in frames */
	uint32_t pos;       /* frame index */
	uint32_t frac;      /* 16-bit fraction */
	uint32_t end;       /* current pass end, in frames */
	uint32_t loopstart; /* frames; lower bound of backward play */
	uint32_t looplen;   /* frames, 0 = no loop */
	uint32_t step;      /* 16.16 samples per output frame, 0 = no rate */
	uint32_t vol_l;
	uint32_t vol_r;
};

struct audio_voice_baseline_engine {
	const uint8_t *bank;
	uint32_t bank_len;
	uint32_t clock;      /* Paula clock for period input */
	uint32_t interp;     /* AUDIO_VOICE_BASELINE_INTERP_* */
	uint32_t master;     /* Q8.8 */
	uint32_t frame;      /* index of the next frame to render; wraps */
	uint32_t late;       /* commands applied after their frame */
	uint32_t rejected;   /* commands refused */
	struct audio_voice_baseline voice[AUDIO_VOICE_BASELINE_MAX_VOICES];
	struct audio_voice_baseline_sample sample[AUDIO_VOICE_BASELINE_MAX_SAMPLES];
};

/*
 * Reset the engine: all voices silent, no samples defined, frame counter
 * 0, counters 0, master unity, nearest interpolation. clock is
 * AUDIO_VOICE_BASELINE_CLOCK_PAL or AUDIO_VOICE_BASELINE_CLOCK_NTSC. The bank must stay
 * valid and unmoved while the engine uses it.
 */
void audio_voice_baseline_init(struct audio_voice_baseline_engine *e, const uint8_t *bank,
                      uint32_t bank_len, uint32_t clock);

/* Silence every voice at once; samples, master and counters are kept. */
void audio_voice_baseline_silence(struct audio_voice_baseline_engine *e);

/*
 * Define (or redefine) sample id. Returns 0, or -1 when the id is out of
 * range, the format is unknown, or the byte range is not inside the bank.
 * A playing voice keeps the geometry it started with.
 */
int audio_voice_baseline_sample_set(struct audio_voice_baseline_engine *e, uint32_t id,
                           uint32_t offset, uint32_t length, uint32_t format);

/*
 * Render `frames` frames of interleaved stereo int16 into out (4 bytes per
 * frame), applying cmds (sorted by stamp) at the frame whose index equals
 * the stamp. A command stamped before the first frame of this call applies
 * at frame 0 and counts as late. Commands stamped at or beyond
 * e->frame + frames stay unconsumed. Advances e->frame by frames; frames
 * must not exceed 0x7FFFFFFF. Returns the number of commands consumed.
 */
size_t audio_voice_baseline_render(struct audio_voice_baseline_engine *e, int16_t *out,
                          uint32_t frames, const struct audio_voice_baseline_cmd *cmds,
                          size_t ncmds);

/*
 * Read-only voice state: stores the sample index and 16-bit fraction of the
 * voice position and returns 1 when the voice is active, else 0. The
 * position of an inactive voice is where it stopped; both read 0 for a
 * voice number out of range.
 */
int audio_voice_baseline_position(const struct audio_voice_baseline_engine *e, uint32_t voice,
                         uint32_t *index, uint32_t *frac);

#endif /* ZZ_AUDIO_VOICE_BASELINE_H */
