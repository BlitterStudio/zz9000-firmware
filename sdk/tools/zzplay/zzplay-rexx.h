/* ZZPlay's ARexx port.
 *
 * A public message port, "ZZPLAY" (or "ZZPLAY.1", "ZZPLAY.2", ... when
 * another ZZPlay already owns that name), that accepts the commands in
 * zzplay-rexx-commands.h. It is polled from the same places as the player
 * window, never blocking, so commands work while an item plays, while the
 * desktop player is idle, and in a one-shot run without a window.
 *
 * Commands that expand paths (ADD, OPEN) wait behind the controller's modal
 * gate; the sending script waits for its reply meanwhile, and later
 * messages stay queued, so commands run in the order they were sent.
 *
 * AmigaOS-only. SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_REXX_H
#define ZZPLAY_REXX_H

#include <stdint.h>

#include "zzplay-controller.h"
#include "zzplay-playlist.h"

typedef struct ZZPlayRexxContext {
  ZZPlayController *controller;
  ZZPlayPlaylist *playlist;
  /* The playing item's own path copy (outlives the port). */
  const char *item_path;
  const char *version;
} ZZPlayRexxContext;

/* Open the port. The context is copied; what it points to must outlive the
 * port. Returns 0 when there is no ARexx (rexxsyslib.library) or no free
 * port name; ZZPlay then runs without one. */
int zzplay_rexx_open(const ZZPlayRexxContext *context);

/* Remove the port and fail every command still waiting. Safe when not
 * open. */
void zzplay_rexx_close(void);

/* The port's name, or NULL when it is not open. */
const char *zzplay_rexx_port_name(void);

/* Signal to Wait() on while idle, 0 when not open. */
uint32_t zzplay_rexx_signal_mask(void);

/* Execute and reply to queued commands. Cheap when there are none. */
void zzplay_rexx_poll(void);

#endif /* ZZPLAY_REXX_H */
