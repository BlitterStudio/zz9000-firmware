/* Host-testable input normalization for zzplay.
 *
 * One binding table serves the video window and the player window, so the
 * same key does the same thing whichever window has focus. Keys only map
 * to actions here; what an action does to playback is the controller's
 * business (zzplay-controller.h).
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
  /* Repeat off -> repeat track -> repeat playlist -> off. */
  ZZPLAY_CONTROL_CYCLE_REPEAT,
  ZZPLAY_CONTROL_TOGGLE_SHUFFLE,
  ZZPLAY_CONTROL_NEXT,
  ZZPLAY_CONTROL_PREVIOUS,
  ZZPLAY_CONTROL_VOLUME_UP,
  ZZPLAY_CONTROL_VOLUME_DOWN,
  ZZPLAY_CONTROL_SEEK_FORWARD,
  ZZPLAY_CONTROL_SEEK_BACK
} ZZPlayControlAction;

/* Raw key codes (IDCMP_RAWKEY, key-down, qualifiers ignored) for the keys
 * that have no vanilla translation. */
#define ZZPLAY_RAWKEY_CURSOR_UP 0x4CU
#define ZZPLAY_RAWKEY_CURSOR_DOWN 0x4DU
#define ZZPLAY_RAWKEY_CURSOR_RIGHT 0x4EU
#define ZZPLAY_RAWKEY_CURSOR_LEFT 0x4FU

/* Raw input observed in one poll. Several can be true at once; the action is
 * resolved by priority so a close gadget clicked in the same poll as a key
 * still wins. */
typedef struct ZZPlayControlInput {
  int ctrl_c;
  int window_close;
  /* Vanilla key code, 0 when none. */
  unsigned key;
  /* Raw key-down code (bit 7 clear), 0 when none. Consulted only when
   * `key` maps to nothing. */
  unsigned rawkey;
} ZZPlayControlInput;

/* Map one vanilla key to its action, or ZZPLAY_CONTROL_NONE. Case-insensitive
 * for letters:
 *   Space pause, Escape/Q stop, F fullscreen, L repeat, S shuffle,
 *   N next, P previous, '+'/'=' volume up, '-' volume down. */
ZZPlayControlAction zzplay_control_action_from_key(unsigned key);

/* Map one raw key-down code: cursor right/left seek, up/down volume.
 * Key-up codes (bit 7 set) map to nothing. */
ZZPlayControlAction zzplay_control_action_from_rawkey(unsigned code);

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
