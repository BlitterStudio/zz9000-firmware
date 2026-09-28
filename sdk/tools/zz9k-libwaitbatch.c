/*
 * Real zz9k.library batch async wait smoke test for AmigaOS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/sdk.h"
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define ZZ9K_LIBWAITBATCH_COUNT 3U
#define ZZ9K_LIBWAITBATCH_MAX_POLLS 1000000UL

struct Library *ZZ9KBase;

static void build_ping_request(ZZ9KRequest *request, uint32_t index)
{
  uint8_t payload[4];

  payload[0] = 'w';
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

  return reply->payload.inline_data[0] == 'w' &&
         reply->payload.inline_data[1] == '0' + (uint8_t)index &&
         reply->payload.inline_data[2] == '!' &&
         reply->payload.inline_data[3] == '!';
}

static int message_index(const struct Message *message,
                         ZZ9KAsyncRequest *asyncs)
{
  uint32_t i;

  for (i = 0; i < ZZ9K_LIBWAITBATCH_COUNT; i++) {
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
  ZZ9KRequest requests[ZZ9K_LIBWAITBATCH_COUNT];
  ZZ9KAsyncRequest asyncs[ZZ9K_LIBWAITBATCH_COUNT];
  uint8_t seen[ZZ9K_LIBWAITBATCH_COUNT];
  uint32_t completed;
  uint32_t messages;
  uint32_t polls;
  uint32_t queued;
  uint32_t i;
  int index;
  int status;
  int result;

  printf("zz9k-libwaitbatch: opening zz9k.library\n");
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
      ZZ9K_LIBRARY_MIN_REVISION_WAIT_ASYNC_BATCH) {
    printf("zz9k.library revision too old for batch async wait\n");
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
  for (i = 0; i < ZZ9K_LIBWAITBATCH_COUNT; i++) {
    build_ping_request(&requests[i], i);
  }

  queued = 0;
  printf("queueing %lu async pings via one message port\n",
         (unsigned long)ZZ9K_LIBWAITBATCH_COUNT);
  status = ZZ9KCallAsyncBatchMsg(asyncs, requests, ZZ9K_LIBWAITBATCH_COUNT,
                                 port, &queued);
  if (status != ZZ9K_STATUS_QUEUED ||
      queued != ZZ9K_LIBWAITBATCH_COUNT) {
    printf("async batch queue failed: status=%s (%d) queued=%lu\n",
           zz9k_status_text(status), status, (unsigned long)queued);
    DeleteMsgPort(port);
    CloseLibrary(ZZ9KBase);
    return 10;
  }

  completed = 0;
  polls = 0;
  status = ZZ9KWaitAsyncBatch(asyncs, ZZ9K_LIBWAITBATCH_COUNT,
                              ZZ9K_LIBWAITBATCH_MAX_POLLS,
                              &completed, &polls);
  result = 10;
  if (status != ZZ9K_STATUS_OK ||
      completed != ZZ9K_LIBWAITBATCH_COUNT) {
    printf("wait batch failed: status=%s (%d) completed=%lu polls=%lu\n",
           zz9k_status_text(status), status, (unsigned long)completed,
           (unsigned long)polls);
    goto out;
  }

  messages = 0;
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

  if (messages != ZZ9K_LIBWAITBATCH_COUNT) {
    printf("message count mismatch got=%lu expected=%lu\n",
           (unsigned long)messages,
           (unsigned long)ZZ9K_LIBWAITBATCH_COUNT);
    goto out;
  }

  for (i = 0; i < ZZ9K_LIBWAITBATCH_COUNT; i++) {
    if (!reply_ok(&asyncs[i], i)) {
      printf("reply verification failed slot=%lu request_id=%lu "
             "status=%s (%d)\n",
             (unsigned long)i, (unsigned long)asyncs[i].request_id,
             zz9k_status_text(asyncs[i].status),
             asyncs[i].status);
      goto out;
    }
  }

  printf("wait batch ok after %lu polls completed=%lu messages=%lu\n",
         (unsigned long)polls, (unsigned long)completed,
         (unsigned long)messages);
  result = 0;

out:
  DeleteMsgPort(port);
  CloseLibrary(ZZ9KBase);
  return result;
}
