/*
 * Real zz9k.library event-dispatched callback smoke test for AmigaOS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/event_wait.h"
#include "zz9k/sdk.h"
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define ZZ9K_LIBEVENTCB_TIMEOUT_SECS 2UL

struct Library *ZZ9KBase;

typedef struct CallbackState {
  struct Task *task;
  ULONG signal_mask;
  volatile ULONG called;
  uint32_t request_id;
  int status;
  ZZ9KMailboxEntry reply;
} CallbackState;

static void build_ping_request(ZZ9KRequest *request)
{
  static const uint8_t payload[4] = {'c', 'b', '!', '!'};
  (void)zz9k_request_ping(request, payload, sizeof(payload));
}

static int ping_reply_ok(const ZZ9KMailboxEntry *reply)
{
  if (!reply || reply->opcode != ZZ9K_OP_PING ||
      reply->status != ZZ9K_STATUS_OK || reply->payload_len != 4) {
    return 0;
  }

  return reply->payload.inline_data[0] == 'c' &&
         reply->payload.inline_data[1] == 'b' &&
         reply->payload.inline_data[2] == '!' &&
         reply->payload.inline_data[3] == '!';
}

static void event_callback(ZZ9KAsyncRequest *async, void *user_data)
{
  CallbackState *state = (CallbackState *)user_data;

  if (!state || !async) {
    return;
  }

  state->request_id = async->request_id;
  state->status = async->status;
  state->reply = async->reply;
  state->called++;
  if (state->task && state->signal_mask) {
    Signal(state->task, state->signal_mask);
  }
}

int main(void)
{
  ZZ9KEventTimer timer;
  CallbackState state;
  ZZ9KRequest request;
  ZZ9KAsyncRequest async;
  BYTE signal_bit;
  ULONG signal_mask;
  ULONG signals;
  int status;
  int result;

  printf("zz9k-libeventcb: opening zz9k.library\n");
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

  signal_bit = AllocSignal(-1);
  if (signal_bit < 0) {
    printf("AllocSignal failed\n");
    CloseLibrary(ZZ9KBase);
    return 20;
  }
  signal_mask = 1UL << signal_bit;
  (void)SetSignal(0, signal_mask);

  if (!zz9k_event_timer_open(&timer)) {
    printf("timer open failed\n");
    FreeSignal(signal_bit);
    CloseLibrary(ZZ9KBase);
    return 20;
  }

  build_ping_request(&request);
  memset(&async, 0, sizeof(async));
  memset(&state, 0, sizeof(state));
  state.task = FindTask(0);
  state.signal_mask = signal_mask;

  printf("queueing async ping via callback\n");
  status = ZZ9KCallAsync(&async, &request, event_callback, &state);
  if (status != ZZ9K_STATUS_QUEUED) {
    printf("async callback queue failed: %s (%d)\n",
           zz9k_status_text(status), status);
    zz9k_event_timer_close(&timer);
    FreeSignal(signal_bit);
    CloseLibrary(ZZ9KBase);
    return 10;
  }

  result = 10;
  status = zz9k_event_wait_signal(&timer, signal_mask,
                                  ZZ9K_LIBEVENTCB_TIMEOUT_SECS,
                                  SIGBREAKF_CTRL_C, &signals);
  if (status == ZZ9K_STATUS_CANCELLED) {
    printf("cancelled while waiting for event callback\n");
  } else if (status == ZZ9K_STATUS_TIMEOUT && state.called == 0) {
    printf("timeout waiting for event callback\n");
  } else if (status != ZZ9K_STATUS_OK && state.called == 0) {
    printf("event callback wait failed: %s (%d)\n",
           zz9k_status_text(status), status);
  } else if (state.called != 1) {
    printf("unexpected callback count=%lu\n", (unsigned long)state.called);
  } else if (!async.completed || async.queued) {
    printf("bad async state completed=%lu queued=%lu\n",
           (unsigned long)async.completed, (unsigned long)async.queued);
  } else if (state.status != ZZ9K_STATUS_OK ||
             async.status != ZZ9K_STATUS_OK) {
    printf("callback status failed: state=%s (%d) async=%s (%d)\n",
           zz9k_status_text(state.status), state.status,
           zz9k_status_text(async.status), async.status);
  } else if (state.request_id != async.request_id) {
    printf("request id mismatch state=%lu async=%lu\n",
           (unsigned long)state.request_id,
           (unsigned long)async.request_id);
  } else if (!ping_reply_ok(&state.reply) || !ping_reply_ok(&async.reply)) {
    printf("reply verification failed opcode=%lu status=%s (%d) len=%lu\n",
           (unsigned long)async.reply.opcode,
           zz9k_status_text(async.reply.status), async.reply.status,
           (unsigned long)async.reply.payload_len);
  } else {
    printf("event callback ok request_id=%lu\n",
           (unsigned long)async.request_id);
    result = 0;
  }

  zz9k_event_timer_close(&timer);
  FreeSignal(signal_bit);
  CloseLibrary(ZZ9KBase);
  return result;
}
