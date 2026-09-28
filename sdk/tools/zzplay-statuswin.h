/* Small Intuition player window for playback with no video (U6, D3).
 *
 * Standalone MP3 has no PIP window, so U5 had to defer pause/resume: there
 * was nowhere for a keypress to land. This gives that path the same control
 * surface and key bindings as the video window, plus the track information
 * and position a minimal player is expected to show.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_STATUSWIN_H
#define ZZPLAY_STATUSWIN_H

#include <stdint.h>

#include "zzplay-controls.h"

typedef struct ZZPlayStatusWindow {
  void *window;        /* struct Window * */
  char name[64];       /* file name, no path */
  char backend[16];
  char title[128];
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t bitrate_kbps;
  uint32_t elapsed_ms;
  uint32_t total_ms;   /* 0 when unknown */
  int position_exact;
  int paused;
  int stop;
  int loop;
  int dirty;
  /* Layout derived from the screen font rather than assumed, so a taller
   * font is not clipped and a shorter one does not leave a gap. */
  int line_height;
  int baseline;
  int text_left;
} ZZPlayStatusWindow;

/* Opens on the default public screen. Returns 0 if a window could not be
 * opened; callers must still work headless in that case. `total_ms` may be
 * 0 when the duration is unknown (e.g. a VBR file). */
int zzplay_statuswin_open(ZZPlayStatusWindow *status, const char *path,
                          uint32_t sample_rate, uint32_t channels,
                          uint32_t bitrate_kbps, uint32_t total_ms);
void zzplay_statuswin_close(ZZPlayStatusWindow *status);

/* Drain input, update state, and redraw when something changed. Also
 * honours Ctrl-C so a shell launch behaves the same with or without the
 * window. */
ZZPlayControlAction zzplay_statuswin_poll(ZZPlayStatusWindow *status);

void zzplay_statuswin_set_backend(ZZPlayStatusWindow *status,
                                  const char *backend);
void zzplay_statuswin_set_position(ZZPlayStatusWindow *status,
                                   uint32_t elapsed_ms, int exact);

/* Estimate a CBR duration from the file size. Returns 0 when unknown. */
uint32_t zzplay_statuswin_duration_ms(const char *path,
                                      uint32_t bitrate_kbps);

/* The ZZPlayMP3Controls callbacks live in zzplay.c so they can also consult
 * the SIGINT flag libnix raises for a Ctrl-C taken during I/O. */

#endif /* ZZPLAY_STATUSWIN_H */
