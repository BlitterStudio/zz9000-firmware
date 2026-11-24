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
#define AUDIO_VOICE_INTERP_CUBIC   2U

/* The selectors of MODE and CONFIG records on the wire (zz9k/voice.h,
 * ZZ9K_VOICE_INTERP_*). A selector above INHERIT names the session value
 * AUDIO_VOICE_INTERP_* plus one. */
#define AUDIO_VOICE_INTERP_MODE_INHERIT 0U
#define AUDIO_VOICE_INTERP_MODE_NEAREST 1U
#define AUDIO_VOICE_INTERP_MODE_LINEAR  2U
#define AUDIO_VOICE_INTERP_MODE_CUBIC   3U

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
 *          fades out instead. flags: AUDIO_VOICE_STOP_RELEASE makes it a
 *          key-off: a voice that has an envelope enters its release (see
 *          "Envelopes"), without the stop ramp. Without an envelope the
 *          flag changes nothing and the record is a plain STOP. Other flag
 *          bits are ignored.
 *   DIRECTION flags: AUDIO_VOICE_DIR_BACKWARD sets backward play, clear
 *          sets forward play, on the voice as it is. No-op on a silent
 *          voice.
 *   MASTER arg0: master gain Q8.8 (256 = unity, capped at 0xFFFF); the
 *          voice field is ignored.
 *   MODE   voice: the voice. flags: the settings the record carries, only
 *          AUDIO_VOICE_MODE_INTERP, otherwise the record is rejected.
 *          arg0: an interpolation selector, AUDIO_VOICE_INTERP_MODE_INHERIT
 *          to _CUBIC (0 to 3), arg1 must be 0; otherwise the record is
 *          rejected and the voice keeps its setting. It takes effect on
 *          the voice at its frame (see "Interpolation").
 *   ENVELOPE  voice: the voice. flags: 0 removes the voice's envelope
 *          (arg0 and arg1 must be 0), AUDIO_VOICE_ENV_SET sets it from
 *          arg0 = (attack ms << 16) | decay ms and arg1 = (sustain << 16) |
 *          release ms; the times are 0 to AUDIO_VOICE_ENV_MS_MAX and the
 *          sustain level 0 to AUDIO_VOICE_ENV_SUSTAIN_UNITY (Q15, 32768 =
 *          full). Any other flag, or a sustain above the unity, rejects
 *          the record and the voice keeps its setting. The setting is for
 *          the next START of the voice (see "Envelopes").
 *   CONFIG flags: the settings the record carries; at least one, and only
 *          known bits, else the record is rejected whole. The voice field
 *          is ignored. AUDIO_VOICE_CONFIG_RAMP: arg0 is the ramp length in
 *          frames, 0 (off, the default) to AUDIO_VOICE_RAMP_MAX_FRAMES; a
 *          longer one rejects the record. AUDIO_VOICE_CONFIG_LIMITER:
 *          arg1 is 0 (off, the default) or 1 (on), otherwise the record is
 *          rejected. A limiter-only record requires arg0 = 0; a ramp-only
 *          record keeps ignoring arg1. Combined settings validate before
 *          either changes. AUDIO_VOICE_CONFIG_INTERP: arg0 is the session
 *          default interpolation, AUDIO_VOICE_INTERP_MODE_NEAREST to _CUBIC
 *          (1 to 3), arg1 must be 0, and the record names no other
 *          setting; anything else rejects it. Same-stamp records apply in
 *          their supplied order.
 *
 * Limiting. The optional static soft knee acts on the final sum after the
 * Q8.8 master. It is identity through magnitude 24576, then approximates
 * sign(x) * (24576 + 8191*d/(8191+d)), d = abs(x)-24576, within 2 S16
 * units. It is odd, monotonic and stays below either rail, including with
 * all voices and fade slots and the maximum master. No lookahead or state
 * between samples is used. Disabling it restores the original S16 clamp.
 *
 * Interpolation. Each voice reads its sample nearest, linear or with the
 * 4-point Catmull-Rom cubic. The session default is AUDIO_VOICE_INTERP_*
 * (nearest, or linear when the session was opened with the linear flag; a
 * CONFIG record changes it). A voice follows the default until a MODE
 * record gives it a selector of its own. A selector is a wire value:
 * AUDIO_VOICE_INTERP_MODE_INHERIT (0, follow the default), _NEAREST,
 * _LINEAR or _CUBIC. A MODE or CONFIG record takes effect at its frame:
 * frames before it use the old setting, so the same-stamp order of the
 * records decides what a frame reads. The setting of a voice stays across
 * START, STOP and RESET, like its rate and volume. A fade slot freezes the
 * interpolation the voice has at the restart (the default included) with
 * its position, rate, geometry and gain: later MODE and CONFIG records
 * never change a tail.
 *   Nearest and linear are the arithmetic described under "Mixing" in
 * audio_voice.c, unchanged. Cubic reads the frame before the position
 * (the previous frame in play order), the frame at the position and the
 * next two in play order, a, b, c and d, with t the 16-bit fraction as the
 * linear blend takes it, and returns
 *   b + (t * ((c - a) + t * ((2a - 5b + 4c - d) +
 *                              t * (d - a + 3(b - c))))) / 2
 * in 16-bit units, evaluated in fixed point with every product rounded
 * (see cubic_interp() in audio_voice.c); 8-bit data is promoted by 8 bits
 * first. The result lies within 1 unit of the real-valued curve and can be
 * up to 25% outside the S16 range, so a cubic voice's side is scaled by its
 * volume in 64 bits (or the vector equivalent) and the sums and the master
 * keep a wider margin than the 32-bit bound of the other modes. The frames
 * around the position follow the voice: in play order the next frame after
 * the pass end is the loop start when there is a loop and the last frame
 * again when there is none, and the frame before the loop start is the
 * loop end once a pass has wrapped. Backward play counts in its own play
 * order (the frame below the position is the next one) as the linear blend
 * does.
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
 * without ramping. With R 0 and the limiter off, the output is identical
 * to an engine without CONFIG. A ramp already running when R becomes 0
 * runs out, but new records act at once.
 *
 * Envelopes. A voice that has an envelope when it starts multiplies its
 * volume by a level L from 0 to 1 that follows the stages below. The level
 * is kept in Q30; a frame uses it as the Q15 multiplier m = L >> 15 (32768
 * is 1). The envelope is independent of the volume and of the ramp: the
 * volume used for a frame is ((g >> 8) * m) >> 15, g being the Q8 gain the
 * volume or the ramp gives that frame (see "Ramping"), so SET volume moves
 * the base level only, a ramp keeps its linear shape, and a voice without
 * an envelope (m = 32768) plays exactly as before. The level is constant
 * over blocks of AUDIO_VOICE_ENV_STEP_FRAMES (16) frames and changes
 * between them. A stage of t ms lasts 3 t blocks (48 frames per ms), so
 * times are exact on the 48 kHz timeline and quantised to 16 frames
 * (1/3 ms); there is no floating point, and the curves are integer
 * stepping of a 2^(-x) table (see audio_voice.c).
 *   key-on   START starts the attack at its frame with block 0, using the
 *          envelope the voice had at that record (a later ENVELOPE does
 *          not change a sounding voice). A START of a voice that is
 *          sounding restarts the attack from the start; with ramping on the
 *          old sound goes to a fade slot first, carrying its envelope
 *          state, which keeps running there on its own.
 *   attack A blocks (A = 3 * attack ms): block j has L = floor(j * 2^30 /
 *          A), so it rises linearly from 0 and the decay starts at block A
 *          with L = 1. A = 0 skips it: the first block is the first block
 *          of the decay.
 *   decay  D blocks: block k has L = S + (1 - S) * 2^(-16 k / D), a 96 dB
 *          exponential from 1 toward the sustain level S, equal to S from
 *          block D on. About 62% of the time passes before the curve has
 *          fallen 60 dB. D = 0 skips it: the level is S at once.
 *   sustain  L = S until the key-off. S = 32768 / 32768 with no attack and
 *          no decay is the constant level 1.
 *   key-off  STOP with AUDIO_VOICE_STOP_RELEASE on a voice in any stage but
 *          the release enters the release at the record's frame from the
 *          level L0 the voice has then (the block in progress), not from
 *          the sustain level. The release has R blocks (R = 3 * release ms),
 *          starts a new block at the record's frame, and block k has
 *          L = L0 * 2^(-16 k / R), a 96 dB exponential from L0. After block
 *          R-1 the voice ends: it deactivates and the engine counts one
 *          end (e->ends) unless it is a fade slot. R = 0 ends it at the
 *          key-off frame. A second key-off changes nothing; a key-off of
 *          a stopping voice (a plain STOP ramp is running) changes
 *          nothing; a key-off of a voice without an envelope is a plain
 *          STOP.
 *   STOP   a plain STOP on a voice with an envelope ramps the base gain
 *          down (or silences it at once without ramping) as without an
 *          envelope and counts no end; the envelope runs on meanwhile. Only
 *          a release that finishes counts an end: a voice that ends first
 *          for another reason (a pass end, or the stop ramp) counts none.
 *          A pass end, a ramp end and a release end on the same frame
 *          apply in that order, and the first that deactivates the voice
 *          decides.
 * A release or any other stage runs whether or not the voice is audible
 * (volume 0, no rate), so its end is on time. The release end is measured
 * in blocks, never by the level reaching 0, so a long release of a low
 * level does not end early. A voice's setting persists across START, STOP
 * and RESET like its rate and volume; a fade slot copies the state of the
 * sounding voice and nothing sent later changes it. A restart hands a
 * sound to a fade slot unless its gain or its envelope level is 0.
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

