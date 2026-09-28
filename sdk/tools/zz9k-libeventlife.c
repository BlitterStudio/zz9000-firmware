/*
 * Real zz9k.library event dispatcher lifecycle smoke test for AmigaOS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/event_wait.h"
#include "zz9k/sdk.h"
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define ZZ9K_LIFE_ROUNDS 5U
#define ZZ9K_LIFE_TIMEOUT_SECS 2UL

struct Library *ZZ9KBase;

static void build_ping_request(ZZ9KRequest *request, uint32_t round)
{
  uint8_t payload[4];

  payload[0] = 'l';
  payload[1] = 'i';
  payload[2] = '0' + (uint8_t)round;
  payload[3] = '!';
  (void)zz9k_request_ping(request, payload, sizeof(payload));
}

static int ping_reply_ok(const ZZ9KMailboxEntry *reply, uint32_t round)
{
  if (!reply || reply->opcode != ZZ9K_OP_PING ||
      reply->status != ZZ9K_STATUS_OK || reply->payload_len != 4) {
    return 0;
  }

  return reply->payload.inline_data[0] == 'l' &&
         reply->payload.inline_data[1] == 'i' &&
         reply->payload.inline_data[2] == '0' + (uint8_t)round &&
         reply->payload.inline_data[3] == '!';
}

static int run_round(uint32_t round)
{
  ZZ9KEventTimer timer;
  struct MsgPort *port;
  ZZ9KRequest request;
  ZZ9KAsyncRequest async;
  ULONG signals;
  int status;
  int result;

  printf("round %lu: opening zz9k.library\n", (unsigned long)round + 1UL);
  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    printf("round %lu: open failed\n", (unsigned long)round + 1UL);
    return 20;
  }

  if (ZZ9KBase->lib_Revision <
      ZZ9K_LIBRARY_MIN_REVISION_EVENT_DISPATCHER) {
    printf("zz9k.library revision too old for event dispatcher\n");
    CloseLibrary(ZZ9KBase);
    ZZ9KBase = 0;
    return 20;
  }

  port = CreateMsgPort();
  if (!port) {
    printf("round %lu: CreateMsgPort failed\n",
           (unsigned long)round + 1UL);
    CloseLibrary(ZZ9KBase);
    ZZ9KBase = 0;
    return 20;
  }

  if (!zz9k_event_timer_open(&timer)) {
    printf("round %lu: timer open failed\n", (unsigned long)round + 1UL);
    DeleteMsgPort(port);
    CloseLibrary(ZZ9KBase);
    ZZ9KBase = 0;
    return 20;
  }

  build_ping_request(&request, round);
  memset(&async, 0, sizeof(async));
  printf("round %lu: queueing async ping\n", (unsigned long)round + 1UL);
  status = ZZ9KCallAsyncMsg(&async, &request, port);
  if (status != ZZ9K_STATUS_QUEUED) {
    printf("round %lu: async queue failed: %s (%d)\n",
           (unsigned long)round + 1UL, zz9k_status_text(status), status);
    zz9k_event_timer_close(&timer);
    DeleteMsgPort(port);
    CloseLibrary(ZZ9KBase);
    ZZ9KBase = 0;
    return 10;
  }

  result = 10;
  status = zz9k_event_wait_msg(&timer, port, &async,
                               ZZ9K_LIFE_TIMEOUT_SECS,
                               SIGBREAKF_CTRL_C, &signals);
  if (status == ZZ9K_STATUS_CANCELLED) {
    printf("round %lu: cancelled while waiting for event message\n",
           (unsigned long)round + 1UL);
  } else if (status == ZZ9K_STATUS_TIMEOUT) {
    printf("round %lu: timeout waiting for event message\n",
           (unsigned long)round + 1UL);
  } else if (status != ZZ9K_STATUS_OK) {
    printf("round %lu: async status failed: %s (%d)\n",
           (unsigned long)round + 1UL, zz9k_status_text(status), status);
  } else if (!ping_reply_ok(&async.reply, round)) {
    printf("round %lu: reply verification failed opcode=%lu status=%s "
           "(%d) len=%lu\n",
           (unsigned long)round + 1UL, (unsigned long)async.reply.opcode,
           zz9k_status_text(async.reply.status), async.reply.status,
           (unsigned long)async.reply.payload_len);
  } else {
    printf("round %lu: ok request_id=%lu\n",
           (unsigned long)round + 1UL, (unsigned long)async.request_id);
    result = 0;
  }

  zz9k_event_timer_close(&timer);
  DeleteMsgPort(port);
  CloseLibrary(ZZ9KBase);
  ZZ9KBase = 0;
  return result;
}

int main(void)
{
  uint32_t round;
  int result;

  printf("zz9k-libeventlife: event dispatcher lifecycle test\n");
  for (round = 0; round < ZZ9K_LIFE_ROUNDS; round++) {
    result = run_round(round);
    if (result != 0) {
      return result;
    }
  }

  printf("event lifecycle ok rounds=%lu\n",
         (unsigned long)ZZ9K_LIFE_ROUNDS);
  return 0;
}
