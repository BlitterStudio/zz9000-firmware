/*
 * zz9k.library synchronous and event-driven message-port async example.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/event_wait.h"
#include "zz9k/sdk.h"
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define DEMO_ASYNC_TIMEOUT_SECS 2UL
#define DEMO_BATCH_COUNT 2U

struct Library *ZZ9KBase;

static void build_ping_request(ZZ9KRequest *request, const char *text)
{
  (void)zz9k_request_ping(request, (const uint8_t *)text, 4U);
}

static int async_ping_reply_ok(const ZZ9KAsyncRequest *async,
                               const char *text)
{
  return async &&
         async->completed &&
         async->status == ZZ9K_STATUS_OK &&
         async->reply.payload_len == 4 &&
         memcmp(async->reply.payload.inline_data, text, 4) == 0;
}

static int run_sync_ping(void)
{
  uint8_t request[4] = { 's', 'y', 'n', 'c' };
  uint8_t reply[8];
  uint32_t reply_len;
  int status;

  memset(reply, 0, sizeof(reply));
  reply_len = sizeof(reply);
  status = ZZ9KPing(request, 4, reply, &reply_len);
  if (status != ZZ9K_STATUS_OK) {
    printf("sync ping failed: %s (%d)\n", zz9k_status_text(status), status);
    return 0;
  }
  if (reply_len != 4 || memcmp(reply, request, 4) != 0) {
    printf("sync ping reply mismatch\n");
    return 0;
  }

  printf("sync ping: ok\n");
  return 1;
}

static int run_async_ping(void)
{
  ZZ9KEventTimer timer;
  struct MsgPort *port;
  ZZ9KRequest request;
  ZZ9KAsyncRequest async;
  ULONG signals;
  int status;
  int ok;

  if (ZZ9KBase->lib_Revision <
      ZZ9K_LIBRARY_MIN_REVISION_EVENT_DISPATCHER) {
    printf("zz9k.library revision too old for event dispatcher\n");
    return 0;
  }

  port = CreateMsgPort();
  if (!port) {
    printf("CreateMsgPort failed\n");
    return 0;
  }

  if (!zz9k_event_timer_open(&timer)) {
    printf("timer open failed\n");
    DeleteMsgPort(port);
    return 0;
  }

  build_ping_request(&request, "asyn");
  memset(&async, 0, sizeof(async));
  status = ZZ9KCallAsyncMsg(&async, &request, port);
  if (status != ZZ9K_STATUS_QUEUED) {
    printf("async queue failed: %s (%d)\n", zz9k_status_text(status),
           status);
    zz9k_event_timer_close(&timer);
    DeleteMsgPort(port);
    return 0;
  }

  ok = 0;
  status = zz9k_event_wait_msg(&timer, port, &async,
                               DEMO_ASYNC_TIMEOUT_SECS,
                               SIGBREAKF_CTRL_C, &signals);
  if (status != ZZ9K_STATUS_OK) {
    printf("async event wait failed: %s (%d) signals=0x%08lx\n",
           zz9k_status_text(status), status, (unsigned long)signals);
  } else {
    ok = async_ping_reply_ok(&async, "asyn");
  }

  zz9k_event_timer_close(&timer);
  DeleteMsgPort(port);
  if (!ok) {
    printf("async event ping failed\n");
    return 0;
  }

  printf("async event ping: ok\n");
  return 1;
}

static int batch_index(ZZ9KAsyncRequest *asyncs, ZZ9KAsyncRequest *async)
{
  uint32_t i;

  for (i = 0; i < DEMO_BATCH_COUNT; i++) {
    if (async == &asyncs[i]) {
      return (int)i;
    }
  }

  return -1;
}

static int run_async_batch_ping(void)
{
  ZZ9KEventTimer timer;
  struct MsgPort *port;
  ZZ9KRequest requests[DEMO_BATCH_COUNT];
  ZZ9KAsyncRequest asyncs[DEMO_BATCH_COUNT];
  ZZ9KAsyncRequest *completed[DEMO_BATCH_COUNT];
  const char *payloads[DEMO_BATCH_COUNT] = { "b0ch", "b1ch" };
  uint8_t seen[DEMO_BATCH_COUNT];
  ULONG signals;
  uint32_t drained;
  uint32_t messages;
  uint32_t queued;
  uint32_t i;
  int index;
  int status;
  int ok;

  port = CreateMsgPort();
  if (!port) {
    printf("CreateMsgPort failed\n");
    return 0;
  }

  if (!zz9k_event_timer_open(&timer)) {
    printf("timer open failed\n");
    DeleteMsgPort(port);
    return 0;
  }

  memset(asyncs, 0, sizeof(asyncs));
  memset(seen, 0, sizeof(seen));
  for (i = 0; i < DEMO_BATCH_COUNT; i++) {
    build_ping_request(&requests[i], payloads[i]);
  }

  queued = 0;
  status = ZZ9KCallAsyncBatchMsg(asyncs, requests, DEMO_BATCH_COUNT,
                                 port, &queued);
  if (status != ZZ9K_STATUS_QUEUED || queued != DEMO_BATCH_COUNT) {
    printf("async batch queue failed: %s (%d) queued=%lu\n",
           zz9k_status_text(status), status, (unsigned long)queued);
    zz9k_event_timer_close(&timer);
    DeleteMsgPort(port);
    return 0;
  }

  ok = 0;
  messages = 0;
  while (messages < DEMO_BATCH_COUNT) {
    drained = 0;
    status = zz9k_event_wait_async_port(&timer, port,
                                        DEMO_ASYNC_TIMEOUT_SECS,
                                        SIGBREAKF_CTRL_C,
                                        completed, DEMO_BATCH_COUNT,
                                        &drained, &signals);
    if (status != ZZ9K_STATUS_OK) {
      printf("async batch event wait failed: %s (%d) messages=%lu "
             "signals=0x%08lx\n",
             zz9k_status_text(status), status, (unsigned long)messages,
             (unsigned long)signals);
      break;
    }

    for (i = 0; i < drained; i++) {
      index = batch_index(asyncs, completed[i]);
      if (index < 0 || seen[index]) {
        printf("async batch unexpected/duplicate completion\n");
        goto out;
      }
      seen[index] = 1;
      messages++;
    }
  }

  if (messages != DEMO_BATCH_COUNT) {
    goto out;
  }
  for (i = 0; i < DEMO_BATCH_COUNT; i++) {
    if (!seen[i] || !async_ping_reply_ok(&asyncs[i], payloads[i])) {
      printf("async batch reply mismatch slot=%lu\n", (unsigned long)i);
      goto out;
    }
  }

  ok = 1;

out:
  zz9k_event_timer_close(&timer);
  DeleteMsgPort(port);
  if (!ok) {
    printf("async event batch ping failed\n");
    return 0;
  }

  printf("async event batch ping: ok messages=%lu\n",
         (unsigned long)messages);
  return 1;
}

int main(void)
{
  ZZ9KCaps caps;
  int status;
  int rc;

  rc = 1;
  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    printf("OpenLibrary(%s, %u) failed\n",
           ZZ9K_LIBRARY_NAME, ZZ9K_LIBRARY_VERSION);
    return 1;
  }

  printf("opened %s version=%u revision=%u\n",
         ZZ9K_LIBRARY_NAME, ZZ9KBase->lib_Version, ZZ9KBase->lib_Revision);

  memset(&caps, 0, sizeof(caps));
  status = ZZ9KQueryCaps(&caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("query caps failed: %s (%d)\n", zz9k_status_text(status),
           status);
    goto out;
  }

  printf("SDK ABI %u.%u caps=0x%08lx\n",
         (unsigned)caps.abi_major,
         (unsigned)caps.abi_minor,
         (unsigned long)caps.capability_bits);

  if (!run_sync_ping()) {
    goto out;
  }
  if (!run_async_ping()) {
    goto out;
  }
  if (!run_async_batch_ping()) {
    goto out;
  }

  rc = 0;

out:
  CloseLibrary(ZZ9KBase);
  return rc;
}
