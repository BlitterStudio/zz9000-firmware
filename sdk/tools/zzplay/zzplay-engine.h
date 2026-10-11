/* Playback engine interface for zzplay: one function per media kind
 * (zzplay_engines[] in zzplay.c) plays one item and reports how it ended.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_ENGINE_H
#define ZZPLAY_ENGINE_H

#include "zzplay-controller.h"
#include "zzplay-options.h"
#include "zzplay-prefs.h"
#include "zzplay-probe.h"

#include <stdint.h>

/* What an engine run ended with. The application loop consumes any
 * controller request itself (that is how STOP/QUIT/NEXT/... are routed),
 * so these only distinguish "ran to its own end" from "a request ended
 * it" and "it could not play". */
typedef enum ZZPlayEngineResult {
  ZZPLAY_ENGINE_EOF = 0,   /* the item completed on its own */
  ZZPLAY_ENGINE_STOPPED,   /* a controller request ended the item */
  ZZPLAY_ENGINE_QUIT,      /* the engine observed a quit by itself */
  ZZPLAY_ENGINE_FAILED     /* the item could not be played; already reported */
} ZZPlayEngineResult;

/* One engine invocation. `path` is private storage that outlives the run
 * (the playlist must stay editable while an item plays). `probe` is the
 * result of zzplay_probe_media_file() on that path. `seek_ms` starts the
 * first pass at that position when non-zero (MP3 only). */
typedef struct ZZPlayEngineRun {
  ZZPlayController *ctl;
  const char *path;
  const ZZPlayProbeInfo *probe;
  const ZZPlayOptions *options;
  const ZZPlayPrefs *prefs;
  /* Metadata and length read during probe; the MP3 engine owns only its
   * streaming handles. */
  uint64_t audio_start;
  uint32_t trailer_bytes;
  uint64_t file_size;
  uint32_t seek_ms;
  /* Called regularly from every wait loop so the GUI stays live while
   * the engine blocks; may be NULL when there is nothing to pump. */
  void (*pump)(void *user);
  void *pump_user;
} ZZPlayEngineRun;

typedef ZZPlayEngineResult (*ZZPlayEngineFn)(const ZZPlayEngineRun *run);

#endif /* ZZPLAY_ENGINE_H */
