/*
 * Real zz9k.library batch async message-port smoke test for AmigaOS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/sdk.h"
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define ZZ9K_LIBBATCH_COUNT 3U
#define ZZ9K_LIBBATCH_MAX_POLLS 1000000UL

struct Library *ZZ9KBase;

static void build_ping_request(ZZ9KRequest *request, uint32_t index)
{
  uint8_t payload[4];

  payload[0] = 'b';
  payload[1] = '0' + (uint8_t)index;
  payload[2] = '!';
  payload[3] = '!';
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

  return reply->payload.inline_data[0] == 'b' &&
         reply->payload.inline_data[1] == '0' + (uint8_t)index &&
         reply->payload.inline_data[2] == '!' &&
         reply->payload.inline_data[3] == '!';
}

static int message_index(const struct Message *message,
                         ZZ9KAsyncRequest *asyncs)
{
  uint32_t i;

  for (i = 0; i < ZZ9K_LIBBATCH_COUNT; i++) {
    if (message == &asyncs[i].message) {
      return (int)i;
    }
  }

  return -1;
}

int main(void)
{
  struct MsgPort *port;
  struct Message *message;
  ZZ9KRequest requests[ZZ9K_LIBBATCH_COUNT];
  ZZ9KAsyncRequest asyncs[ZZ9K_LIBBATCH_COUNT];
  uint8_t seen[ZZ9K_LIBBATCH_COUNT];
  uint32_t completed;
  uint32_t messages;
  uint32_t polls;
  uint32_t queued;
  uint32_t i;
  int index;
  int status;
  int result;

  printf("zz9k-libbatch: opening zz9k.library\n");
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
      ZZ9K_LIBRARY_MIN_REVISION_CALL_ASYNC_BATCH_MSG) {
    printf("zz9k.library revision too old for batch message API\n");
    CloseLibrary(ZZ9KBase);
    return 20;
  }

  port = CreateMsgPort();
  if (!port) {
    printf("CreateMsgPort failed\n");
    CloseLibrary(ZZ9KBase);
    return 20;
  }

  memset(asyncs, 0, sizeof(asyncs));
  memset(seen, 0, sizeof(seen));
  for (i = 0; i < ZZ9K_LIBBATCH_COUNT; i++) {
    build_ping_request(&requests[i], i);
  }

  queued = 0;
  printf("queueing %lu async pings via one message port\n",
         (unsigned long)ZZ9K_LIBBATCH_COUNT);
  status = ZZ9KCallAsyncBatchMsg(asyncs, requests, ZZ9K_LIBBATCH_COUNT,
                                 port, &queued);
  if (status != ZZ9K_STATUS_QUEUED || queued != ZZ9K_LIBBATCH_COUNT) {
    printf("async batch queue failed: status=%s (%d) queued=%lu\n",
           zz9k_status_text(status), status, (unsigned long)queued);
    DeleteMsgPort(port);
    CloseLibrary(ZZ9KBase);
    return 10;
  }

  messages = 0;
  result = 10;
  for (polls = 0; polls < ZZ9K_LIBBATCH_MAX_POLLS &&
                  messages < ZZ9K_LIBBATCH_COUNT; polls++) {
    completed = 0;
    status = ZZ9KPoll(ZZ9K_LIBBATCH_COUNT, &completed);
    if (status != ZZ9K_STATUS_OK) {
      printf("poll failed: %s (%d)\n", zz9k_status_text(status), status);
      goto out;
    }

    while ((message = GetMsg(port)) != 0) {
      index = message_index(message, asyncs);
      if (index < 0) {
        printf("unexpected message 0x%08lx\n", (unsigned long)message);
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

  if (messages != ZZ9K_LIBBATCH_COUNT) {
    printf("timeout waiting for batch messages after %lu polls got=%lu\n",
           (unsigned long)polls, (unsigned long)messages);
    goto out;
  }

  for (i = 0; i < ZZ9K_LIBBATCH_COUNT; i++) {
    if (!reply_ok(&asyncs[i], i)) {
      printf("reply verification failed slot=%lu request_id=%lu "
             "status=%s (%d)\n",
             (unsigned long)i, (unsigned long)asyncs[i].request_id,
             zz9k_status_text(asyncs[i].status),
             asyncs[i].status);
      goto out;
    }
  }

  printf("async batch ok after %lu polls queued=%lu messages=%lu\n",
         (unsigned long)polls, (unsigned long)queued,
         (unsigned long)messages);
  result = 0;

out:
  DeleteMsgPort(port);
  CloseLibrary(ZZ9KBase);
  return result;
}
