/*
 * Real zz9k.library async cancellation smoke test for AmigaOS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/sdk.h"
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define ZZ9K_LIBCANCEL_DRAIN_POLLS 1000UL

struct Library *ZZ9KBase;

static void build_ping_request(ZZ9KRequest *request)
{
  static const uint8_t payload[4] = {'s', 't', 'o', 'p'};
  (void)zz9k_request_ping(request, payload, sizeof(payload));
}

static int drain_late_completion(uint32_t *drained)
{
  uint32_t completed;
  uint32_t polls;
  int status;

  *drained = 0;
  for (polls = 0; polls < ZZ9K_LIBCANCEL_DRAIN_POLLS; polls++) {
    completed = 0;
    status = ZZ9KPoll(1, &completed);
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }
    *drained += completed;
    if (completed != 0) {
      break;
    }
  }

  return ZZ9K_STATUS_OK;
}

int main(void)
{
  struct MsgPort *port;
  struct Message *message;
  ZZ9KRequest request;
  ZZ9KAsyncRequest async;
  uint32_t drained;
  int status;
  int result;

  printf("zz9k-libcancel: opening zz9k.library\n");
  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    printf("open failed\n");
    return 20;
  }

  printf("open ok base=0x%08lx version=%u revision=%u\n",
         (unsigned long)ZZ9KBase, ZZ9KBase->lib_Version,
         ZZ9KBase->lib_Revision);

  if (ZZ9KBase->lib_Revision < ZZ9K_LIBRARY_MIN_REVISION_CANCEL_ASYNC) {
    printf("zz9k.library revision too old for async cancellation\n");
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

  printf("cancelling request_id=%lu\n", (unsigned long)async.request_id);
  status = ZZ9KCancelAsync(&async);
  if (status != ZZ9K_STATUS_OK) {
    printf("cancel failed: %s (%d)\n", zz9k_status_text(status), status);
    DeleteMsgPort(port);
    CloseLibrary(ZZ9KBase);
    return 10;
  }

  result = 10;
  message = GetMsg(port);
  if (!message) {
    printf("cancel did not post a completion message\n");
    goto out;
  }
  if (message != &async.message) {
    printf("unexpected message 0x%08lx expected 0x%08lx\n",
           (unsigned long)message, (unsigned long)&async.message);
    goto out;
  }
  if (!async.completed || async.queued) {
    printf("bad async state completed=%lu queued=%lu\n",
           (unsigned long)async.completed, (unsigned long)async.queued);
    goto out;
  }
  if (async.status != ZZ9K_STATUS_CANCELLED ||
      async.reply.status != ZZ9K_STATUS_CANCELLED) {
    printf("bad cancel status async=%s (%d) reply=%s (%d)\n",
           zz9k_status_text(async.status), async.status,
           zz9k_status_text(async.reply.status), async.reply.status);
    goto out;
  }

  status = drain_late_completion(&drained);
  if (status != ZZ9K_STATUS_OK) {
    printf("late completion drain failed: %s (%d)\n",
           zz9k_status_text(status), status);
    goto out;
  }

  printf("cancel async ok request_id=%lu drained=%lu\n",
         (unsigned long)async.request_id, (unsigned long)drained);
  result = 0;

out:
  DeleteMsgPort(port);
  CloseLibrary(ZZ9KBase);
  return result;
}
