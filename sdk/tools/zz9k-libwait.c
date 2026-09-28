/*
 * Real zz9k.library async wait smoke test for AmigaOS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/sdk.h"
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define ZZ9K_LIBWAIT_MAX_POLLS 1000000UL

struct Library *ZZ9KBase;

static void build_ping_request(ZZ9KRequest *request)
{
  static const uint8_t payload[4] = {'w', 'a', 'i', 't'};
  (void)zz9k_request_ping(request, payload, sizeof(payload));
}

static int ping_reply_ok(const ZZ9KMailboxEntry *reply)
{
  if (!reply || reply->opcode != ZZ9K_OP_PING ||
      reply->status != ZZ9K_STATUS_OK || reply->payload_len != 4) {
    return 0;
  }

  return reply->payload.inline_data[0] == 'w' &&
         reply->payload.inline_data[1] == 'a' &&
         reply->payload.inline_data[2] == 'i' &&
         reply->payload.inline_data[3] == 't';
}

int main(void)
{
  struct MsgPort *port;
  struct Message *message;
  ZZ9KRequest request;
  ZZ9KAsyncRequest async;
  uint32_t polls;
  int status;
  int result;

  printf("zz9k-libwait: opening zz9k.library\n");
  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    printf("open failed\n");
    return 20;
  }

  printf("open ok base=0x%08lx version=%u revision=%u\n",
         (unsigned long)ZZ9KBase, ZZ9KBase->lib_Version,
         ZZ9KBase->lib_Revision);

  if (ZZ9KBase->lib_Revision < ZZ9K_LIBRARY_MIN_REVISION_WAIT_ASYNC) {
    printf("zz9k.library revision too old for async wait\n");
    CloseLibrary(ZZ9KBase);
    return 20;
  }

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

  polls = 0;
  status = ZZ9KWaitAsync(&async, ZZ9K_LIBWAIT_MAX_POLLS, &polls);
  result = 10;
  if (status != ZZ9K_STATUS_OK) {
    printf("wait failed: %s (%d) polls=%lu\n",
           zz9k_status_text(status), status, (unsigned long)polls);
    goto out;
  }

  message = GetMsg(port);
  if (!message) {
    printf("wait completed without a message-port reply\n");
    goto out;
  }
  if (message != &async.message) {
    printf("unexpected message 0x%08lx expected 0x%08lx\n",
           (unsigned long)message, (unsigned long)&async.message);
    goto out;
  }
  if (!async.completed || async.queued || async.status != ZZ9K_STATUS_OK) {
    printf("bad async state completed=%lu queued=%lu status=%s (%d)\n",
           (unsigned long)async.completed, (unsigned long)async.queued,
           zz9k_status_text(async.status),
           async.status);
    goto out;
  }
  if (!ping_reply_ok(&async.reply)) {
    printf("reply verification failed opcode=%lu status=%s (%d) len=%lu\n",
           (unsigned long)async.reply.opcode,
           zz9k_status_text(async.reply.status), async.reply.status,
           (unsigned long)async.reply.payload_len);
    goto out;
  }

  printf("wait async ok after %lu polls request_id=%lu\n",
         (unsigned long)polls, (unsigned long)async.request_id);
  result = 0;

out:
  DeleteMsgPort(port);
  CloseLibrary(ZZ9KBase);
  return result;
}
