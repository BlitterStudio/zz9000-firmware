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

#ifndef ZZ_AUDIO_VOICE_H
#define ZZ_AUDIO_VOICE_H

#include <stddef.h>
#include <stdint.h>

#define AUDIO_VOICE_MAX_VOICES  32U
#define AUDIO_VOICE_MAX_SAMPLES 256U
#define AUDIO_VOICE_OUT_HZ      48000U
#define AUDIO_VOICE_MAX_HZ      192000U
/* Periods below this are ignored. */
#define AUDIO_VOICE_MIN_PERIOD  5U
#define AUDIO_VOICE_MASTER_UNITY 256U
#define AUDIO_VOICE_MASTER_MAX   0xFFFFU
/* Longest ramp, and the fade slots that hold the tails of restarted voices. */
#define AUDIO_VOICE_RAMP_MAX_FRAMES 512U
#define AUDIO_VOICE_FADE_SLOTS      32U
/* Frames mixed per chunk; a longer render call is mixed in chunks. */
#define AUDIO_VOICE_MIX_FRAMES   960U

#define AUDIO_VOICE_CLOCK_PAL  3546895U
#define AUDIO_VOICE_CLOCK_NTSC 3579545U

/*
 * Sample formats. 16-bit data is big-endian, as the Amiga wrote it. The
 * stereo formats hold interleaved frames, the left sample then the right
 * one: a frame is 2 bytes in S8S and 4 bytes in S16S. A DEFINE accepts
 * formats below AUDIO_VOICE_FMT_COUNT. The numbers match ZZ9K_VOICE_FORMAT_*
 * in the SDK's zz9k/voice.h.
 */
#define AUDIO_VOICE_FMT_S8    0U
#define AUDIO_VOICE_FMT_S16   1U
#define AUDIO_VOICE_FMT_S8S   2U
#define AUDIO_VOICE_FMT_S16S  3U
#define AUDIO_VOICE_FMT_COUNT 4U

#define AUDIO_VOICE_INTERP_NEAREST 0U
#define AUDIO_VOICE_INTERP_LINEAR  1U

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
 *   SET    flags: AUDIO_VOICE_SET_RATE and/or AUDIO_VOICE_SET_VOLUME;
 *          AUDIO_VOICE_RATE_HZ makes the rate a frequency in Hz instead
 *          of a Paula period.
 *          arg0: rate. arg1: (left volume << 16) | right volume, each
 *          0..65535. A period below 5, or 0 Hz, is ignored and the voice
 *          keeps its previous step. A voice that never received a rate has
 *          step 0: it outputs nothing and does not advance.
 *   STOP   silences the voice. With ramping on (see "Ramping") the voice
 *          fades out instead.
 *   DIRECTION flags: AUDIO_VOICE_DIR_BACKWARD sets backward play, clear
 *          sets forward play, on the voice as it is. No-op on a silent
 *          voice.
 *   MASTER arg0: master gain Q8.8 (256 = unity, capped at 0xFFFF); the
 *          voice field is ignored.
 *   MODE, ENVELOPE  reserved op numbers of later features. The engine
 *          rejects them.
 *   CONFIG flags: the settings the record carries; at least one, and only
 *          known bits, else the record is rejected whole. The voice field
 *          is ignored. AUDIO_VOICE_CONFIG_RAMP: arg0 is the ramp length in
 *          frames, 0 (off, the default) to AUDIO_VOICE_RAMP_MAX_FRAMES; a
 *          longer one rejects the record. arg1 is reserved.
 *
 * A voice number of 32 or more, or an unknown op, is rejected.
 * Every byte quantity (offsets and lengths of START and LOOP) is divided
 * into frames, rounding down: by 1 for S8, 2 for S16 and S8S, and 4 for
 * S16S. A stereo sample plays its left sample at the left volume and its
 * right sample at the right volume; a mono sample feeds both sides.
 *
 * Ramping. With a ramp length R above 0, set by CONFIG, volume changes,
 * starts, stops and restarts are not instant. Each side of a voice has a
 * gain, kept in Q8 (volume << 8), that moves linearly to its target over R
 * frames and then equals the target; the volume used for frame j of a ramp
 * is (gain + j * step) >> 8, where step is (target - gain) / R in C
 * division (toward zero) taken when the ramp starts, and frame R uses the
 * target. A ramp starts at the record's frame and a new target retargets
 * from the gain the voice has at that frame.
 *   START  the voice rises from silence to its volume over R frames. A
 *          voice that was sounding hands its sound to a fade slot first:
 *          a copy that keeps playing from where it was and fades from its
 *          gain to silence over R frames (a voice that was already
 *          stopping keeps the schedule it has). A START that is rejected
 *          still hands the old sound over, so the old sound fades.
 *          The pool has AUDIO_VOICE_FADE_SLOTS slots; when all are in
 *          use the oldest is overwritten (its tail is cut). No slot is
 *          taken for a sound that is silent or has no rate.
 *   SET volume  on a sounding voice, ramps from the current gain to the
 *          new volume. On a stopping voice it only stores the volume.
 *   STOP   on a sounding voice that is not stopping, ramps its gain to
 *          silence over R frames and then deactivates it (at once when it
 *          is silent). A second STOP changes nothing.
 * A fade slot is a frozen copy: later LOOP, DIRECTION, SET and STOP
 * records for the voice number never touch it, and a STOP does not end
 * earlier tails. A voice that reaches its pass end stops at once, as
 * without ramping. With R 0 none of this applies and the output is
 * identical to an engine without CONFIG; a ramp already running when R
 * becomes 0 runs out, but new records act at once.
 */
