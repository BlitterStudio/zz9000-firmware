/*
 * AmigaOS zz9k.library client API for ARM app-runtime handles.
 *
 * Copyright (C) 2024-2026 Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#ifndef ZZ9K_APP_CLIENT_H
#define ZZ9K_APP_CLIENT_H

#include "zz9k/app.h"
#include <exec/ports.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZZ9KAppHandle ZZ9KAppHandle;

enum ZZ9KAppEventType {
  ZZ9K_APP_EVENT_MESSAGE = 1U,
  ZZ9K_APP_EVENT_COMPLETE = 2U,
  ZZ9K_APP_EVENT_FAULT = 3U
};


typedef struct ZZ9KAppEvent {
  struct Message message;
  uint32_t type;
  ZZ9KAppStatusInfo status;
} ZZ9KAppEvent;

/* A handle belongs to the task that calls ZZ9KAppOpen; do not pass it to
 * another task. Only its owner can operate on or close it. When the runtime
 * advertises ZZ9K_APP_CAP_SLOTS, up to ZZ9K_APP_SLOT_COUNT handles may be
 * open at once. Older firmware accepts one handle, preserving the original
 * single-slot behaviour. `notify_port` receives at most one outstanding ZZ9KAppEvent.
 * Before closing the handle, drain that port, then call ZZ9KAppGetMessage
 * until it reports ZZ9K_STATUS_BUSY. The message signal is also available
 * independently for tasks that do not use a message port. */
int ZZ9KAppOpen(ZZ9KAppHandle **handle, struct MsgPort *notify_port);
int ZZ9KAppLoadFile(ZZ9KAppHandle *handle, const char *path);
int ZZ9KAppLoadMemory(ZZ9KAppHandle *handle, const void *data,
                      uint32_t length);
int ZZ9KAppStart(ZZ9KAppHandle *handle, const char *const *argv,
                 uint32_t argc);
int ZZ9KAppStop(ZZ9KAppHandle *handle);
int ZZ9KAppStatus(ZZ9KAppHandle *handle, ZZ9KAppStatusInfo *status);
int ZZ9KAppSendMessage(ZZ9KAppHandle *handle,
                       const ZZ9KAppMessage *message);
int ZZ9KAppGetMessage(ZZ9KAppHandle *handle, ZZ9KAppMessage *message);
uint32_t ZZ9KAppMessageSignal(const ZZ9KAppHandle *handle);
void ZZ9KAppClose(ZZ9KAppHandle *handle);
/* Sets the limits the next ZZ9KAppLoadFile or ZZ9KAppLoadMemory sends with
 * its LOAD_BEGIN, both in MiB. `max_mib` caps the app's private memory,
 * 0 for no cap beyond the card's pool. `host_visible_mib` is the app's
 * host-visible limit, 0 for the firmware default of 16 MiB. The host-visible
 * limit is a cap, not a reservation: allocations can still fail below it
 * while the card's shared heap is in use. Call it between ZZ9KAppOpen and
 * the load; it returns ZZ9K_STATUS_BUSY once a load has begun,
 * ZZ9K_STATUS_BAD_REQUEST above 4095, and ZZ9K_STATUS_UNSUPPORTED on a
 * legacy runtime that cannot carry a nonzero value. A host-visible request
 * larger than the card can ever hold makes the load itself fail with
 * ZZ9K_STATUS_NO_MEMORY. Requires ZZ9K_LIBRARY_MIN_REVISION_APP_SET_LIMITS. */
int ZZ9KAppSetLimits(ZZ9KAppHandle *handle, uint32_t max_mib,
                     uint32_t host_visible_mib);

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_APP_CLIENT_H */
