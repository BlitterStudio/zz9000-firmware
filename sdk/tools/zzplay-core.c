/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-core.h"

#include <stdarg.h>
#include <stdio.h>
#include "zzplay-controls.h"

#include <string.h>

static int zzplay_quiet_output;

void zzplay_set_quiet(int quiet)
{
  zzplay_quiet_output = quiet ? 1 : 0;
}

int zzplay_is_quiet(void)
{
  return zzplay_quiet_output;
}

void zzplay_info(const char *format, ...)
{
  va_list args;

  if (zzplay_quiet_output) {
    return;
  }
  va_start(args, format);
  (void)vprintf(format, args);
  va_end(args);
}

void zzplay_core_init(ZZPlayCore *core)
{
  if (!core) {
    return;
  }
  memset(core, 0, sizeof(*core));
  core->state = ZZPLAY_STATE_PREPARING;
}

int zzplay_core_start(ZZPlayCore *core)
{
  if (!core ||
      (core->state != ZZPLAY_STATE_PREPARING &&
       core->state != ZZPLAY_STATE_PREBUFFERING)) {
    return 0;
  }
  core->state = ZZPLAY_STATE_PLAYING;
  return 1;
}

int zzplay_core_begin_prebuffer(ZZPlayCore *core)
{
  if (!core || core->state != ZZPLAY_STATE_PREPARING) {
    return 0;
  }
  core->state = ZZPLAY_STATE_PREBUFFERING;
  return 1;
}

int zzplay_core_pause(ZZPlayCore *core)
{
  if (!core || core->state != ZZPLAY_STATE_PLAYING) {
    return 0;
  }
  core->state = ZZPLAY_STATE_PAUSED;
  return 1;
}

int zzplay_core_resume(ZZPlayCore *core)
{
  if (!core || core->state != ZZPLAY_STATE_PAUSED) {
    return 0;
  }
  core->state = ZZPLAY_STATE_PLAYING;
  return 1;
}

int zzplay_core_begin_drain(ZZPlayCore *core)
{
  if (!core ||
      (core->state != ZZPLAY_STATE_PLAYING &&
       core->state != ZZPLAY_STATE_PREBUFFERING)) {
    return 0;
  }
  core->state = ZZPLAY_STATE_DRAINING;
  return 1;
}

int zzplay_core_begin_loop(ZZPlayCore *core)
{
  if (!core || core->state != ZZPLAY_STATE_DRAINING) {
    return 0;
  }
  core->state = ZZPLAY_STATE_LOOPING;
  return 1;
}

int zzplay_core_restart_loop(ZZPlayCore *core)
{
  if (!core || core->state != ZZPLAY_STATE_LOOPING) {
    return 0;
  }
  core->state = ZZPLAY_STATE_PREBUFFERING;
  return 1;
}

void zzplay_core_stop(ZZPlayCore *core, ZZPlayStopReason reason)
{
  if (!core || core->state == ZZPLAY_STATE_ERROR) {
    return;
  }
  core->state = ZZPLAY_STATE_STOPPED;
  core->stop_reason = reason;
}

void zzplay_core_fail(ZZPlayCore *core, ZZPlayFailure failure, int status)
{
  if (!core) {
    return;
  }
  core->state = ZZPLAY_STATE_ERROR;
  core->failure = failure;
  core->status = status;
}

int zzplay_core_is_terminal(const ZZPlayCore *core)
{
  return core &&
         (core->state == ZZPLAY_STATE_STOPPED ||
          core->state == ZZPLAY_STATE_ERROR);
}

int zzplay_resource_acquire(ZZPlayResources *resources,
                            ZZPlayResource resource)
{
  uint32_t bit;

  if (!resources || resource < ZZPLAY_RESOURCE_INPUT_FILE ||
      resource >= ZZPLAY_RESOURCE_COUNT) {
    return 0;
  }
  bit = (uint32_t)1U << (unsigned)resource;
  if ((resources->acquired & bit) != 0U) {
    return 0;
  }
  resources->acquired |= bit;
  return 1;
}

