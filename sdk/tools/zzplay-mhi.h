/* Optional runtime MHI output for standalone Layer III playback.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_MHI_H
#define ZZPLAY_MHI_H

#include <stdint.h>
#include <stdio.h>

#define ZZPLAY_MHI_BUFFER_COUNT 2U

typedef enum ZZPlayMHIStatus {
  ZZPLAY_MHI_OK = 0,
  ZZPLAY_MHI_MISSING,
  ZZPLAY_MHI_UNSUPPORTED,
  ZZPLAY_MHI_BUSY,
  ZZPLAY_MHI_NO_MEMORY,
  ZZPLAY_MHI_IO_ERROR,
  ZZPLAY_MHI_STOPPED
} ZZPlayMHIStatus;

typedef struct ZZPlayMHISink {
  void *library;
  void *decoder;
  int signal_bit;
  uint32_t signal_mask;
  void *buffers[ZZPLAY_MHI_BUFFER_COUNT];
  uint64_t input_bytes;
  uint8_t playing;
  uint8_t paused;
} ZZPlayMHISink;

typedef int (*ZZPlayMHIStopRequested)(void *user);
/* Non-zero holds playback through the real MHI pause primitive without
 * ending the session. */
typedef int (*ZZPlayMHIPaused)(void *user);

ZZPlayMHIStatus zzplay_mhi_acquire(ZZPlayMHISink *sink);
ZZPlayMHIStatus zzplay_mhi_play_file(
    ZZPlayMHISink *sink,
    FILE *file,
    ZZPlayMHIStopRequested stop_requested,
    ZZPlayMHIPaused paused,
    void *user);
int zzplay_mhi_pause(ZZPlayMHISink *sink);
int zzplay_mhi_resume(ZZPlayMHISink *sink);
void zzplay_mhi_stop(ZZPlayMHISink *sink);
void zzplay_mhi_release(ZZPlayMHISink *sink);
const char *zzplay_mhi_status_name(ZZPlayMHIStatus status);

#endif /* ZZPLAY_MHI_H */
