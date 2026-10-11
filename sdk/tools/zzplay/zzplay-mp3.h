/* Standalone MP3 playback engine for zzplay: MP3 through MHI or
 * accelerated decode + AHI (the shared compressed-audio engine,
 * zzplay-codec.h). FLAC and Ogg Vorbis are zzplay-codec-stream.h.
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

#include "zzplay-engine.h"

/* Play one standalone MP3 file. Backend, AHI unit and MHI driver come
 * from `prefs` resolved with `options` (zzplay_prefs_requested_backend);
 * a strict requested backend that cannot play fails instead of falling
 * back. */
ZZPlayEngineResult zzplay_mp3_run(const ZZPlayEngineRun *run);

#endif /* ZZPLAY_MP3_H */
