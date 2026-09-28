/*
 * Header-only AmigaOS timer helper for event-driven zz9k.library callers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_EVENT_WAIT_H
#define ZZ9K_EVENT_WAIT_H

#include <devices/timer.h>
#include <proto/exec.h>
#include <string.h>
#include "zz9k/library.h"

typedef struct ZZ9KEventTimer {
  struct MsgPort *port;
  struct timerequest *request;
  ULONG signal_mask;
  int open;
} ZZ9KEventTimer;

static inline int zz9k_event_timer_open(ZZ9KEventTimer *timer)
{
  if (!timer) {
    return 0;
  }

  memset(timer, 0, sizeof(*timer));
  timer->port = CreateMsgPort();
  if (!timer->port) {
    return 0;
  }

  timer->request = (struct timerequest *)CreateIORequest(
      timer->port, sizeof(struct timerequest));
  if (!timer->request) {
    DeleteMsgPort(timer->port);
    memset(timer, 0, sizeof(*timer));
    return 0;
  }

  if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                 (struct IORequest *)timer->request, 0) != 0) {
    DeleteIORequest((struct IORequest *)timer->request);
    DeleteMsgPort(timer->port);
    memset(timer, 0, sizeof(*timer));
    return 0;
  }

  timer->open = 1;
  timer->signal_mask = 1UL << timer->port->mp_SigBit;
  return 1;
}

static inline void zz9k_event_timer_start(ZZ9KEventTimer *timer,
                                          ULONG seconds)
{
  if (!timer || !timer->request) {
    return;
  }

  timer->request->tr_node.io_Command = TR_ADDREQUEST;
  timer->request->tr_time.tv_secs = seconds;
  timer->request->tr_time.tv_micro = 0;
  SendIO((struct IORequest *)timer->request);
}

static inline void zz9k_event_timer_finish(ZZ9KEventTimer *timer)
{
  if (!timer || !timer->request) {
    return;
  }

  if (!CheckIO((struct IORequest *)timer->request)) {
    AbortIO((struct IORequest *)timer->request);
  }
  WaitIO((struct IORequest *)timer->request);
}

static inline void zz9k_event_timer_close(ZZ9KEventTimer *timer)
{
  if (!timer) {
    return;
  }

  if (timer->open) {
    CloseDevice((struct IORequest *)timer->request);
  }
  if (timer->request) {
    DeleteIORequest((struct IORequest *)timer->request);
  }
  if (timer->port) {
    DeleteMsgPort(timer->port);
  }
  memset(timer, 0, sizeof(*timer));
}

static inline int zz9k_event_drain_async_port(struct MsgPort *reply_port,
                                              ZZ9KAsyncRequest **asyncs,
                                              uint32_t max_asyncs,
                                              uint32_t *drained_out)
{
  struct Message *message;
  ZZ9KAsyncRequest *async;
  uint32_t drained;
  int overflow;

  if (drained_out) {
    *drained_out = 0;
  }
  if (!reply_port || !drained_out || (!asyncs && max_asyncs != 0U)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  drained = 0;
  overflow = 0;
  while ((message = GetMsg(reply_port)) != 0) {
    if (message->mn_Length != (uint16_t)sizeof(ZZ9KAsyncRequest)) {
      *drained_out = drained;
      return ZZ9K_STATUS_INTERNAL_ERROR;
    }

    async = (ZZ9KAsyncRequest *)message;
    if (!async->completed || async->queued) {
      *drained_out = drained;
      return ZZ9K_STATUS_INTERNAL_ERROR;
    }

    if (asyncs && drained < max_asyncs) {
      asyncs[drained] = async;
    } else if (asyncs) {
      overflow = 1;
    }
    drained++;
  }

  *drained_out = drained;
  if (overflow) {
    return ZZ9K_STATUS_NO_MEMORY;
  }
  return ZZ9K_STATUS_OK;
}

static inline int zz9k_event_drain_async_ports(struct MsgPort **reply_ports,
                                               uint32_t port_count,
                                               ZZ9KAsyncRequest **asyncs,
                                               uint32_t max_asyncs,
                                               uint32_t *drained_out)
{
  ZZ9KAsyncRequest **store;
  uint32_t capacity;
  uint32_t port_drained;
  uint32_t drained;
  uint32_t i;
  int final_status;
  int status;

  if (drained_out) {
    *drained_out = 0;
  }
  if (!reply_ports || port_count == 0U || !drained_out ||
      (!asyncs && max_asyncs != 0U)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  for (i = 0; i < port_count; i++) {
    if (!reply_ports[i]) {
      return ZZ9K_STATUS_BAD_REQUEST;
    }
  }

  drained = 0;
  final_status = ZZ9K_STATUS_OK;
  for (i = 0; i < port_count; i++) {
    store = 0;
    capacity = 0;
    if (asyncs && drained < max_asyncs) {
      store = asyncs + drained;
      capacity = max_asyncs - drained;
    } else if (asyncs) {
      store = asyncs;
    }

    port_drained = 0;
    status = zz9k_event_drain_async_port(reply_ports[i], store, capacity,
                                         &port_drained);
    drained += port_drained;
    *drained_out = drained;
    if (status == ZZ9K_STATUS_NO_MEMORY) {
      final_status = ZZ9K_STATUS_NO_MEMORY;
    } else if (status != ZZ9K_STATUS_OK) {
      return status;
    }
  }

  return final_status;
}

static inline int zz9k_event_wait_async_port(ZZ9KEventTimer *timer,
                                             struct MsgPort *reply_port,
                                             ULONG timeout_secs,
                                             ULONG cancel_mask,
                                             ZZ9KAsyncRequest **asyncs,
                                             uint32_t max_asyncs,
                                             uint32_t *drained_out,
                                             ULONG *signals_out)
{
  ULONG port_signal;
  ULONG wait_mask;
  ULONG signals;
  uint32_t drained;
  int result;
  int status;

  if (signals_out) {
    *signals_out = 0;
  }
  if (drained_out) {
    *drained_out = 0;
  }
  if (!timer || !reply_port || !drained_out) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  drained = 0;
  status = zz9k_event_drain_async_port(reply_port, asyncs, max_asyncs,
                                       &drained);
  *drained_out = drained;
  if (status != ZZ9K_STATUS_OK || drained != 0U) {
    return status;
  }

  port_signal = 1UL << reply_port->mp_SigBit;
  wait_mask = port_signal | timer->signal_mask | cancel_mask;

  result = ZZ9K_STATUS_INTERNAL_ERROR;
  zz9k_event_timer_start(timer, timeout_secs);
  for (;;) {
    signals = Wait(wait_mask);
    if (signals_out) {
      *signals_out = signals;
    }

    drained = 0;
    status = zz9k_event_drain_async_port(reply_port, asyncs, max_asyncs,
                                         &drained);
    *drained_out = drained;
    if (status != ZZ9K_STATUS_OK) {
      result = status;
      break;
    }
    if (drained != 0U) {
      result = ZZ9K_STATUS_OK;
      break;
    }
    if ((signals & cancel_mask) != 0UL) {
      result = ZZ9K_STATUS_CANCELLED;
      break;
    }
    if ((signals & timer->signal_mask) != 0UL) {
      result = ZZ9K_STATUS_TIMEOUT;
      break;
    }

    /* Empty reply-port signals are legal; keep waiting. */
    continue;
  }
  zz9k_event_timer_finish(timer);
  return result;
}

