/* Software mpega.library backend for card-less MP3 playback.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_MPEGA_H
#define ZZPLAY_MPEGA_H

#include "zzplay-engine.h"

/* Opens mpega.library at playback time. The decoder receives input through a
 * bitstream hook, so file and http(s) sources follow the same path. */
ZZPlayEngineResult zzplay_mpega_run(const ZZPlayEngineRun *run);

#endif /* ZZPLAY_MPEGA_H */
