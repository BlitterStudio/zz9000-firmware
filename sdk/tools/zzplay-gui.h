/* ZZPlay player window and settings window (GadTools, AmigaOS 3.0+).
 *
 * The player window is the desktop surface: now-playing text, position
 * slider, transport buttons, volume, the playlist, menus, keyboard
 * shortcuts and Workbench drag-and-drop (AppWindow). The settings window
 * edits the audio device settings (zzplay-prefs.h) with Save/Use/Cancel.
 *
 * The GUI never plays anything itself. It turns input into controller
 * requests and playlist edits, and redraws from the controller's dirty
 * bits. It is polled, never blocking, from both the idle loop and the
 * playback engines; the only blocking work it does (ASL requesters,
 * About) runs through the controller's modal gate, so an engine is
 * always paused first.
 *
 * AmigaOS-only. SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_GUI_H
#define ZZPLAY_GUI_H

#include <stdint.h>

#include "zzplay-controller.h"
#include "zzplay-playlist.h"
#include "zzplay-prefs.h"

typedef struct ZZPlayGuiContext {
  ZZPlayController *controller;
  ZZPlayPlaylist *playlist;
  /* Live settings. "Use"/"Save" in the settings window update this struct
   * (and ENV:/ENVARC:) and set controller->settings_changed. Volume,
   * repeat, shuffle, window position and last drawer are kept current
   * here too, so the app can remember them with
   * zzplay_prefs_save_session(). */
  ZZPlayPrefs *prefs;
  /* Version string for About, e.g. "ZZPlay 0.6 (07.10.2026)". */
  const char *version;
} ZZPlayGuiContext;

/* Open the player window on the default public screen. The context struct
 * is copied; the controller, playlist and prefs it points to must outlive
 * the GUI. Returns 0 when the window cannot open (callers keep working
 * headless). */
int zzplay_gui_open(const ZZPlayGuiContext *context);

/* Close every GUI window, recording the player window position in prefs.
 * Safe when not open. */
void zzplay_gui_close(void);

int zzplay_gui_is_open(void);

/* Signals to Wait() on while idle: window ports and the AppWindow port. */
uint32_t zzplay_gui_signal_mask(void);

/* Drain all pending GUI input, apply it to the controller and playlist,
 * run a modal operation if the gate releases one, and redraw what changed.
 * Returns immediately when there is nothing to do. */
void zzplay_gui_poll(void);

/* Report a message to the user: shown in the player window's message line
 * when it is open. Returns 0 when there is no window to show it in. */
int zzplay_gui_report(const char *message);

#endif /* ZZPLAY_GUI_H */
