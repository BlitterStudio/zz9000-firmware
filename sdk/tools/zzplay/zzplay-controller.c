/* Playback controller implementation for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-controller.h"

#include <string.h>

static void zzplay_safe_copy(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0U) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, cap - 1U);
  dst[cap - 1U] = '\0';
}

/* Stored now-playing text is bounded; equality is after the same truncation
 * that a write would apply. */
static int zzplay_safe_copy_if_changed(char *dst, size_t cap,
                                       const char *src)
{
  size_t i;

  if (!dst || cap == 0U) {
    return 0;
  }
  if (!src) {
    src = "";
  }
  for (i = 0U; i + 1U < cap; i++) {
    if (dst[i] != src[i]) {
      zzplay_safe_copy(dst, cap, src);
      return 1;
    }
    if (src[i] == '\0') {
      return 0;
    }
  }
  return 0;
}

void zzplay_controller_init(ZZPlayController *ctl, uint32_t volume,
                            ZZPlayRepeat repeat, int shuffle)
{
  if (!ctl) {
    return;
  }
  memset(ctl, 0, sizeof(*ctl));
  ctl->volume = volume > ZZPLAY_VOLUME_MAX ? ZZPLAY_VOLUME_MAX : volume;
  ctl->repeat = repeat;
  ctl->shuffle = shuffle ? 1 : 0;
  ctl->jump_index = -1;
  ctl->dirty = ZZPLAY_DIRTY_ALL;
}

int zzplay_controller_request(ZZPlayController *ctl, ZZPlayRequest request)
{
  if (!ctl || request == ZZPLAY_REQUEST_NONE) {
    return 0;
  }
  /* Precedence: QUIT > STOP > latest-of-rest. */
  if (ctl->request == ZZPLAY_REQUEST_QUIT) {
    return request == ZZPLAY_REQUEST_QUIT;
  }
  if (ctl->request == ZZPLAY_REQUEST_STOP) {
    if (request == ZZPLAY_REQUEST_QUIT) {
      ctl->request = ZZPLAY_REQUEST_QUIT;
      return 1;
    }
    return request == ZZPLAY_REQUEST_STOP;
  }
  ctl->request = request;
  return 1;
}

int zzplay_controller_jump(ZZPlayController *ctl, int32_t index)
{
  if (!ctl || index < 0) {
    return 0;
  }
  if (!zzplay_controller_request(ctl, ZZPLAY_REQUEST_JUMP)) {
    return 0;
  }
  ctl->jump_index = index;
  return 1;
}

int zzplay_controller_seek_to(ZZPlayController *ctl, uint32_t ms)
{
  if (!ctl || !ctl->item_active || !ctl->now.seekable) {
    return 0;
  }
  /* Seeking to (or past) a known end leaves no audio to restart on:
   * advance instead, as the forward seek key does. */
  if (ctl->now.total_ms > 0U && ms >= ctl->now.total_ms) {
    return zzplay_controller_request(ctl, ZZPLAY_REQUEST_NEXT);
  }
  if (!zzplay_controller_request(ctl, ZZPLAY_REQUEST_SEEK)) {
    return 0;
  }
  ctl->seek_ms = ms;
  return 1;
}

void zzplay_controller_set_volume(ZZPlayController *ctl, uint32_t volume)
{
  if (!ctl) {
    return;
  }
  if (volume > ZZPLAY_VOLUME_MAX) {
    volume = ZZPLAY_VOLUME_MAX;
  }
  if (ctl->volume != volume) {
    ctl->volume = volume;
    ctl->volume_changed = 1;
    ctl->dirty |= ZZPLAY_DIRTY_STATE;
  }
}

void zzplay_controller_set_repeat(ZZPlayController *ctl,
                                  ZZPlayRepeat repeat)
{
  if (!ctl) {
    return;
  }
  if (ctl->repeat != repeat) {
    ctl->repeat = repeat;
    ctl->dirty |= ZZPLAY_DIRTY_STATE;
  }
}

void zzplay_controller_set_shuffle(ZZPlayController *ctl, int shuffle)
{
  int norm;

  if (!ctl) {
    return;
  }
  norm = shuffle ? 1 : 0;
  if (ctl->shuffle != norm) {
    ctl->shuffle = norm;
    ctl->dirty |= ZZPLAY_DIRTY_STATE;
  }
}

