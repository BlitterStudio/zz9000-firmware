/*
 * Completion-accounted audio clock with a bounded presentation estimate.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZPLAY_AUDIO_CLOCK_H
#define ZZPLAY_AUDIO_CLOCK_H

#include <stdint.h>

typedef enum ZZPlayAudioClockState {
  ZZPLAY_AUDIO_CLOCK_CLOSED = 0,
  ZZPLAY_AUDIO_CLOCK_PREBUFFERING,
  ZZPLAY_AUDIO_CLOCK_PLAYING,
  ZZPLAY_AUDIO_CLOCK_PAUSED,
  ZZPLAY_AUDIO_CLOCK_DRAINING,
  ZZPLAY_AUDIO_CLOCK_STOPPED
} ZZPlayAudioClockState;

typedef struct ZZPlayAudioClock {
  uint32_t sample_rate;
  uint32_t queue_limit_frames;
  uint64_t submitted_frames;
  uint64_t played_frames;
  uint64_t discarded_frames;
  uint64_t queued_frames;
  uint64_t presentation_frames;
  uint64_t presentation_last_us;
  uint32_t presentation_fraction;
  uint32_t underruns;
  ZZPlayAudioClockState state;
  uint8_t presentation_active;
} ZZPlayAudioClock;

void zzplay_audio_clock_prepare(ZZPlayAudioClock *clock,
                                uint32_t sample_rate,
                                uint32_t queue_limit_frames);
int zzplay_audio_clock_queue(ZZPlayAudioClock *clock, uint32_t frames);
int zzplay_audio_clock_play(ZZPlayAudioClock *clock);
int zzplay_audio_clock_pause(ZZPlayAudioClock *clock);
int zzplay_audio_clock_resume(ZZPlayAudioClock *clock);
int zzplay_audio_clock_begin_drain(ZZPlayAudioClock *clock);
int zzplay_audio_clock_complete(ZZPlayAudioClock *clock,
                                uint32_t submitted_frames,
                                uint32_t played_frames);
void zzplay_audio_clock_start_presentation(ZZPlayAudioClock *clock,
                                           uint64_t now_us);
void zzplay_audio_clock_update_presentation(ZZPlayAudioClock *clock,
                                            uint64_t now_us);
void zzplay_audio_clock_note_underrun(ZZPlayAudioClock *clock);
void zzplay_audio_clock_retract_underrun(ZZPlayAudioClock *clock);
int zzplay_audio_clock_drained(const ZZPlayAudioClock *clock);
void zzplay_audio_clock_stop(ZZPlayAudioClock *clock);
void zzplay_audio_clock_close(ZZPlayAudioClock *clock);
uint64_t zzplay_audio_clock_pts(const ZZPlayAudioClock *clock,
                                uint64_t origin_pts);
uint64_t zzplay_audio_clock_presentation_pts(
    const ZZPlayAudioClock *clock, uint64_t origin_pts);

/* Card output clock. The transmitter is 48 kHz stereo S16; one played
 * frame is one DMA sample, not a source frame and not a file byte.
 * `written_frames` is what has been committed to the transmitter,
 * `lead_frames` is still queued ahead of the DMA, and `origin_frames`
 * is the played count when this item (or seek) started. Bitrate, ID3
 * size, a Xing header, channel count and samples-per-frame do not
 * enter: those are why a byte clock runs at a different speed on
 * every file. */
#define ZZPLAY_CARD_OUTPUT_RATE 48000U
uint32_t zzplay_output_elapsed_ms(uint64_t written_frames,
                                  uint64_t lead_frames,
                                  uint64_t origin_frames,
                                  uint32_t output_rate);

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
struct ZZ9KContext;
/* Read the card clock and return elapsed milliseconds for this item.
 * The first successful read anchors origin_frames. Returns 0 when the
 * card has no clock yet; the caller keeps its previous position. */
int zzplay_output_clock_elapsed(struct ZZ9KContext *ctx,
                                uint64_t *origin_frames, int *armed,
                                uint32_t base_ms, uint32_t *elapsed_ms);
#endif

#endif /* ZZPLAY_AUDIO_CLOCK_H */