int zzplay_resource_is_acquired(const ZZPlayResources *resources,
                                ZZPlayResource resource)
{
  if (!resources || resource < ZZPLAY_RESOURCE_INPUT_FILE ||
      resource >= ZZPLAY_RESOURCE_COUNT) {
    return 0;
  }
  return (resources->acquired &
          ((uint32_t)1U << (unsigned)resource)) != 0U;
}

int zzplay_resource_release(ZZPlayResources *resources,
                            ZZPlayResource resource,
                            ZZPlayReleaseResource release,
                            void *user)
{
  uint32_t bit;
  int status;

  if (!resources || !release ||
      resource < ZZPLAY_RESOURCE_INPUT_FILE ||
      resource >= ZZPLAY_RESOURCE_COUNT) {
    return -1;
  }
  bit = (uint32_t)1U << (unsigned)resource;
  if ((resources->acquired & bit) == 0U) {
    return 0;
  }
  status = release(user, resource);
  if (status != 0) {
    return status;
  }
  resources->acquired &= ~bit;
  return 0;
}

int zzplay_resources_release_all(ZZPlayResources *resources,
                                 ZZPlayReleaseResource release,
                                 void *user)
{
  int first_error = 0;
  int index;

  if (!resources || !release) {
    return -1;
  }
  for (index = (int)ZZPLAY_RESOURCE_COUNT - 1; index >= 0; index--) {
    ZZPlayResource resource = (ZZPlayResource)index;
    uint32_t bit = (uint32_t)1U << (unsigned)resource;
    int status;

    if ((resources->acquired & bit) == 0U) {
      continue;
    }
    resources->acquired &= ~bit;
    status = release(user, resource);
    if (first_error == 0 && status != 0) {
      first_error = status;
    }
  }
  return first_error;
}

ZZPlayControlAction zzplay_control_action(int ctrl_c,
                                          int window_close,
                                          int toggle_pause)
{
  if (ctrl_c) {
    return ZZPLAY_CONTROL_STOP_CTRL_C;
  }
  if (window_close) {
    return ZZPLAY_CONTROL_STOP_WINDOW;
  }
  if (toggle_pause) {
    return ZZPLAY_CONTROL_TOGGLE_PAUSE;
  }
  return ZZPLAY_CONTROL_NONE;
}

ZZPlayControlAction zzplay_control_action_from_key(unsigned key)
{
  switch (key) {
  case ' ':
    return ZZPLAY_CONTROL_TOGGLE_PAUSE;
  case 0x1bU: /* Escape */
  case 'q':
  case 'Q':
    return ZZPLAY_CONTROL_STOP_KEY;
  case 'f':
  case 'F':
    return ZZPLAY_CONTROL_TOGGLE_FULLSCREEN;
  case 'l':
  case 'L':
    return ZZPLAY_CONTROL_TOGGLE_LOOP;
  default:
    return ZZPLAY_CONTROL_NONE;
  }
}

ZZPlayControlAction zzplay_control_resolve(const ZZPlayControlInput *input)
{
  if (!input) {
    return ZZPLAY_CONTROL_NONE;
  }
  /* Stops win over everything else in the same poll: a user who closed the
   * window is not also asking to go fullscreen. */
  if (input->ctrl_c) {
    return ZZPLAY_CONTROL_STOP_CTRL_C;
  }
  if (input->window_close) {
    return ZZPLAY_CONTROL_STOP_WINDOW;
  }
  return zzplay_control_action_from_key(input->key);
}

int zzplay_control_is_stop(ZZPlayControlAction action)
{
  return action == ZZPLAY_CONTROL_STOP_CTRL_C ||
         action == ZZPLAY_CONTROL_STOP_WINDOW ||
         action == ZZPLAY_CONTROL_STOP_KEY;
}

ZZPlayStopReason zzplay_control_stop_reason_from_action(
    ZZPlayControlAction action)
{
  if (action == ZZPLAY_CONTROL_STOP_CTRL_C) {
    return ZZPLAY_STOP_CTRL_C;
  }
  /* A key-driven stop is a deliberate user stop, exactly like the close
   * gadget, and must retire resources through the same path. */
  if (action == ZZPLAY_CONTROL_STOP_WINDOW ||
      action == ZZPLAY_CONTROL_STOP_KEY) {
    return ZZPLAY_STOP_WINDOW_CLOSE;
  }
  return ZZPLAY_STOP_NONE;
}
