/*
 * Real zz9k.library async message-port smoke test for AmigaOS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/sdk.h"
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define ZZ9K_LIBASYNC_MAX_POLLS 1000000UL

struct Library *ZZ9KBase;

static void build_ping_request(ZZ9KRequest *request)
{
  static const uint8_t payload[4] = {'m', 's', 'g', '!'};
  (void)zz9k_request_ping(request, payload, sizeof(payload));
}

static int ping_reply_ok(const ZZ9KMailboxEntry *reply)
{
  if (!reply || reply->opcode != ZZ9K_OP_PING ||
      reply->status != ZZ9K_STATUS_OK || reply->payload_len != 4) {
    return 0;
  }

  return reply->payload.inline_data[0] == 'm' &&
         reply->payload.inline_data[1] == 's' &&
         reply->payload.inline_data[2] == 'g' &&
         reply->payload.inline_data[3] == '!';
}

int main(void)
{
  struct MsgPort *port;
  struct Message *message;
  ZZ9KRequest request;
  ZZ9KAsyncRequest async;
  uint32_t completed;
  uint32_t polls;
  int status;
  int result;

  printf("zz9k-libasync: opening zz9k.library\n");
  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    printf("open failed\n");
    return 20;
  }

  printf("open ok base=0x%08lx version=%u revision=%u\n",
         (unsigned long)ZZ9KBase, ZZ9KBase->lib_Version,
         ZZ9KBase->lib_Revision);

  port = CreateMsgPort();
  if (!port) {
    printf("CreateMsgPort failed\n");
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
    DeleteMsgPort(port);
    CloseLibrary(ZZ9KBase);
    return 10;
  }

  message = 0;
  for (polls = 0; polls < ZZ9K_LIBASYNC_MAX_POLLS; polls++) {
    completed = 0;
    status = ZZ9KPoll(1, &completed);
    if (status != ZZ9K_STATUS_OK) {
      printf("poll failed: %s (%d)\n", zz9k_status_text(status), status);
      break;
    }

    message = GetMsg(port);
    if (message) {
      break;
    }
  }

  result = 10;
  if (!message) {
    printf("timeout waiting for message after %lu polls\n",
           (unsigned long)polls);
  } else if (message != &async.message) {
    printf("unexpected message 0x%08lx expected 0x%08lx\n",
           (unsigned long)message, (unsigned long)&async.message);
  } else if (!async.completed || async.queued) {
    printf("bad async state completed=%lu queued=%lu\n",
           (unsigned long)async.completed, (unsigned long)async.queued);
  } else if (async.status != ZZ9K_STATUS_OK) {
    printf("async status failed: %s (%d)\n",
           zz9k_status_text(async.status), async.status);
  } else if (!ping_reply_ok(&async.reply)) {
    printf("reply verification failed opcode=%lu status=%s (%d) len=%lu\n",
           (unsigned long)async.reply.opcode,
           zz9k_status_text(async.reply.status), async.reply.status,
           (unsigned long)async.reply.payload_len);
  } else {
    printf("async ping ok after %lu polls request_id=%lu\n",
           (unsigned long)polls, (unsigned long)async.request_id);
    result = 0;
  }

  DeleteMsgPort(port);
  CloseLibrary(ZZ9KBase);
  return result;
}
