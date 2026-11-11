/* Service browser window for ZZPlay (GadTools, AmigaOS 3.0+).
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_BROWSE_H
#define ZZPLAY_BROWSE_H

#include <stdint.h>

#include "zzplay-controller.h"
#include "zzplay-playlist.h"

struct Screen;

/* Open the service browser window on the given screen. */
int zzplay_browse_open(struct Screen *screen, ZZPlayController *ctl,
                       ZZPlayPlaylist *playlist);

/* Close the Browse window and any open settings form. */
void zzplay_browse_close(void);

/* True when the Browse window is open. */
int zzplay_browse_is_open(void);

/* Signal mask for Wait() while idle: Browse window and its worker. */
uint32_t zzplay_browse_signal_mask(void);

/* Poll IDCMP events for Browse window (and Settings window) and worker replies. */
void zzplay_browse_poll(void);

#endif /* ZZPLAY_BROWSE_H */