static inline int zz9k_event_wait_async_ports(ZZ9KEventTimer *timer,
                                              struct MsgPort **reply_ports,
                                              uint32_t port_count,
                                              ULONG timeout_secs,
                                              ULONG cancel_mask,
                                              ZZ9KAsyncRequest **asyncs,
                                              uint32_t max_asyncs,
                                              uint32_t *drained_out,
                                              ULONG *signals_out)
{
  ULONG wait_mask;
  ULONG signals;
  uint32_t drained;
  uint32_t i;
  int result;
  int status;

  if (signals_out) {
    *signals_out = 0;
  }
  if (drained_out) {
    *drained_out = 0;
  }
  if (!timer || !reply_ports || port_count == 0U || !drained_out) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  drained = 0;
  status = zz9k_event_drain_async_ports(reply_ports, port_count, asyncs,
                                        max_asyncs, &drained);
  *drained_out = drained;
  if (status != ZZ9K_STATUS_OK || drained != 0U) {
    return status;
  }

  wait_mask = timer->signal_mask | cancel_mask;
  for (i = 0; i < port_count; i++) {
    if (!reply_ports[i]) {
      return ZZ9K_STATUS_BAD_REQUEST;
    }
    wait_mask |= 1UL << reply_ports[i]->mp_SigBit;
  }

  result = ZZ9K_STATUS_INTERNAL_ERROR;
  zz9k_event_timer_start(timer, timeout_secs);
  for (;;) {
    signals = Wait(wait_mask);
    if (signals_out) {
      *signals_out = signals;
    }

    drained = 0;
    status = zz9k_event_drain_async_ports(reply_ports, port_count, asyncs,
                                          max_asyncs, &drained);
    *drained_out = drained;
    if (status != ZZ9K_STATUS_OK) {
      result = status;
      break;
    }
    if (drained != 0U) {
      result = ZZ9K_STATUS_OK;
      break;
    }
    if ((signals & cancel_mask) != 0UL) {
      result = ZZ9K_STATUS_CANCELLED;
      break;
    }
    if ((signals & timer->signal_mask) != 0UL) {
      result = ZZ9K_STATUS_TIMEOUT;
      break;
    }

    /* Empty reply-port signals are legal; keep waiting. */
    continue;
  }
  zz9k_event_timer_finish(timer);
  return result;
}

