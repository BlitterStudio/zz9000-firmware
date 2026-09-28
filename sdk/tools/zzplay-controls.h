/* Host-testable input normalization for zzplay.
 *
 * One binding table serves both the video window and the standalone-MP3
 * status window, so R7's "stable state machine across MPEG and MP3" is a
 * property of the code rather than of two implementations agreeing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_CONTROLS_H
#define ZZPLAY_CONTROLS_H

#include "zzplay-core.h"

typedef enum ZZPlayControlAction {
  ZZPLAY_CONTROL_NONE = 0,
  ZZPLAY_CONTROL_TOGGLE_PAUSE,
  ZZPLAY_CONTROL_STOP_CTRL_C,
  ZZPLAY_CONTROL_STOP_WINDOW,
  ZZPLAY_CONTROL_STOP_KEY,
  ZZPLAY_CONTROL_TOGGLE_FULLSCREEN,
  ZZPLAY_CONTROL_TOGGLE_LOOP
} ZZPlayControlAction;

/* Raw input observed in one poll. Several can be true at once; the action is
 * resolved by priority so a close gadget clicked in the same poll as a key
 * still wins. */
typedef struct ZZPlayControlInput {
  int ctrl_c;
  int window_close;
  /* Vanilla key code, 0 when none. */
  unsigned key;
} ZZPlayControlInput;

/* Map one vanilla key to its action, or ZZPLAY_CONTROL_NONE. Case-insensitive
 * for letters. */
ZZPlayControlAction zzplay_control_action_from_key(unsigned key);

/* Resolve one poll's worth of input. */
ZZPlayControlAction zzplay_control_resolve(const ZZPlayControlInput *input);

/* Retained for the existing call sites and tests. */
ZZPlayControlAction zzplay_control_action(int ctrl_c,
                                          int window_close,
                                          int toggle_pause);
ZZPlayStopReason zzplay_control_stop_reason_from_action(
    ZZPlayControlAction action);

/* True when the action ends playback. */
int zzplay_control_is_stop(ZZPlayControlAction action);

#endif /* ZZPLAY_CONTROLS_H */