#define AUDIO_VOICE_OP_START     1U
#define AUDIO_VOICE_OP_LOOP      2U
#define AUDIO_VOICE_OP_SET       3U
#define AUDIO_VOICE_OP_STOP      4U
#define AUDIO_VOICE_OP_MASTER    5U
#define AUDIO_VOICE_OP_DIRECTION 6U
#define AUDIO_VOICE_OP_MODE      7U
#define AUDIO_VOICE_OP_ENVELOPE  8U
#define AUDIO_VOICE_OP_CONFIG    9U

#define AUDIO_VOICE_DIR_BACKWARD 0x0001U

#define AUDIO_VOICE_SET_RATE   0x0001U
#define AUDIO_VOICE_SET_VOLUME 0x0002U
#define AUDIO_VOICE_RATE_HZ    0x0004U

#define AUDIO_VOICE_CONFIG_RAMP  0x0001U
#define AUDIO_VOICE_CONFIG_KNOWN AUDIO_VOICE_CONFIG_RAMP

struct audio_voice_cmd {
	uint32_t stamp; /* engine frame to apply at */
	uint8_t voice;
	uint8_t op;
	uint16_t flags;
	uint32_t arg0;
	uint32_t arg1;
};

struct audio_voice_sample {
	uint32_t offset; /* bytes into the bank */
	uint32_t length; /* bytes */
	uint8_t format;  /* AUDIO_VOICE_FMT_* */
	uint8_t defined;
};

struct audio_voice {
	uint8_t active;
	uint8_t backward;
	uint8_t shift;      /* log2 of the frame size in bytes */
	uint8_t wide;       /* 16-bit samples */
	uint8_t stereo;     /* interleaved left and right samples */
	uint8_t stopping;   /* ramping to silence, then inactive */
	uint32_t base;      /* sample byte offset in the bank */
	uint32_t len;       /* sample length in frames */
	uint32_t pos;       /* frame index */
	uint32_t frac;      /* 16-bit fraction */
	uint32_t end;       /* current pass end, in frames */
	uint32_t loopstart; /* frames; lower bound of backward play */
	uint32_t looplen;   /* frames, 0 = no loop */
	uint32_t step;      /* 16.16 samples per output frame, 0 = no rate */
	uint32_t vol_l;     /* volume target */
	uint32_t vol_r;
	/* Ramp state, used only while ramp_left is not 0: the gain of each
	 * side in Q8 (volume << 8), its change per frame and the frames
	 * left. With ramp_left 0 the volume is vol_l and vol_r. */
	uint32_t ramp_left;
	int32_t gain_l;
	int32_t gain_r;
	int32_t inc_l;
	int32_t inc_r;
	/* Render state, no meaning between render calls: the frames of the
	 * current chunk this voice has been mixed to, and for a fade slot
	 * the engine frame at which it ends. */
	uint32_t cur;
	uint32_t die;
};

