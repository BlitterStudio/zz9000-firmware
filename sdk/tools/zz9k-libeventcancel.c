/*
 * Real zz9k.library event-dispatched cancellation smoke test for AmigaOS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/event_wait.h"
#include "zz9k/sdk.h"
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define ZZ9K_EVENT_CANCEL_COUNT 16U
#define ZZ9K_EVENT_CANCEL_INDEX (ZZ9K_EVENT_CANCEL_COUNT - 1U)
#define ZZ9K_EVENT_CANCEL_ATTEMPTS 4U
#define ZZ9K_EVENT_CANCEL_TIMEOUT_SECS 2UL

struct Library *ZZ9KBase;

static void build_ping_request(ZZ9KRequest *request, uint32_t index)
{
  uint8_t payload[4];

  payload[0] = 'c';
  payload[1] = (uint8_t)index;
  payload[2] = 'e';
  payload[3] = 'v';
  (void)zz9k_request_ping(request, payload, sizeof(payload));
}

static int ping_reply_ok(const ZZ9KMailboxEntry *reply, uint32_t index)
{
  if (!reply || reply->opcode != ZZ9K_OP_PING ||
      reply->status != ZZ9K_STATUS_OK || reply->payload_len != 4) {
    return 0;
  }

  return reply->payload.inline_data[0] == 'c' &&
         reply->payload.inline_data[1] == (uint8_t)index &&
         reply->payload.inline_data[2] == 'e' &&
         reply->payload.inline_data[3] == 'v';
}

static int async_index(const ZZ9KAsyncRequest *asyncs,
                       const ZZ9KAsyncRequest *async, uint32_t count,
                       uint32_t *index)
{
  uint32_t i;

  for (i = 0; i < count; i++) {
    if (async == &asyncs[i]) {
      *index = i;
      return 1;
    }
  }

  return 0;
}

static int wait_for_messages(ZZ9KEventTimer *timer, struct MsgPort *port,
                             ZZ9KAsyncRequest *asyncs, uint32_t queued,
                             int expect_cancel, uint32_t *ok_messages,
                             uint32_t *cancel_messages)
{
  ZZ9KAsyncRequest *completed[ZZ9K_EVENT_CANCEL_COUNT];
  ULONG signals;
  uint32_t drained;
  uint32_t messages;
  uint32_t index;
  uint32_t i;
  int status;
  int result;

  *ok_messages = 0;
  *cancel_messages = 0;
  messages = 0;
  result = 10;

  while (messages < queued) {
    drained = 0;
    status = zz9k_event_wait_async_port(timer, port,
                                        ZZ9K_EVENT_CANCEL_TIMEOUT_SECS,
                                        SIGBREAKF_CTRL_C, completed,
                                        ZZ9K_EVENT_CANCEL_COUNT,
                                        &drained, &signals);
    if (status == ZZ9K_STATUS_CANCELLED) {
      printf("cancelled while waiting for event cancellation messages\n");
      goto out;
    }
    if (status == ZZ9K_STATUS_TIMEOUT) {
      printf("timeout waiting for event cancellation messages got=%lu\n",
             (unsigned long)messages);
      goto out;
    }
    if (status != ZZ9K_STATUS_OK) {
      printf("event cancellation wait failed: %s (%d)\n",
             zz9k_status_text(status), status);
      goto out;
    }

    for (i = 0; i < drained; i++) {
      if (!async_index(asyncs, completed[i], queued, &index)) {
        printf("unexpected completion 0x%08lx\n",
               (unsigned long)completed[i]);
        goto out;
      }
      if (!asyncs[index].completed || asyncs[index].queued) {
        printf("bad async state index=%lu completed=%lu queued=%lu\n",
               (unsigned long)index, (unsigned long)asyncs[index].completed,
               (unsigned long)asyncs[index].queued);
        goto out;
      }

      if (index == ZZ9K_EVENT_CANCEL_INDEX && expect_cancel) {
        if (asyncs[index].status != ZZ9K_STATUS_CANCELLED ||
            asyncs[index].reply.status != ZZ9K_STATUS_CANCELLED) {
          printf("bad cancel status async=%s (%d) reply=%s (%d)\n",
                 zz9k_status_text(asyncs[index].status),
                 asyncs[index].status,
                 zz9k_status_text(asyncs[index].reply.status),
                 asyncs[index].reply.status);
          goto out;
        }
        (*cancel_messages)++;
      } else {
        if (asyncs[index].status != ZZ9K_STATUS_OK ||
            !ping_reply_ok(&asyncs[index].reply, index)) {
          printf("bad ping reply index=%lu status=%s (%d)\n",
                 (unsigned long)index,
                 zz9k_status_text(asyncs[index].status),
                 asyncs[index].status);
          goto out;
        }
        (*ok_messages)++;
      }

      messages++;
    }
  }

  if (expect_cancel && *cancel_messages != 1U) {
    printf("missing cancel message count=%lu\n",
           (unsigned long)*cancel_messages);
    goto out;
  }
  if (!expect_cancel && *cancel_messages != 0U) {
    printf("unexpected cancel message count=%lu\n",
           (unsigned long)*cancel_messages);
    goto out;
  }

  result = 0;

out:
  return result;
}

static int run_cancel_attempt(uint32_t attempt)
{
  ZZ9KEventTimer timer;
  struct MsgPort *port;
  ZZ9KRequest requests[ZZ9K_EVENT_CANCEL_COUNT];
  ZZ9KAsyncRequest asyncs[ZZ9K_EVENT_CANCEL_COUNT];
  uint32_t queued;
  uint32_t ok_messages;
  uint32_t cancel_messages;
  uint32_t i;
  int status;
  int result;

  port = CreateMsgPort();
  if (!port) {
    printf("CreateMsgPort failed\n");
    return 20;
  }
  if (!zz9k_event_timer_open(&timer)) {
    printf("timer open failed\n");
    DeleteMsgPort(port);
    return 20;
  }

  for (i = 0; i < ZZ9K_EVENT_CANCEL_COUNT; i++) {
    build_ping_request(&requests[i], i);
  }
  memset(asyncs, 0, sizeof(asyncs));

  printf("attempt %lu: queueing %lu async pings via event message port\n",
         (unsigned long)attempt,
         (unsigned long)ZZ9K_EVENT_CANCEL_COUNT);
  queued = 0;
  status = ZZ9KCallAsyncBatchMsg(asyncs, requests,
                                 ZZ9K_EVENT_CANCEL_COUNT, port, &queued);
  if (status != ZZ9K_STATUS_QUEUED ||
      queued != ZZ9K_EVENT_CANCEL_COUNT) {
    printf("async batch queue failed: status=%s (%d) queued=%lu\n",
           zz9k_status_text(status), status, (unsigned long)queued);
    if (status == ZZ9K_STATUS_QUEUED && queued != 0U) {
      printf("partial batch drain queued=%lu\n", (unsigned long)queued);
      (void)wait_for_messages(&timer, port, asyncs, queued, 0,
                              &ok_messages, &cancel_messages);
    }
    result = 10;
    goto out;
  }

  status = ZZ9KCancelAsync(&asyncs[ZZ9K_EVENT_CANCEL_INDEX]);
  if (status == ZZ9K_STATUS_OK) {
    result = wait_for_messages(&timer, port, asyncs, queued, 1,
                               &ok_messages, &cancel_messages);
    if (result == 0) {
      printf("cancel event ok attempt=%lu ok=%lu cancelled=%lu\n",
             (unsigned long)attempt, (unsigned long)ok_messages,
             (unsigned long)cancel_messages);
    }
    goto out;
  }

  if (status == ZZ9K_STATUS_BAD_REQUEST ||
      status == ZZ9K_STATUS_NOT_FOUND) {
    printf("attempt %lu: cancel lost completion race: %s (%d)\n",
           (unsigned long)attempt, zz9k_status_text(status), status);
    result = wait_for_messages(&timer, port, asyncs, queued, 0,
                               &ok_messages, &cancel_messages);
    if (result == 0) {
      result = ZZ9K_STATUS_BUSY;
    }
    goto out;
  }

  printf("cancel failed: %s (%d)\n", zz9k_status_text(status), status);
  result = 10;

out:
  zz9k_event_timer_close(&timer);
  DeleteMsgPort(port);
  return result;
}

int main(void)
{
  uint32_t attempt;
  int result;

  printf("zz9k-libeventcancel: opening zz9k.library\n");
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

  result = 10;
  for (attempt = 1; attempt <= ZZ9K_EVENT_CANCEL_ATTEMPTS; attempt++) {
    result = run_cancel_attempt(attempt);
    if (result == 0) {
      CloseLibrary(ZZ9KBase);
      return 0;
    }
    if (result != ZZ9K_STATUS_BUSY) {
      CloseLibrary(ZZ9KBase);
      return result;
    }
  }

  printf("cancel request completed before cancellation on all attempts\n");
  CloseLibrary(ZZ9KBase);
  return 10;
}