int zzplay_controller_apply(ZZPlayController *ctl,
                            ZZPlayControlAction action)
{
  if (!ctl) {
    return 0;
  }
  switch (action) {
  case ZZPLAY_CONTROL_TOGGLE_PAUSE:
    if (ctl->item_active) {
      /* While a modal operation is queued the gate owns the pause: a toggle
       * now would resume the item before the engine confirmed the hold, and
       * the requester would never be released. */
      if (ctl->modal != ZZPLAY_MODAL_NONE) {
        return 0;
      }
      ctl->paused = !ctl->paused;
      ctl->dirty |= ZZPLAY_DIRTY_STATE;
      return 1;
    }
    return zzplay_controller_request(ctl, ZZPLAY_REQUEST_PLAY);

  case ZZPLAY_CONTROL_STOP_KEY:
  case ZZPLAY_CONTROL_STOP_WINDOW:
    return zzplay_controller_request(ctl, ZZPLAY_REQUEST_STOP);

  case ZZPLAY_CONTROL_STOP_CTRL_C:
    return zzplay_controller_request(ctl, ZZPLAY_REQUEST_QUIT);

  case ZZPLAY_CONTROL_NEXT:
    return zzplay_controller_request(ctl, ZZPLAY_REQUEST_NEXT);

  case ZZPLAY_CONTROL_PREVIOUS:
    return zzplay_controller_request(ctl, ZZPLAY_REQUEST_PREVIOUS);

  case ZZPLAY_CONTROL_CYCLE_REPEAT:
    ctl->repeat = (ZZPlayRepeat)((ctl->repeat + 1U) % 3U);
    ctl->dirty |= ZZPLAY_DIRTY_STATE;
    return 1;

  case ZZPLAY_CONTROL_TOGGLE_SHUFFLE:
    ctl->shuffle = !ctl->shuffle;
    ctl->dirty |= ZZPLAY_DIRTY_STATE;
    return 1;

  case ZZPLAY_CONTROL_VOLUME_UP:
    if (ctl->volume < ZZPLAY_VOLUME_MAX) {
      uint32_t next = ctl->volume + ZZPLAY_VOLUME_STEP;
      if (next > ZZPLAY_VOLUME_MAX) {
        next = ZZPLAY_VOLUME_MAX;
      }
      zzplay_controller_set_volume(ctl, next);
      return 1;
    }
    return 0;

  case ZZPLAY_CONTROL_VOLUME_DOWN:
    if (ctl->volume > 0U) {
      uint32_t next = (ctl->volume >= ZZPLAY_VOLUME_STEP)
                          ? (ctl->volume - ZZPLAY_VOLUME_STEP)
                          : 0U;
      zzplay_controller_set_volume(ctl, next);
      return 1;
    }
    return 0;

  case ZZPLAY_CONTROL_SEEK_FORWARD:
    return zzplay_controller_seek_to(ctl,
                                     ctl->now.elapsed_ms + ZZPLAY_SEEK_STEP_MS);

  case ZZPLAY_CONTROL_SEEK_BACK:
    {
      uint32_t target = (ctl->now.elapsed_ms > ZZPLAY_SEEK_STEP_MS)
                            ? (ctl->now.elapsed_ms - ZZPLAY_SEEK_STEP_MS)
                            : 0U;
      return zzplay_controller_seek_to(ctl, target);
    }

  case ZZPLAY_CONTROL_TOGGLE_FULLSCREEN:
    if (ctl->item_active && ctl->now.has_video) {
      ctl->fullscreen_toggles++;
      return 1;
    }
    return 0;

  case ZZPLAY_CONTROL_NONE:
  default:
    return 0;
  }
}

int zzplay_controller_item_should_end(const ZZPlayController *ctl)
{
  return ctl && ctl->request != ZZPLAY_REQUEST_NONE;
}

ZZPlayRequest zzplay_controller_take_request(ZZPlayController *ctl,
                                             int32_t *jump_index,
                                             uint32_t *seek_ms)
{
  ZZPlayRequest req;

  if (!ctl) {
    return ZZPLAY_REQUEST_NONE;
  }
  req = ctl->request;
  if (jump_index) {
    *jump_index = ctl->jump_index;
  }
  if (seek_ms) {
    *seek_ms = ctl->seek_ms;
  }
  /* The offset belongs to the request it came with: a later PLAY must
   * not inherit an old seek position. */
  ctl->request = ZZPLAY_REQUEST_NONE;
  ctl->seek_ms = 0U;
  return req;
}

void zzplay_controller_item_begin(ZZPlayController *ctl)
{
  if (!ctl) {
    return;
  }
  ctl->item_active = 1;
  ctl->paused = 0;
  ctl->engine_paused = 0;
  ctl->fullscreen_toggles = 0U;

  ctl->now.elapsed_ms = 0U;
  ctl->now.total_ms = 0U;
  ctl->now.position_exact = 0;
  ctl->now.seekable = 0;
  ctl->now.volume_supported = 0;
  ctl->now.has_video = 0;
  ctl->now.title[0] = '\0';
  ctl->now.format[0] = '\0';
  ctl->now.output[0] = '\0';
  /* Note: ctl->now.message is intentionally retained across items. */

  /* The playlist structure is unchanged at item start; the GUI updates only
   * the old and new current-row markers. */
  ctl->dirty = ZZPLAY_DIRTY_ALL & ~ZZPLAY_DIRTY_PLAYLIST;
}

void zzplay_controller_item_end(ZZPlayController *ctl)
{
  if (!ctl) {
    return;
  }
  ctl->item_active = 0;
  ctl->paused = 0;
  ctl->engine_paused = 0;
  ctl->modal_paused = 0;
  ctl->dirty |= ZZPLAY_DIRTY_STATE | ZZPLAY_DIRTY_POSITION;
}

