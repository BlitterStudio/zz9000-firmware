/*
 * Real zz9k.library event-dispatched multi-port smoke test for AmigaOS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/event_wait.h"
#include "zz9k/sdk.h"
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define ZZ9K_EVENT_PORTS 2U
#define ZZ9K_EVENT_PORTS_TIMEOUT_SECS 2UL

struct Library *ZZ9KBase;

static void build_ping_request(ZZ9KRequest *request, uint32_t index)
{
  uint8_t payload[4];

  payload[0] = 'p';
  payload[1] = '0' + (uint8_t)index;
  payload[2] = 'r';
  payload[3] = 't';
  (void)zz9k_request_ping(request, payload, sizeof(payload));
}

static int reply_ok(const ZZ9KAsyncRequest *async, uint32_t index)
{
  const ZZ9KMailboxEntry *reply;

  if (!async || !async->completed || async->queued ||
      async->status != ZZ9K_STATUS_OK) {
    return 0;
  }

  reply = &async->reply;
  if (reply->opcode != ZZ9K_OP_PING || reply->status != ZZ9K_STATUS_OK ||
      reply->payload_len != 4) {
    return 0;
  }

  return reply->payload.inline_data[0] == 'p' &&
         reply->payload.inline_data[1] == '0' + (uint8_t)index &&
         reply->payload.inline_data[2] == 'r' &&
         reply->payload.inline_data[3] == 't';
}

static int completion_index(const ZZ9KAsyncRequest *completed,
                            ZZ9KAsyncRequest *asyncs)
{
  uint32_t i;

  for (i = 0; i < ZZ9K_EVENT_PORTS; i++) {
    if (completed == &asyncs[i]) {
      return (int)i;
    }
  }

  return -1;
}

int main(void)
{
  ZZ9KEventTimer timer;
  struct MsgPort *ports[ZZ9K_EVENT_PORTS];
  ZZ9KRequest requests[ZZ9K_EVENT_PORTS];
  ZZ9KAsyncRequest asyncs[ZZ9K_EVENT_PORTS];
  ZZ9KAsyncRequest *completed[ZZ9K_EVENT_PORTS];
  uint8_t seen[ZZ9K_EVENT_PORTS];
  ULONG signals;
  uint32_t drained;
  uint32_t messages;
  uint32_t i;
  int index;
  int status;
  int result;

  printf("zz9k-libeventports: opening zz9k.library\n");
  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    printf("open failed\n");
    return 20;
  }

  printf("open ok base=0x%08lx version=%u revision=%u\n",
         (unsigned long)ZZ9KBase, ZZ9KBase->lib_Version,
         ZZ9KBase->lib_Revision);

  if (ZZ9KBase->lib_Revision <
      ZZ9K_LIBRARY_MIN_REVISION_EVENT_DISPATCHER) {
    printf("zz9k.library revision too old for event dispatcher\n");
    CloseLibrary(ZZ9KBase);
    return 20;
  }

  memset(ports, 0, sizeof(ports));
  memset(asyncs, 0, sizeof(asyncs));
  memset(seen, 0, sizeof(seen));

  for (i = 0; i < ZZ9K_EVENT_PORTS; i++) {
    ports[i] = CreateMsgPort();
    if (!ports[i]) {
      printf("CreateMsgPort failed for port %lu\n", (unsigned long)i);
      result = 20;
      goto out_ports;
    }
    build_ping_request(&requests[i], i);
  }

  if (!zz9k_event_timer_open(&timer)) {
    printf("timer open failed\n");
    result = 20;
    goto out_ports;
  }

  for (i = 0; i < ZZ9K_EVENT_PORTS; i++) {
    printf("queueing async ping on port %lu\n", (unsigned long)i);
    status = ZZ9KCallAsyncMsg(&asyncs[i], &requests[i], ports[i]);
    if (status != ZZ9K_STATUS_QUEUED) {
      printf("async queue failed on port %lu: %s (%d)\n",
             (unsigned long)i, zz9k_status_text(status), status);
      result = 10;
      goto out_timer;
    }
  }

  messages = 0;
  result = 10;
  while (messages < ZZ9K_EVENT_PORTS) {
    drained = 0;
    signals = 0;
    status = zz9k_event_wait_async_ports(
        &timer, ports, ZZ9K_EVENT_PORTS, ZZ9K_EVENT_PORTS_TIMEOUT_SECS,
        SIGBREAKF_CTRL_C, completed, ZZ9K_EVENT_PORTS, &drained, &signals);
    if (status == ZZ9K_STATUS_CANCELLED) {
      printf("cancelled while waiting for multi-port events\n");
      goto out_timer;
    }
    if (status == ZZ9K_STATUS_TIMEOUT) {
      printf("timeout waiting for multi-port events messages=%lu\n",
             (unsigned long)messages);
      goto out_timer;
    }
    if (status != ZZ9K_STATUS_OK) {
      printf("multi-port wait failed: %s (%d) drained=%lu "
             "signals=0x%08lx\n",
             zz9k_status_text(status), status, (unsigned long)drained,
             (unsigned long)signals);
      goto out_timer;
    }
    if (drained == 0U) {
      printf("multi-port wait returned without completions "
             "signals=0x%08lx\n",
             (unsigned long)signals);
      goto out_timer;
    }

    for (i = 0; i < drained; i++) {
      index = completion_index(completed[i], asyncs);
      if (index < 0) {
        printf("unexpected completion 0x%08lx\n",
               (unsigned long)completed[i]);
        goto out_timer;
      }
      if (seen[index]) {
        printf("port %d: duplicate message\n", index);
        goto out_timer;
      }
      seen[index] = 1;
      messages++;
    }
  }

  for (i = 0; i < ZZ9K_EVENT_PORTS; i++) {
    if (seen[i] != 1 || !reply_ok(&asyncs[i], i)) {
      printf("reply verification failed port=%lu seen=%lu request_id=%lu "
             "status=%s (%d)\n",
             (unsigned long)i, (unsigned long)seen[i],
             (unsigned long)asyncs[i].request_id,
             zz9k_status_text(asyncs[i].status), asyncs[i].status);
      goto out_timer;
    }
  }

  printf("event ports ok messages=%lu\n", (unsigned long)messages);
  result = 0;

out_timer:
  zz9k_event_timer_close(&timer);
out_ports:
  for (i = 0; i < ZZ9K_EVENT_PORTS; i++) {
    if (ports[i]) {
      DeleteMsgPort(ports[i]);
    }
  }
  CloseLibrary(ZZ9KBase);
  return result;
}
