/*
 * Real zz9k.library event-dispatched batch message-port smoke test for AmigaOS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/event_wait.h"
#include "zz9k/sdk.h"
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define ZZ9K_LIBEVENTBATCH_COUNT 3U
#define ZZ9K_LIBEVENTBATCH_TIMEOUT_SECS 3UL

struct Library *ZZ9KBase;

static void build_ping_request(ZZ9KRequest *request, uint32_t index)
{
  uint8_t payload[4];

  payload[0] = 'e';
  payload[1] = '0' + (uint8_t)index;
  payload[2] = 'v';
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

  return reply->payload.inline_data[0] == 'e' &&
         reply->payload.inline_data[1] == '0' + (uint8_t)index &&
         reply->payload.inline_data[2] == 'v' &&
         reply->payload.inline_data[3] == 't';
}

static int async_index(const ZZ9KAsyncRequest *async,
                       ZZ9KAsyncRequest *asyncs)
{
  uint32_t i;

  for (i = 0; i < ZZ9K_LIBEVENTBATCH_COUNT; i++) {
    if (async == &asyncs[i]) {
      return (int)i;
    }
  }

  return -1;
}

int main(void)
{
  ZZ9KEventTimer timer;
  struct MsgPort *port;
  ZZ9KRequest requests[ZZ9K_LIBEVENTBATCH_COUNT];
  ZZ9KAsyncRequest asyncs[ZZ9K_LIBEVENTBATCH_COUNT];
  ZZ9KAsyncRequest *completed[ZZ9K_LIBEVENTBATCH_COUNT];
  uint8_t seen[ZZ9K_LIBEVENTBATCH_COUNT];
  ULONG signals;
  uint32_t drained;
  uint32_t messages;
  uint32_t queued;
  uint32_t i;
  int index;
  int status;
  int result;

  printf("zz9k-libeventbatch: opening zz9k.library\n");
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

  port = CreateMsgPort();
  if (!port) {
    printf("CreateMsgPort failed\n");
    CloseLibrary(ZZ9KBase);
    return 20;
  }

  if (!zz9k_event_timer_open(&timer)) {
    printf("timer open failed\n");
    DeleteMsgPort(port);
    CloseLibrary(ZZ9KBase);
    return 20;
  }

  memset(asyncs, 0, sizeof(asyncs));
  memset(seen, 0, sizeof(seen));
  for (i = 0; i < ZZ9K_LIBEVENTBATCH_COUNT; i++) {
    build_ping_request(&requests[i], i);
  }

  queued = 0;
  printf("queueing %lu async pings via event message port\n",
         (unsigned long)ZZ9K_LIBEVENTBATCH_COUNT);
  status = ZZ9KCallAsyncBatchMsg(asyncs, requests,
                                 ZZ9K_LIBEVENTBATCH_COUNT, port, &queued);
  if (status != ZZ9K_STATUS_QUEUED ||
      queued != ZZ9K_LIBEVENTBATCH_COUNT) {
    printf("async event batch queue failed: status=%s (%d) queued=%lu\n",
           zz9k_status_text(status), status, (unsigned long)queued);
    zz9k_event_timer_close(&timer);
    DeleteMsgPort(port);
    CloseLibrary(ZZ9KBase);
    return 10;
  }

  messages = 0;
  result = 10;

  while (messages < ZZ9K_LIBEVENTBATCH_COUNT) {
    drained = 0;
    status = zz9k_event_wait_async_port(&timer, port,
                                        ZZ9K_LIBEVENTBATCH_TIMEOUT_SECS,
                                        SIGBREAKF_CTRL_C, completed,
                                        ZZ9K_LIBEVENTBATCH_COUNT,
                                        &drained, &signals);
    if (status == ZZ9K_STATUS_CANCELLED) {
      printf("cancelled while waiting for event batch messages\n");
      goto out;
    }
    if (status == ZZ9K_STATUS_TIMEOUT) {
      printf("timeout waiting for event batch messages got=%lu\n",
             (unsigned long)messages);
      goto out;
    }
    if (status != ZZ9K_STATUS_OK) {
      printf("event batch wait failed: %s (%d)\n",
             zz9k_status_text(status), status);
      goto out;
    }

    for (i = 0; i < drained; i++) {
      index = async_index(completed[i], asyncs);
      if (index < 0) {
        printf("unexpected completion 0x%08lx\n",
               (unsigned long)completed[i]);
        goto out;
      }
      if (seen[index]) {
        printf("duplicate message for slot %d\n", index);
        goto out;
      }
      seen[index] = 1;
      messages++;
    }
  }

  for (i = 0; i < ZZ9K_LIBEVENTBATCH_COUNT; i++) {
    if (!reply_ok(&asyncs[i], i)) {
      printf("reply verification failed slot=%lu request_id=%lu "
             "status=%s (%d)\n",
             (unsigned long)i, (unsigned long)asyncs[i].request_id,
             zz9k_status_text(asyncs[i].status),
             asyncs[i].status);
      goto out;
    }
  }

  printf("event batch ok queued=%lu messages=%lu\n",
         (unsigned long)queued, (unsigned long)messages);
  result = 0;

out:
  zz9k_event_timer_close(&timer);
  DeleteMsgPort(port);
  CloseLibrary(ZZ9KBase);
  return result;
}
