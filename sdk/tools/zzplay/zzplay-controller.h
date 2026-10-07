/* Playback controller: the one place where user input becomes player state.
 *
 * Every input surface (video window keys, player window gadgets and menus,
 * Ctrl-C) feeds this struct; every playback engine reads it. Nothing here
 * touches AmigaOS, so the whole state machine is host-testable.
 *
 * Three kinds of state live here:
 *
 *  - An item-ending request (stop, next, previous, jump, seek, play, quit).
 *    Engines poll zzplay_controller_item_should_end() and return; the
 *    application loop consumes the request with take_request() and decides
 *    what plays next.
 *  - Live controls the engine applies without ending the item: pause,
 *    volume, fullscreen toggles, repeat/shuffle.
 *  - "Now playing" text and position, written by engines and drawn by the
 *    player window, with dirty bits so the UI redraws only what changed.
 *
 * Modal operations (file requesters, About) block the task that runs them,
 * so they are gated: begin_modal() pauses an active item, and
 * modal_ready() only releases the operation once the engine has confirmed
 * it is actually holding (set_engine_paused) or no item is active.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_CONTROLLER_H
#define ZZPLAY_CONTROLLER_H

#include <stdint.h>

#include "zzplay-controls.h"
#include "zzplay-playlist.h"

#define ZZPLAY_VOLUME_MAX 100U
#define ZZPLAY_VOLUME_STEP 5U
#define ZZPLAY_SEEK_STEP_MS 10000U

typedef enum ZZPlayRequest {
  ZZPLAY_REQUEST_NONE = 0,
  /* Start the current entry (or the first) while idle. */
  ZZPLAY_REQUEST_PLAY,
  /* Restart the current item at `seek_ms`. */
  ZZPLAY_REQUEST_SEEK,
  ZZPLAY_REQUEST_NEXT,
  ZZPLAY_REQUEST_PREVIOUS,
  /* Play entry `jump_index`. */
  ZZPLAY_REQUEST_JUMP,
  /* End playback. A desktop player goes idle; a one-shot run exits. */
  ZZPLAY_REQUEST_STOP,
  /* Leave the program. */
  ZZPLAY_REQUEST_QUIT
} ZZPlayRequest;

typedef enum ZZPlayModal {
  ZZPLAY_MODAL_NONE = 0,
  ZZPLAY_MODAL_OPEN_FILES,      /* replace the playlist, then play */
  ZZPLAY_MODAL_ADD_FILES,
  ZZPLAY_MODAL_ADD_DRAWER,
  ZZPLAY_MODAL_LOAD_PLAYLIST,
  ZZPLAY_MODAL_SAVE_PLAYLIST,
  ZZPLAY_MODAL_ABOUT
} ZZPlayModal;

#define ZZPLAY_NOW_TEXT_MAX 96U
#define ZZPLAY_NOW_OUTPUT_MAX 48U
#define ZZPLAY_NOW_MESSAGE_MAX 128U

typedef struct ZZPlayNowPlaying {
  char title[ZZPLAY_NOW_TEXT_MAX];    /* "Artist - Title" or file name */
  char format[ZZPLAY_NOW_TEXT_MAX];   /* "MP3 44100 Hz stereo 128 kbps" */
  char output[ZZPLAY_NOW_OUTPUT_MAX]; /* "MHI mhizz9000.library", "AHI unit 0" */
  char message[ZZPLAY_NOW_MESSAGE_MAX]; /* last notice or error, "" if none */
  uint32_t elapsed_ms;
  uint32_t total_ms;                  /* 0 when unknown */
  int position_exact;                 /* 0: show '~' */
  int seekable;
  int volume_supported;               /* the active output honours volume */
  int has_video;
} ZZPlayNowPlaying;

/* Dirty bits for the UI. */
#define ZZPLAY_DIRTY_INFO 0x0001U      /* title/format/output/message */
#define ZZPLAY_DIRTY_POSITION 0x0002U  /* elapsed/total */
#define ZZPLAY_DIRTY_STATE 0x0004U     /* playing/paused/idle, repeat,
                                           shuffle, volume, seekable */
#define ZZPLAY_DIRTY_PLAYLIST 0x0008U  /* structural entry changes */
#define ZZPLAY_DIRTY_CURRENT 0x0010U   /* current-entry highlight only */
#define ZZPLAY_DIRTY_ALL 0x001FU

typedef struct ZZPlayController {
  /* Item-ending request; see take_request(). */
  ZZPlayRequest request;
  int32_t jump_index;
  uint32_t seek_ms;

  /* Live controls. */
  int item_active;          /* an engine is playing an item */
  int paused;               /* what the user wants */
  int engine_paused;        /* what the engine confirms it is doing */
  uint32_t fullscreen_toggles;
  ZZPlayRepeat repeat;
  int shuffle;
  uint32_t volume;          /* 0..ZZPLAY_VOLUME_MAX */
  int volume_changed;
  /* The settings window changed device settings; the app clears it. */
  int settings_changed;

  /* Modal gate. */
  ZZPlayModal modal;
  int modal_paused;         /* the gate paused the item and will resume it */

  ZZPlayNowPlaying now;
  uint32_t dirty;
} ZZPlayController;