#define AUDIO_VOICE_STOP_RELEASE 0x0001U

#define AUDIO_VOICE_DIR_BACKWARD 0x0001U

#define AUDIO_VOICE_SET_RATE   0x0001U
#define AUDIO_VOICE_SET_VOLUME 0x0002U
#define AUDIO_VOICE_RATE_HZ    0x0004U

#define AUDIO_VOICE_CONFIG_RAMP  0x0001U
#define AUDIO_VOICE_CONFIG_LIMITER 0x0002U
#define AUDIO_VOICE_CONFIG_INTERP  0x0004U
#define AUDIO_VOICE_CONFIG_KNOWN \
	(AUDIO_VOICE_CONFIG_RAMP | AUDIO_VOICE_CONFIG_LIMITER | \
	 AUDIO_VOICE_CONFIG_INTERP)

#define AUDIO_VOICE_MODE_INTERP  0x0001U
#define AUDIO_VOICE_MODE_KNOWN   AUDIO_VOICE_MODE_INTERP

#define AUDIO_VOICE_ENV_SET            0x0001U
#define AUDIO_VOICE_ENV_KNOWN          AUDIO_VOICE_ENV_SET
#define AUDIO_VOICE_ENV_STEP_FRAMES    16U
#define AUDIO_VOICE_ENV_STEPS_PER_MS   3U     /* 48 frames per ms / 16 */
#define AUDIO_VOICE_ENV_MS_MAX         65535U
#define AUDIO_VOICE_ENV_SUSTAIN_UNITY  0x8000U
/* Envelope level and multiplier: Q30 level, Q15 multiplier. */
#define AUDIO_VOICE_ENV_LEVEL_ONE      (1U << 30)
#define AUDIO_VOICE_ENV_MUL_UNITY      32768U
/* audio_voice.env_stage */
#define AUDIO_VOICE_ENV_NONE           0U
#define AUDIO_VOICE_ENV_ATTACK         1U
#define AUDIO_VOICE_ENV_DECAY          2U
#define AUDIO_VOICE_ENV_SUSTAIN        3U
#define AUDIO_VOICE_ENV_RELEASE        4U

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

