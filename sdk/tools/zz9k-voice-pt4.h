/*
 * Voice events from the libpt4 replayer.
 *
 * Runs libpt4 one replayer tick at a time and reports what changed on each
 * voice (start, stop, loop, direction, period, volume), stamped with the
 * 48 kHz frame the tick starts at. A client sends these events to the
 * ZZ9000AX voice engine; the firmware host tests feed them to the engine and
 * compare the result with libpt4's own WIDE output. The sample bank is
 * libpt4's sample arena: event offsets are byte addresses in it.
 *
 * Only the WIDE path is reproduced (every voice on its own). Modules with
 * DYN volume envelopes or the EFx funk effect are refused: the first plays
 * chunk lists, the second rewrites sample bytes while playing, and neither
 * can be expressed as voice events.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_VOICE_PT4_H
#define ZZ9K_VOICE_PT4_H

#include <stdint.h>

#include "libpt4/include/pt4.h"

#define ZZ9K_PT4_RATE 48000U
#define ZZ9K_PT4_VOICES 32

enum {
  ZZ9K_PT4_EV_START = 1,     /* a = bank offset, b = length in bytes */
  ZZ9K_PT4_EV_STOP = 2,
  ZZ9K_PT4_EV_LOOP = 3,      /* a = loop start (bank offset), b = loop length, 0 = none */
  ZZ9K_PT4_EV_DIRECTION = 4, /* a = 1 backward, 0 forward */
  ZZ9K_PT4_EV_PERIOD = 5,    /* a = Paula period (PAL clock), always >= 5 */
  ZZ9K_PT4_EV_VOLUME = 6     /* a = volume 0..64 */
};

typedef struct ZZ9KPt4Event {
  uint32_t frame;
  uint8_t voice;
  uint8_t kind;
  uint32_t a;
  uint32_t b;
} ZZ9KPt4Event;

typedef void (*ZZ9KPt4EmitFn)(void *user, const ZZ9KPt4Event *event);

typedef struct ZZ9KPt4Voice {
  uint8_t on;
  uint8_t back;
  uint16_t period;
  uint16_t volume;
  uint32_t loop_start;
  uint32_t loop_length;
} ZZ9KPt4Voice;

typedef struct ZZ9KPt4Events {
  pt4_song *song;
  uint32_t frame;
  int advance_voices;
  int voices;
  ZZ9KPt4Voice sent[ZZ9K_PT4_VOICES];
} ZZ9KPt4Events;

/*
 * Prepares song for event output: WIDE path, 48 kHz, playback from the
 * start. With advance_voices set, every tick also steps libpt4's own voices
 * through the tick (no output), so effects that look at whether a voice is
 * still playing behave as in libpt4's rendering; a player on a slow CPU
 * leaves it clear. Returns PT4_OK or PT4_ERR_FORMAT for a module this
 * cannot reproduce.
 */
int zz9k_pt4_events_init(ZZ9KPt4Events *events, pt4_song *song,
                         int advance_voices);

/*
 * Runs one replayer tick and emits each voice change stamped with the frame
 * the tick starts at. Returns the tick length in frames, or 0 when the song
 * has stopped (F00 or its end, loop off).
 */
uint32_t zz9k_pt4_events_tick(ZZ9KPt4Events *events, ZZ9KPt4EmitFn emit,
                              void *user);

/* Number of voices the WIDE mix divides by 4/voices: 32 for wide modules. */
int zz9k_pt4_events_voices(const ZZ9KPt4Events *events);

/* libpt4's sample arena: the bytes to upload as the engine's sample bank. */
const uint8_t *zz9k_pt4_events_bank(const ZZ9KPt4Events *events,
                                    uint32_t *size);

#endif
