/*
 * Voice events from the libpt4 replayer. See zz9k-voice-pt4.h.
 *
 * A tick is run the way libpt4's own render loop runs it (do_tick in
 * libpt4/src/api.c): replayer tick, tempo and samples-per-tick update, then
 * the voice update. The trigger flags are read just before the voice update
 * consumes them, which tells a new note apart from a voice that only
 * changed period, volume or loop.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-voice-pt4.h"

#include <string.h>

#include "libpt4/src/pt4_internal.h"

/* EFx with x > 0 (funk repeat) rewrites sample bytes while the song plays,
 * which an uploaded bank cannot follow. libpt4's own uses_funk flag only
 * looks at the first four channels of a wide module, so every stored cell
 * is checked here. */
static int uses_funk(const pt4_song *s)
{
  size_t cells = (size_t)s->npat * 64U * (size_t)s->lay;
  size_t k;

  if (s->uses_funk) {
    return 1;
  }
  for (k = 0; k < cells; k++) {
    const uint8_t *c = s->pat + k * 4U;

    if ((c[2] & 0x0f) == 0x0e && (c[3] & 0xf0) == 0xf0 && (c[3] & 0x0f)) {
      return 1;
    }
  }
  return 0;
}

int zz9k_pt4_events_init(ZZ9KPt4Events *events, pt4_song *song,
                         int advance_voices)
{
  memset(events, 0, sizeof(*events));
  if ((pt4_extensions(song) & PT4_X_DYN) != 0 || uses_funk(song)) {
    return PT4_ERR_FORMAT;
  }
  if (pt4_set_option(song, PT4_OPT_PATH, PT4_PATH_WIDE) != PT4_OK ||
      pt4_set_rate(song, ZZ9K_PT4_RATE) != PT4_OK ||
      pt4_seek_ms(song, 0) != PT4_OK) {
    return PT4_ERR_ARG;
  }
  events->song = song;
  events->advance_voices = advance_voices;
  events->voices = song->wide ? PT4_MAXCH : song->lay;
  return PT4_OK;
}

static void emit_one(ZZ9KPt4EmitFn emit, void *user, uint32_t frame,
                     int voice, int kind, uint32_t a, uint32_t b)
{
  ZZ9KPt4Event event;

  event.frame = frame;
  event.voice = (uint8_t)voice;
  event.kind = (uint8_t)kind;
  event.a = a;
  event.b = b;
  emit(user, &event);
}

uint32_t zz9k_pt4_events_tick(ZZ9KPt4Events *events, ZZ9KPt4EmitFn emit,
                              void *user)
{
  pt4_song *s = events->song;
  pt4_state *st = &s->st;
  uint8_t trig[PT4_MAXCH];
  unsigned old_tempo;
  unsigned use;
  uint32_t frames;
  uint32_t frame = events->frame;
  int k;

  if (st->ended) {
    return 0;
  }
  old_tempo = st->tempo;
  pt4__tick(s);
  if (!st->playing) {
    return 0;
  }
  use = s->opt_latch ? old_tempo : st->tempo;
  if (st->spt_bpm != use || st->spt == 0) {
    pt4__calc_spt(s, use);
  }
  st->frac += st->spt;
  frames = st->frac >> 16;
  st->frac &= 0xffff;
  st->ticks_eclk += pt4__cia_period(use);

  for (k = 0; k < events->voices; k++) {
    trig[k] = (uint8_t)(st->ch[k].dmawait != 0 || st->aud[k].per == 1);
  }
  pt4__update_voices(s);

  for (k = 0; k < events->voices; k++) {
    const pt4_voice *v = &st->v[k];
    ZZ9KPt4Voice *sent = &events->sent[k];

    if (trig[k] && !v->active) {
      if (sent->on) {
        emit_one(emit, user, frame, k, ZZ9K_PT4_EV_STOP, 0, 0);
        sent->on = 0;
      }
      continue;
    }
    if (trig[k]) {
      /* START clears the engine voice's loop, so the loop follows it. */
      emit_one(emit, user, frame, k, ZZ9K_PT4_EV_START, v->pos,
               v->end - v->pos);
      sent->on = 1;
      sent->back = 0;
      emit_one(emit, user, frame, k, ZZ9K_PT4_EV_LOOP, v->loopstart,
               v->looplen);
      sent->loop_start = v->loopstart;
      sent->loop_length = v->looplen;
    } else if (v->loopstart != sent->loop_start ||
               v->looplen != sent->loop_length) {
      emit_one(emit, user, frame, k, ZZ9K_PT4_EV_LOOP, v->loopstart,
               v->looplen);
      sent->loop_start = v->loopstart;
      sent->loop_length = v->looplen;
    }
    if (v->period >= 5 && v->period != sent->period) {
      emit_one(emit, user, frame, k, ZZ9K_PT4_EV_PERIOD, v->period, 0);
      sent->period = v->period;
    }
    if (v->vol != sent->volume) {
      emit_one(emit, user, frame, k, ZZ9K_PT4_EV_VOLUME, v->vol, 0);
      sent->volume = v->vol;
    }
    if (!trig[k] && (uint8_t)v->back != sent->back) {
      emit_one(emit, user, frame, k, ZZ9K_PT4_EV_DIRECTION,
               (uint32_t)v->back, 0);
      sent->back = (uint8_t)v->back;
    }
  }

  if (events->advance_voices) {
    pt4__mix(s, NULL, NULL, 0, frames);
  }
  events->frame = frame + frames;
  return frames;
}

int zz9k_pt4_events_voices(const ZZ9KPt4Events *events)
{
  return events->voices;
}

const uint8_t *zz9k_pt4_events_bank(const ZZ9KPt4Events *events,
                                    uint32_t *size)
{
  *size = events->song->arena_size;
  return events->song->arena;
}