void zzplay_controller_set_engine_paused(ZZPlayController *ctl, int paused)
{
  if (!ctl) {
    return;
  }
  ctl->engine_paused = paused ? 1 : 0;
}

int zzplay_controller_take_fullscreen_toggle(ZZPlayController *ctl)
{
  if (!ctl || ctl->fullscreen_toggles == 0U) {
    return 0;
  }
  ctl->fullscreen_toggles--;
  return 1;
}

int zzplay_controller_take_volume(ZZPlayController *ctl, uint32_t *volume)
{
  if (!ctl || !ctl->volume_changed) {
    return 0;
  }
  ctl->volume_changed = 0;
  if (volume) {
    *volume = ctl->volume;
  }
  return 1;
}

int zzplay_controller_loop_item(const ZZPlayController *ctl)
{
  return ctl && ctl->repeat == ZZPLAY_REPEAT_ONE;
}

void zzplay_controller_set_title(ZZPlayController *ctl, const char *text)
{
  if (!ctl) {
    return;
  }
  if (zzplay_safe_copy_if_changed(ctl->now.title, sizeof(ctl->now.title),
                                  text)) {
    ctl->dirty |= ZZPLAY_DIRTY_INFO;
  }
}

void zzplay_controller_set_format(ZZPlayController *ctl, const char *text)
{
  if (!ctl) {
    return;
  }
  if (zzplay_safe_copy_if_changed(ctl->now.format, sizeof(ctl->now.format),
                                  text)) {
    ctl->dirty |= ZZPLAY_DIRTY_INFO;
  }
}

void zzplay_controller_set_output(ZZPlayController *ctl, const char *text)
{
  if (!ctl) {
    return;
  }
  if (zzplay_safe_copy_if_changed(ctl->now.output, sizeof(ctl->now.output),
                                  text)) {
    ctl->dirty |= ZZPLAY_DIRTY_INFO;
  }
}

void zzplay_controller_set_message(ZZPlayController *ctl, const char *text)
{
  if (!ctl) {
    return;
  }
  if (zzplay_safe_copy_if_changed(ctl->now.message,
                                  sizeof(ctl->now.message), text)) {
    ctl->dirty |= ZZPLAY_DIRTY_INFO;
  }
}

void zzplay_controller_set_position(ZZPlayController *ctl,
                                    uint32_t elapsed_ms, int exact)
{
  int norm_exact;

  if (!ctl) {
    return;
  }
  norm_exact = exact ? 1 : 0;
  if ((elapsed_ms / 1000U) != (ctl->now.elapsed_ms / 1000U) ||
      norm_exact != ctl->now.position_exact) {
    ctl->dirty |= ZZPLAY_DIRTY_POSITION;
  }
  ctl->now.elapsed_ms = elapsed_ms;
  ctl->now.position_exact = norm_exact;
}

void zzplay_controller_set_duration(ZZPlayController *ctl,
                                    uint32_t total_ms)
{
  if (!ctl) {
    return;
  }
  if (ctl->now.total_ms != total_ms) {
    ctl->now.total_ms = total_ms;
    ctl->dirty |= ZZPLAY_DIRTY_POSITION;
  }
}

void zzplay_controller_set_capabilities(ZZPlayController *ctl,
                                        int seekable, int volume_supported,
                                        int has_video)
{
  if (!ctl) {
    return;
  }
  ctl->now.seekable = seekable ? 1 : 0;
  ctl->now.volume_supported = volume_supported ? 1 : 0;
  ctl->now.has_video = has_video ? 1 : 0;
  ctl->dirty |= ZZPLAY_DIRTY_STATE;
}

int zzplay_controller_begin_modal(ZZPlayController *ctl, ZZPlayModal op)
{
  if (!ctl || op == ZZPLAY_MODAL_NONE || ctl->modal != ZZPLAY_MODAL_NONE) {
    return 0;
  }
  ctl->modal = op;
  if (ctl->item_active && !ctl->paused) {
    ctl->paused = 1;
    ctl->modal_paused = 1;
    ctl->dirty |= ZZPLAY_DIRTY_STATE;
  }
  return 1;
}

ZZPlayModal zzplay_controller_modal_ready(const ZZPlayController *ctl)
{
  if (!ctl || ctl->modal == ZZPLAY_MODAL_NONE) {
    return ZZPLAY_MODAL_NONE;
  }
  if (!ctl->item_active || ctl->engine_paused) {
    return ctl->modal;
  }
  return ZZPLAY_MODAL_NONE;
}

void zzplay_controller_end_modal(ZZPlayController *ctl)
{
  if (!ctl) {
    return;
  }
  ctl->modal = ZZPLAY_MODAL_NONE;
  if (ctl->modal_paused) {
    ctl->modal_paused = 0;
    if (ctl->item_active) {
      ctl->paused = 0;
      ctl->dirty |= ZZPLAY_DIRTY_STATE;
    }
  }
}
