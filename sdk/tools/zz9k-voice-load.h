/*
 * The record schedule of `zz9k-voiceplay LOAD ... CHURN`, with or without
 * `ENV`: the restarts of the voices in groups and, with ENV, the key-off of
 * every note.
 *
 * It lives in a header so that the host test (test/audio/audio_voice_load_test.c)
 * runs the same code as the tool and can check the order the ring needs.
 *
 * Every voice starts at frame 0 (the tool writes that itself). A restart slot
 * n (n = 1, 2, ...) starts the group of voices whose number is n modulo
 * `groups` at frame n * step: a START and a LOOP over the sample. With a hold
 * above 0 each note also gets a key-off, a STOP with the release flag, `hold`
 * frames after its start: key-off slot m is start slot m shifted by `hold`,
 * and slot 0 is the start of every voice at frame 0. The two kinds of slot are
 * generated in the order of their frames, the key-off first when both fall on
 * the same frame, because the ring takes stamps in order and a key-off that
 * waits for the starts of other groups before it would be written behind them.
 * The hold must be shorter than groups * step, the time between two starts of
 * a voice, so that a key-off never meets the next start of its voice. A first
 * note whose voice restarts before its key-off is due gets no key-off.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_VOICE_LOAD_H
#define ZZ9K_VOICE_LOAD_H

#include "zz9k/voice.h"

#include <stdint.h>

#define ZZ9K_VOICE_LOAD_NONE        0xFFFFFFFFU
/* The most records one slot writes: a START and a LOOP for each of 32 voices. */
#define ZZ9K_VOICE_LOAD_MAX_RECORDS (2U * ZZ9K_VOICE_MAX_VOICES)

typedef struct ZZ9KVoiceLoad {
  uint32_t voices;     /* 1 to 32 */
  uint32_t groups;     /* restart groups, 1 to voices */
  uint32_t step;       /* frames between two restart slots, 1 or more */
  uint32_t hold;       /* frames from a start to its key-off, 0 for no key-offs */
  uint32_t loop_bytes; /* the LOOP length */
  uint32_t start_slot; /* the next restart slot, from 1 */
  uint32_t off_slot;   /* the next key-off slot, from 0 */
  uint32_t restarts;   /* voices restarted so far */
  uint32_t keyoffs;    /* key-offs written so far */
} ZZ9KVoiceLoad;

static inline void zz9k_voice_load_init(ZZ9KVoiceLoad *l, uint32_t voices,
                                        uint32_t groups, uint32_t step,
                                        uint32_t hold, uint32_t loop_bytes)
{
  l->voices = voices;
  l->groups = groups;
  l->step = step;
  l->hold = hold;
  l->loop_bytes = loop_bytes;
  l->start_slot = 1U;
  l->off_slot = 0U;
  l->restarts = 0U;
  l->keyoffs = 0U;
}

/* The frames between two starts of one voice. */
static inline uint32_t zz9k_voice_load_period(const ZZ9KVoiceLoad *l)
{
  return l->groups * l->step;
}

/* Whether the hold fits the schedule: none, or shorter than the period. */
static inline int zz9k_voice_load_hold_ok(const ZZ9KVoiceLoad *l)
{
  return l->hold == 0U || l->hold < zz9k_voice_load_period(l);
}

/* The first frame at which a voice restarts after its start at frame 0. */
static inline uint32_t zz9k_voice_load_first_restart(const ZZ9KVoiceLoad *l,
                                                     uint32_t voice)
{
  const uint32_t group = voice % l->groups;

  return (group == 0U ? l->groups : group) * l->step;
}

/* The frame of the next slot, and in *keyoff whether it is a key-off slot. */
static inline uint32_t zz9k_voice_load_next(const ZZ9KVoiceLoad *l,
                                            uint32_t *keyoff)
{
  const uint32_t start = l->start_slot * l->step;
  const uint32_t off = l->hold != 0U ? l->off_slot * l->step + l->hold
                                     : ZZ9K_VOICE_LOAD_NONE;

  if (l->hold != 0U && off <= start) {
    if (keyoff) {
      *keyoff = 1U;
    }
    return off;
  }
  if (keyoff) {
    *keyoff = 0U;
  }
  return start;
}

/* The records a slot can write at most: what the pending queue must have room
 * for before zz9k_voice_load_emit(). */
static inline uint32_t zz9k_voice_load_room(const ZZ9KVoiceLoad *l)
{
  const uint32_t starts = 2U * ((l->voices + l->groups - 1U) / l->groups);

  return l->hold != 0U && l->voices > starts ? l->voices : starts;
}

/* Writes the next slot, its records stamped at lead + its frame, into out
 * (at least ZZ9K_VOICE_LOAD_MAX_RECORDS) and returns how many. A key-off slot
 * can write none. */
static inline uint32_t zz9k_voice_load_emit(ZZ9KVoiceLoad *l, uint32_t lead,
                                            ZZ9KVoiceRecord *out)
{
  uint32_t keyoff;
  const uint32_t stamp = lead + zz9k_voice_load_next(l, &keyoff);
  uint32_t n = 0U;
  uint32_t voice;

  if (keyoff) {
    for (voice = l->off_slot == 0U ? 0U : l->off_slot % l->groups;
         voice < l->voices;
         voice += l->off_slot == 0U ? 1U : l->groups) {
      if (l->off_slot == 0U &&
          l->hold >= zz9k_voice_load_first_restart(l, voice)) {
        continue;
      }
      zz9k_voice_build_stop_release(&out[n++], stamp, (uint8_t)voice);
      l->keyoffs++;
    }
    l->off_slot++;
    return n;
  }
  for (voice = l->start_slot % l->groups; voice < l->voices;
       voice += l->groups) {
    zz9k_voice_build_start(&out[n++], stamp, (uint8_t)voice, 0U, 0U, 0U);
    zz9k_voice_build_loop(&out[n++], stamp, (uint8_t)voice, 0U, l->loop_bytes);
    l->restarts++;
  }
  l->start_slot++;
  return n;
}

#endif /* ZZ9K_VOICE_LOAD_H */
