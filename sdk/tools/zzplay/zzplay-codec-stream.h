/* On-card FLAC and Ogg Vorbis playback for zzplay: the card decodes and
 * plays S16LE straight into the ZZ9000AX output, with accelerated decode
 * + AHI as the fallback. Driven by the playback controller like every
 * other engine (zzplay-engine.h).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_CODEC_STREAM_H
#define ZZPLAY_CODEC_STREAM_H

#include "zzplay-engine.h"

/* Play one native FLAC file. AUTO tries on-card playback (S16LE; the pump
 * consumes the PCM) and falls back to accelerated decode + AHI when the
 * card refuses that output. Strict AUDIO=AX does not fall back. Seeking
 * is unsupported (the decoder starts from the stream header). */
ZZPlayEngineResult zzplay_flac_run(const ZZPlayEngineRun *run);

/* Play one Ogg Vorbis file (a single logical stream) with the FLAC output
 * policy and no seeking. A chained or multiplexed file plays its first
 * link, then fails with an explicit unsupported-stream error. */
ZZPlayEngineResult zzplay_vorbis_run(const ZZPlayEngineRun *run);

#endif /* ZZPLAY_CODEC_STREAM_H */