/* An ENVELOPE setting in blocks of AUDIO_VOICE_ENV_STEP_FRAMES frames. */
struct audio_voice_env {
	uint32_t attack;  /* blocks */
	uint32_t decay;   /* blocks */
	uint32_t release; /* blocks */
	uint32_t sustain; /* level, Q30 */
};

struct audio_voice {
	uint8_t active;
	uint8_t backward;
	uint8_t shift;      /* log2 of the frame size in bytes */
	uint8_t wide;       /* 16-bit samples */
	uint8_t stereo;     /* interleaved left and right samples */
	uint8_t stopping;   /* ramping to silence, then inactive */
	uint8_t vinterp;    /* MODE interpolation: 0 inherit, else INTERP_MODE_* */
	uint8_t looped;     /* a pass has wrapped into the loop (cubic taps) */
	uint8_t env_on;     /* env_set holds an envelope for the next START */
	uint8_t env_stage;  /* AUDIO_VOICE_ENV_*: the envelope the voice is in */
	uint8_t tail;       /* a fade slot: its end is not counted */
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
	/* Envelope (see "Envelopes"): the setting for the next START, the
	 * setting the sounding voice started with, the stage and its
	 * position. env_left is the frames until the next block change, or
	 * 0xFFFFFFFF in the sustain; emul is the Q15 multiplier of the
	 * current block (32768 without an envelope). env_acc steps from 0 to
	 * the stage total (2^30 for the attack, 16 octaves in Q24 for the
	 * others) over env_n blocks by the Bresenham split env_whole + env_rem
	 * / env_n, env_err the remainder. */
	struct audio_voice_env env_set;
	struct audio_voice_env env;
	uint32_t env_k;
	uint32_t env_n;
	uint32_t env_whole;
	uint32_t env_rem;
	uint32_t env_err;
	uint32_t env_acc;
	uint32_t env_level;
	uint32_t env_base; /* the level a release started from */
	uint32_t env_left;
	uint32_t emul;
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
	uint32_t limiter;    /* CONFIG static soft knee, 0 = off */
	uint32_t ends;       /* voices of the client that ended by their release */
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
	int chunk_cubic;      /* a cubic voice may have added to the sums */
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
 * CONFIG settings and counters are kept.
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