static inline int zz9k_event_wait_signal(ZZ9KEventTimer *timer,
                                         ULONG signal_mask,
                                         ULONG timeout_secs,
                                         ULONG cancel_mask,
                                         ULONG *signals_out)
{
  ULONG wait_mask;
  ULONG signals;

  if (signals_out) {
    *signals_out = 0;
  }
  if (!timer || signal_mask == 0UL) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  wait_mask = signal_mask | timer->signal_mask | cancel_mask;
  zz9k_event_timer_start(timer, timeout_secs);
  signals = Wait(wait_mask);
  zz9k_event_timer_finish(timer);

  if (signals_out) {
    *signals_out = signals;
  }
  if (signals & signal_mask) {
    return ZZ9K_STATUS_OK;
  }
  if (signals & cancel_mask) {
    return ZZ9K_STATUS_CANCELLED;
  }
  if (signals & timer->signal_mask) {
    return ZZ9K_STATUS_TIMEOUT;
  }

  return ZZ9K_STATUS_INTERNAL_ERROR;
}

static inline int zz9k_event_wait_msg(ZZ9KEventTimer *timer,
                                      struct MsgPort *reply_port,
                                      ZZ9KAsyncRequest *async,
                                      ULONG timeout_secs,
                                      ULONG cancel_mask,
                                      ULONG *signals_out)
{
  struct Message *message;
  ULONG port_signal;
  ULONG wait_mask;
  ULONG signals;
  int result;

  if (signals_out) {
    *signals_out = 0;
  }
  if (!timer || !reply_port || !async) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (!async->queued && !async->completed) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  port_signal = 1UL << reply_port->mp_SigBit;
  wait_mask = port_signal | timer->signal_mask | cancel_mask;

  message = GetMsg(reply_port);
  if (message == &async->message) {
    if (!async->completed || async->queued) {
      return ZZ9K_STATUS_INTERNAL_ERROR;
    }
    return async->status;
  }
  if (message) {
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }

  result = ZZ9K_STATUS_INTERNAL_ERROR;
  zz9k_event_timer_start(timer, timeout_secs);
  for (;;) {
    signals = Wait(wait_mask);
    if (signals_out) {
      *signals_out = signals;
    }

    message = GetMsg(reply_port);
    if (message == &async->message) {
      if (!async->completed || async->queued) {
        result = ZZ9K_STATUS_INTERNAL_ERROR;
        break;
      }
      result = async->status;
      break;
    }
    if (message) {
      result = ZZ9K_STATUS_INTERNAL_ERROR;
      break;
    }
    if ((signals & cancel_mask) != 0UL) {
      result = ZZ9K_STATUS_CANCELLED;
      break;
    }
    if ((signals & timer->signal_mask) != 0UL) {
      result = ZZ9K_STATUS_TIMEOUT;
      break;
    }

    /* Empty reply-port signals are legal; keep waiting. */
    continue;
  }
  zz9k_event_timer_finish(timer);
  return result;
}

#endif /* ZZ9K_EVENT_WAIT_H */
