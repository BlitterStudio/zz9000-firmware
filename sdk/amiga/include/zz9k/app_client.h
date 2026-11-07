/*
 * AmigaOS zz9k.library client API for one ARM app-runtime instance.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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

/* `notify_port` receives at most one outstanding ZZ9KAppEvent.  Dequeue it
 * before closing the handle; then call ZZ9KAppGetMessage until it reports
 * ZZ9K_STATUS_BUSY.  The message signal is also available independently for
 * tasks that do not use a message port. */
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

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_APP_CLIENT_H */