struct audio_voice_engine {
	const uint8_t *bank;
	uint32_t bank_len;
	uint32_t clock;      /* Paula clock for period input */
	uint32_t interp;     /* AUDIO_VOICE_INTERP_* */
	uint32_t master;     /* Q8.8 */
	uint32_t frame;      /* index of the next frame to render; wraps */
	uint32_t late;       /* commands applied after their frame */
	uint32_t rejected;   /* commands refused */
	uint32_t ramp_frames; /* CONFIG ramp length, 0 = off */
	uint32_t fade_seq;   /* age counter of the fade slots */
	struct audio_voice voice[AUDIO_VOICE_MAX_VOICES];
	/* Sounds handed over by a restart (see "Ramping"): voices in the
	 * stopping state that no record addresses. */
	struct audio_voice fade[AUDIO_VOICE_FADE_SLOTS];
	uint32_t fade_age[AUDIO_VOICE_FADE_SLOTS];
	struct audio_voice_sample sample[AUDIO_VOICE_MAX_SAMPLES];
	/* Scratch of audio_voice_render(), no state between calls: the int32
	 * sum of the voices per side (left, right), and the samples of the
	 * voice being ramped, for one chunk of frames; and the chunk being
	 * rendered (see audio_voice.c, "Rendering a chunk"). */
	int32_t mix[2][AUDIO_VOICE_MIX_FRAMES];
	int32_t smp[2][AUDIO_VOICE_MIX_FRAMES];
	int16_t *chunk_out;
	uint32_t chunk_n;     /* frames of the chunk */
	uint32_t chunk_abs;   /* engine frame of its first frame */
	uint32_t chunk_fin;   /* frames already scaled into the output */
	uint32_t chunk_off;   /* frame of the chunk the next record applies at */
	int chunk_dirty;      /* the sums were cleared and may hold sound */
};

/*
 * Reset the engine: all voices silent, no samples defined, frame counter
 * 0, counters 0, master unity, nearest interpolation. clock is
 * AUDIO_VOICE_CLOCK_PAL or AUDIO_VOICE_CLOCK_NTSC. The bank must stay
 * valid and unmoved while the engine uses it.
 */
void audio_voice_init(struct audio_voice_engine *e, const uint8_t *bank,
                      uint32_t bank_len, uint32_t clock);

/*
 * Silence every voice and every fade slot at once; samples, master, the
 * ramp setting and counters are kept.
 */
void audio_voice_silence(struct audio_voice_engine *e);

/*
 * Define (or redefine) sample id. Returns 0, or -1 when the id is out of
 * range, the format is unknown, or the byte range is not inside the bank.
 * A playing voice keeps the geometry it started with.
 */
int audio_voice_sample_set(struct audio_voice_engine *e, uint32_t id,
                           uint32_t offset, uint32_t length, uint32_t format);

/*
 * Render `frames` frames of interleaved stereo int16 into out (4 bytes per
 * frame), applying cmds (sorted by stamp) at the frame whose index equals
 * the stamp. A command stamped before the first frame of this call applies
 * at frame 0 and counts as late. Commands stamped at or beyond
 * e->frame + frames stay unconsumed. Advances e->frame by frames; frames
 * must not exceed 0x7FFFFFFF. Returns the number of commands consumed.
 */
size_t audio_voice_render(struct audio_voice_engine *e, int16_t *out,
                          uint32_t frames, const struct audio_voice_cmd *cmds,
                          size_t ncmds);

/*
 * Read-only voice state: stores the sample index and 16-bit fraction of the
 * voice position and returns 1 when the voice is active, else 0. The
 * position of an inactive voice is where it stopped; both read 0 for a
 * voice number out of range.
 */
int audio_voice_position(const struct audio_voice_engine *e, uint32_t voice,
                         uint32_t *index, uint32_t *frac);

#endif /* ZZ_AUDIO_VOICE_H */
