/* Player lifecycle and resource ownership for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_CORE_H
#define ZZPLAY_CORE_H

#include <stdint.h>

typedef enum ZZPlayState {
  ZZPLAY_STATE_PREPARING = 0,
  ZZPLAY_STATE_PREBUFFERING,
  ZZPLAY_STATE_PLAYING,
  ZZPLAY_STATE_PAUSED,
  ZZPLAY_STATE_DRAINING,
  ZZPLAY_STATE_LOOPING,
  ZZPLAY_STATE_STOPPED,
  ZZPLAY_STATE_ERROR
} ZZPlayState;

typedef enum ZZPlayStopReason {
  ZZPLAY_STOP_NONE = 0,
  ZZPLAY_STOP_CTRL_C,
  ZZPLAY_STOP_WINDOW_CLOSE,
  ZZPLAY_STOP_EOF
} ZZPlayStopReason;

typedef enum ZZPlayFailure {
  ZZPLAY_FAILURE_NONE = 0,
  ZZPLAY_FAILURE_INVALID_INPUT,
  ZZPLAY_FAILURE_UNSUPPORTED_BOARD,
  ZZPLAY_FAILURE_P96,
  ZZPLAY_FAILURE_PIP,
  ZZPLAY_FAILURE_SDK,
  ZZPLAY_FAILURE_CAPABILITY,
  ZZPLAY_FAILURE_ALLOCATION,
  ZZPLAY_FAILURE_SESSION,
  ZZPLAY_FAILURE_TIMER,
  ZZPLAY_FAILURE_IO,
  ZZPLAY_FAILURE_PROTOCOL
} ZZPlayFailure;

typedef enum ZZPlayResource {
  ZZPLAY_RESOURCE_INPUT_FILE = 0,
  ZZPLAY_RESOURCE_P96_LIBRARY,
  ZZPLAY_RESOURCE_VIDEO_SCREEN,
  ZZPLAY_RESOURCE_VIDEO_WINDOW,
  ZZPLAY_RESOURCE_SDK_CONTEXT,
  ZZPLAY_RESOURCE_INPUT_BUFFER,
  ZZPLAY_RESOURCE_PCM_BUFFER,
  ZZPLAY_RESOURCE_VIDEO_SESSION,
  ZZPLAY_RESOURCE_AUDIO_SINK,
  ZZPLAY_RESOURCE_TIMER,
  ZZPLAY_RESOURCE_COUNT
} ZZPlayResource;

typedef struct ZZPlayResources {
  uint32_t acquired;
} ZZPlayResources;

typedef struct ZZPlayCore {
  ZZPlayState state;
  ZZPlayStopReason stop_reason;
  ZZPlayFailure failure;
  int status;
  ZZPlayResources resources;
} ZZPlayCore;

typedef int (*ZZPlayReleaseResource)(void *user,
                                     ZZPlayResource resource);

/* Informational output. Workbench launches have no console, so printing
 * from one makes AmigaDOS pop an output window that never closes; QUIET is
 * therefore the default there and can be turned off explicitly. Errors are
 * never suppressed - they go to stderr or a requester. */
void zzplay_set_quiet(int quiet);
int zzplay_is_quiet(void);
void zzplay_info(const char *format, ...);

void zzplay_core_init(ZZPlayCore *core);
int zzplay_core_begin_prebuffer(ZZPlayCore *core);
int zzplay_core_start(ZZPlayCore *core);
int zzplay_core_pause(ZZPlayCore *core);
int zzplay_core_resume(ZZPlayCore *core);
int zzplay_core_begin_drain(ZZPlayCore *core);
int zzplay_core_begin_loop(ZZPlayCore *core);
int zzplay_core_restart_loop(ZZPlayCore *core);
void zzplay_core_stop(ZZPlayCore *core, ZZPlayStopReason reason);
void zzplay_core_fail(ZZPlayCore *core, ZZPlayFailure failure, int status);
int zzplay_core_is_terminal(const ZZPlayCore *core);
int zzplay_resource_acquire(ZZPlayResources *resources,
                            ZZPlayResource resource);
int zzplay_resource_is_acquired(const ZZPlayResources *resources,
                                ZZPlayResource resource);
int zzplay_resource_release(ZZPlayResources *resources,
                            ZZPlayResource resource,
                            ZZPlayReleaseResource release,
                            void *user);
int zzplay_resources_release_all(ZZPlayResources *resources,
                                 ZZPlayReleaseResource release,
                                 void *user);

#endif /* ZZPLAY_CORE_H */
