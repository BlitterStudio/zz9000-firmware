/*
 * Card-local ZZ9000AX media-session audio sink.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZPLAY_AX_H
#define ZZPLAY_AX_H

#include "zz9k/abi.h"

#include <stdint.h>

typedef struct ZZPlayAXControlOps {
  int (*bind)(void *user, uint32_t session, uint32_t flags,
              ZZ9KMediaSessionAudioResult *result);
  int (*unbind)(void *user, uint32_t session, uint32_t flags,
                ZZ9KMediaSessionAudioResult *result);
  int (*status)(void *user, uint32_t session, uint32_t page,
                uint32_t flags,
                ZZ9KMediaSessionStatusResult *result);
} ZZPlayAXControlOps;

typedef struct ZZPlayAXSink {
  const ZZPlayAXControlOps *ops;
  void *user;
  uint32_t session;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t frame_bytes;
  uint32_t underruns;
  uint64_t retired_frames;
  uint64_t dma_queued_frames;
  uint64_t staged_frames;
  ZZ9KMediaSessionAudioResult audio;
  uint8_t prepared;
  uint8_t bound;
  uint8_t paused;
  uint8_t draining;
  uint8_t drained;
} ZZPlayAXSink;

void zzplay_ax_init(ZZPlayAXSink *sink, uint32_t session,
                    const ZZPlayAXControlOps *ops, void *user);
int zzplay_ax_prepare(ZZPlayAXSink *sink,
                      const ZZ9KMediaSessionAudioResult *audio);
int zzplay_ax_play(ZZPlayAXSink *sink);
int zzplay_ax_poll(ZZPlayAXSink *sink);
int zzplay_ax_pause(ZZPlayAXSink *sink);
int zzplay_ax_resume(ZZPlayAXSink *sink);
int zzplay_ax_begin_drain(ZZPlayAXSink *sink);
int zzplay_ax_stop(ZZPlayAXSink *sink);
int zzplay_ax_close(ZZPlayAXSink *sink);
uint64_t zzplay_ax_played_frames(const ZZPlayAXSink *sink);
uint64_t zzplay_ax_queued_frames(const ZZPlayAXSink *sink);
uint64_t zzplay_ax_clock_pts(const ZZPlayAXSink *sink,
                             uint64_t origin_pts);
int zzplay_ax_drained(const ZZPlayAXSink *sink);

#endif /* ZZPLAY_AX_H */
