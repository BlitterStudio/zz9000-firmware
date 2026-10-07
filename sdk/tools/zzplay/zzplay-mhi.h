/* Optional runtime MHI output for standalone Layer III playback.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_MHI_H
#define ZZPLAY_MHI_H

#include <stdint.h>
#include <stdio.h>

#define ZZPLAY_MHI_BUFFER_COUNT 2U
#define ZZPLAY_MHI_DRIVER_MAX 32U

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
  /* Driver file name ("mhizz9000.library"), for display. */
  char driver[ZZPLAY_MHI_DRIVER_MAX];
  uint8_t playing;
  uint8_t paused;
  /* The driver advertised MHIQ_VOLUME_CONTROL. */
  uint8_t volume_supported;
  /* The driver is mhizz9000.library, whose zero-length EOF marker is a
   * private extension other MHI drivers must not be fed. */
  uint8_t eof_marker;
} ZZPlayMHISink;

typedef int (*ZZPlayMHIStopRequested)(void *user);
/* Non-zero holds playback through the real MHI pause primitive without
 * ending the session. */
typedef int (*ZZPlayMHIPaused)(void *user);

/* Open "MHI/<driver>" (LIBS:MHI/ is implied). NULL or an empty name
 * selects the default mhizz9000.library driver. */
ZZPlayMHIStatus zzplay_mhi_acquire(ZZPlayMHISink *sink,
                                   const char *driver);
/* Feed the whole file from its current position. Returns ZZPLAY_MHI_OK
 * when the decoder drained the input, ZZPLAY_MHI_STOPPED when
 * `stop_requested` asked for the end of the item. */
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
/* Set the decoder volume (0..100 percent). Returns 0 when the driver did
 * not advertise MHIQ_VOLUME_CONTROL; mhizz9000.library does not. */
int zzplay_mhi_set_volume(ZZPlayMHISink *sink, uint32_t percent);
const char *zzplay_mhi_driver_name(const ZZPlayMHISink *sink);
const char *zzplay_mhi_status_name(ZZPlayMHIStatus status);

#endif /* ZZPLAY_MHI_H */
