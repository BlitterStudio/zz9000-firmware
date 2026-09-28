/*
 * Real zz9k.library event-dispatched message-port smoke test for AmigaOS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/event_wait.h"
#include "zz9k/sdk.h"
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define ZZ9K_LIBEVENT_TIMEOUT_SECS 2UL

struct Library *ZZ9KBase;

static void build_ping_request(ZZ9KRequest *request)
{
  static const uint8_t payload[4] = {'e', 'v', 't', '!'};
  (void)zz9k_request_ping(request, payload, sizeof(payload));
}

static int ping_reply_ok(const ZZ9KMailboxEntry *reply)
{
  if (!reply || reply->opcode != ZZ9K_OP_PING ||
      reply->status != ZZ9K_STATUS_OK || reply->payload_len != 4) {
    return 0;
  }

  return reply->payload.inline_data[0] == 'e' &&
         reply->payload.inline_data[1] == 'v' &&
         reply->payload.inline_data[2] == 't' &&
         reply->payload.inline_data[3] == '!';
}

int main(void)
{
  ZZ9KEventTimer timer;
  struct MsgPort *port;
  ZZ9KRequest request;
  ZZ9KAsyncRequest async;
  ULONG signals;
  int status;
  int result;

  printf("zz9k-libevent: opening zz9k.library\n");
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

  build_ping_request(&request);
  memset(&async, 0, sizeof(async));
  printf("queueing async ping via message port\n");
  status = ZZ9KCallAsyncMsg(&async, &request, port);
  if (status != ZZ9K_STATUS_QUEUED) {
    printf("async queue failed: %s (%d)\n", zz9k_status_text(status),
           status);
    zz9k_event_timer_close(&timer);
    DeleteMsgPort(port);
    CloseLibrary(ZZ9KBase);
    return 10;
  }

  result = 10;
  status = zz9k_event_wait_msg(&timer, port, &async,
                               ZZ9K_LIBEVENT_TIMEOUT_SECS,
                               SIGBREAKF_CTRL_C, &signals);
  if (status == ZZ9K_STATUS_CANCELLED) {
    printf("cancelled while waiting for event message\n");
  } else if (status == ZZ9K_STATUS_TIMEOUT) {
    printf("timeout waiting for event message\n");
  } else if (status != ZZ9K_STATUS_OK) {
    printf("async status failed: %s (%d)\n",
           zz9k_status_text(status), status);
  } else if (!ping_reply_ok(&async.reply)) {
    printf("reply verification failed opcode=%lu status=%s (%d) len=%lu\n",
           (unsigned long)async.reply.opcode,
           zz9k_status_text(async.reply.status), async.reply.status,
           (unsigned long)async.reply.payload_len);
  } else {
    printf("event async ok request_id=%lu\n",
           (unsigned long)async.request_id);
    result = 0;
  }

  zz9k_event_timer_close(&timer);
  DeleteMsgPort(port);
  CloseLibrary(ZZ9KBase);
  return result;
}
