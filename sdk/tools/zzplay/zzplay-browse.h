/* Service browser window for ZZPlay (GadTools, AmigaOS 3.0+).
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_BROWSE_H
#define ZZPLAY_BROWSE_H

#include <stdint.h>

#include "zzplay-controller.h"
#include "zzplay-playlist.h"
#include "zzplay-prefs.h"

struct Screen;

/* Open the service browser window on the given screen. `prefs` is where
 * the window's place and size are remembered when it closes. */
int zzplay_browse_open(struct Screen *screen, ZZPlayController *ctl,
                       ZZPlayPlaylist *playlist, ZZPlayPrefs *prefs);

/* Close the Browse window. A standalone service Settings window remains open. */
void zzplay_browse_close(void);

/* Close Browse, Settings and sign-in windows, and their worker. */
void zzplay_browse_shutdown(void);

/* True when any service window is open. */
int zzplay_browse_has_windows(void);

/* Open Browse on `scheme`, or switch its open window to that service. An empty
 * scheme opens the current service or the first installed service. */
int zzplay_browse_open_service(struct Screen *screen, ZZPlayController *ctl,
                               ZZPlayPlaylist *playlist, ZZPlayPrefs *prefs,
                               const char *scheme);

/* Open standalone settings for `scheme`, bringing an existing window for that
 * service forward. Opening another service replaces the existing form. */
int zzplay_browse_settings_open(struct Screen *screen, ZZPlayController *ctl,
                                ZZPlayPlaylist *playlist, ZZPlayPrefs *prefs,
                                const char *scheme);

/* True when the Browse window is open. */
int zzplay_browse_is_open(void);

/* Signal mask for Wait() while idle: Browse window, service forms and worker. */
uint32_t zzplay_browse_signal_mask(void);

/* Poll IDCMP events for service windows and worker replies. */
void zzplay_browse_poll(void);

#endif /* ZZPLAY_BROWSE_H */