void zzplay_controller_init(ZZPlayController *ctl, uint32_t volume,
                            ZZPlayRepeat repeat, int shuffle);

/* Apply one input action:
 *   TOGGLE_PAUSE   pause/resume an active item; PLAY request when idle
 *   STOP_KEY/STOP_WINDOW  STOP request
 *   STOP_CTRL_C    QUIT request
 *   NEXT/PREVIOUS  NEXT/PREVIOUS request
 *   CYCLE_REPEAT   OFF -> ONE -> ALL -> OFF
 *   TOGGLE_SHUFFLE shuffle on/off
 *   VOLUME_UP/DOWN +-ZZPLAY_VOLUME_STEP, clamped
 *   SEEK_FORWARD/BACK  +-ZZPLAY_SEEK_STEP_MS from elapsed, only for an
 *                  active seekable item; back clamps at 0, forward past a
 *                  known end becomes NEXT
 *   TOGGLE_FULLSCREEN  counted only when the item has video
 * Returns 1 when the action changed anything. */
int zzplay_controller_apply(ZZPlayController *ctl,
                            ZZPlayControlAction action);

/* Post a request. QUIT outranks STOP, which outranks everything else; among
 * the rest the latest wins. Returns 1 when stored. */
int zzplay_controller_request(ZZPlayController *ctl, ZZPlayRequest request);
int zzplay_controller_jump(ZZPlayController *ctl, int32_t index);
/* Only for an active seekable item; a target at or past a known duration
 * becomes a NEXT request. */
int zzplay_controller_seek_to(ZZPlayController *ctl, uint32_t ms);
void zzplay_controller_set_volume(ZZPlayController *ctl, uint32_t volume);
void zzplay_controller_set_repeat(ZZPlayController *ctl,
                                  ZZPlayRepeat repeat);
void zzplay_controller_set_shuffle(ZZPlayController *ctl, int shuffle);

/* Any pending request ends the current item. */
int zzplay_controller_item_should_end(const ZZPlayController *ctl);

/* Return and clear the pending request (NONE when there is none). */
ZZPlayRequest zzplay_controller_take_request(ZZPlayController *ctl,
                                             int32_t *jump_index,
                                             uint32_t *seek_ms);

/* --- Engine side --- */

/* An item starts: not paused, position and per-item info cleared (the
 * message line is kept), everything dirty. */
void zzplay_controller_item_begin(ZZPlayController *ctl);
/* The item ended: idle, unpaused, any modal pause released. */
void zzplay_controller_item_end(ZZPlayController *ctl);
void zzplay_controller_set_engine_paused(ZZPlayController *ctl, int paused);
/* Consume one pending fullscreen toggle. */
int zzplay_controller_take_fullscreen_toggle(ZZPlayController *ctl);
/* Consume a volume change; returns 1 and the new volume when one is
 * pending. */
int zzplay_controller_take_volume(ZZPlayController *ctl, uint32_t *volume);
/* The engine should loop the current item itself (repeat track). */
int zzplay_controller_loop_item(const ZZPlayController *ctl);

void zzplay_controller_set_title(ZZPlayController *ctl, const char *text);
void zzplay_controller_set_format(ZZPlayController *ctl, const char *text);
void zzplay_controller_set_output(ZZPlayController *ctl, const char *text);
void zzplay_controller_set_message(ZZPlayController *ctl, const char *text);
/* Marks POSITION dirty only when the displayed second (or exactness)
 * changes: engines call this every pass. */
void zzplay_controller_set_position(ZZPlayController *ctl,
                                    uint32_t elapsed_ms, int exact);
void zzplay_controller_set_duration(ZZPlayController *ctl,
                                    uint32_t total_ms);
void zzplay_controller_set_capabilities(ZZPlayController *ctl,
                                        int seekable, int volume_supported,
                                        int has_video);

/* --- Modal gate --- */

/* Queue a modal operation. Returns 0 when one is already pending. Pauses
 * an active, unpaused item and remembers to resume it. */
int zzplay_controller_begin_modal(ZZPlayController *ctl, ZZPlayModal op);
/* The pending operation once it may run (no active item, or the engine
 * confirmed the pause), else ZZPLAY_MODAL_NONE. */
ZZPlayModal zzplay_controller_modal_ready(const ZZPlayController *ctl);
/* The operation finished: clear it and resume the item if the gate paused
 * it. */
void zzplay_controller_end_modal(ZZPlayController *ctl);

#endif /* ZZPLAY_CONTROLLER_H */
