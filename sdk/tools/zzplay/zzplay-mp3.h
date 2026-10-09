/* Standalone compressed-audio playback engine for zzplay: MP3 through MHI
 * or accelerated decode + AHI; native FLAC and Ogg Vorbis on the card
 * (S16LE into the ZZ9000AX output) with AHI as the fallback.
 *
 * The engine is driven by the playback controller (zzplay-controller.h):
 * it polls item-ending requests, applies live controls (pause, volume),
 * reports position/capabilities/output text and calls back into the
 * application pump from every wait loop so the player window stays live.
 * It never opens a window of its own.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_MP3_H
#define ZZPLAY_MP3_H

#include "zzplay-controller.h"
#include "zzplay-options.h"
#include "zzplay-prefs.h"
#include "zzplay-probe.h"

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

/* Play one standalone MP3 file. Backend, AHI unit and MHI driver come
 * from `prefs` resolved with `options` (zzplay_prefs_requested_backend);
 * a strict requested backend that cannot play fails instead of falling
 * back. */
ZZPlayEngineResult zzplay_mp3_run(const ZZPlayEngineRun *run);

/* Play one native FLAC file. AUTO tries on-card playback (S16LE; the pump
 * consumes the PCM) and falls back to accelerated decode + AHI when the
 * card refuses that output. Strict AUDIO=AX does not fall back. Seeking
 * is unsupported (the decoder starts from the stream header). */
ZZPlayEngineResult zzplay_flac_run(const ZZPlayEngineRun *run);

/* Play one Ogg Vorbis file (a single logical stream) with the FLAC output
 * policy and no seeking. A chained or multiplexed file plays its first
 * link, then fails with an explicit unsupported-stream error. */
ZZPlayEngineResult zzplay_vorbis_run(const ZZPlayEngineRun *run);

#endif /* ZZPLAY_MP3_H */
