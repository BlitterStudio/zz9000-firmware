/* Asynchronous worker process for service browsing and network transfers.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-browse-worker.h"

#include <stdio.h>
#include <string.h>

#include "zzplay-host.h"
#include "zzplay-sys.h"
#include "zzplay-tags.h"

#if defined(__amigaos__) || defined(AMIGA)

#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/dos.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/exec.h>

typedef struct ZZPlayWorkerMsg {
  struct Message msg;
  int quit;
  ZZPlayBrowseRequest req;
  const ZZPlayProvider *provider;
  ZZPlayBrowseReply reply;
} ZZPlayWorkerMsg;

typedef struct WorkerCollector {
  ZZPlayBrowseItem *items;
  uint32_t capacity;
  uint32_t count;
  uint32_t total;
  int has_total;
  volatile int *cancelled;
} WorkerCollector;

static struct MsgPort *gui_port = NULL;
static struct MsgPort *worker_port = NULL;
static struct Process *worker_proc = NULL;
static volatile int worker_cancel_flag = 0;
static int worker_is_active = 0;
static ZZPlayWorkerMsg active_msg;
static ZZPlayWorkerMsg quit_msg;
static struct Message startup_msg;

static int worker_keep_running(void *user)
{
  volatile int *cancelled = (volatile int *)user;
  return cancelled ? !(*cancelled) : 1;
}

static int worker_sink_item(void *user, const ZZPlayProviderItem *item)
{
  WorkerCollector *c = (WorkerCollector *)user;
  ZZPlayBrowseItem *dst;

  if (!c || !item || *(c->cancelled)) {
    return 1;
  }
  if (c->count >= c->capacity) {
    return 1;
  }
  dst = &c->items[c->count];
  memset(dst, 0, sizeof(*dst));
  if (item->id) {
    (void)snprintf(dst->id, sizeof(dst->id), "%s", item->id);
  }
  if (item->title) {
    (void)snprintf(dst->title, sizeof(dst->title), "%s", item->title);
    zzplay_tags_text_to_latin1(dst->title);
  }
  if (item->subtitle) {
    (void)snprintf(dst->subtitle, sizeof(dst->subtitle), "%s", item->subtitle);
    zzplay_tags_text_to_latin1(dst->subtitle);
  }
  dst->duration_ms = item->duration_ms;
  dst->kind = item->kind;
  dst->playable = item->playable;
  dst->favourite = item->favourite;
  c->count++;
  return 0;
}

static void worker_sink_total(void *user, uint32_t total)
{
  WorkerCollector *c = (WorkerCollector *)user;
  if (c) {
    c->total = total;
    c->has_total = 1;
  }
}
static int worker_provider_has_favourite(const ZZPlayProvider *p)
{
  if (!p) {
    return 0;
  }
  if (p->size < offsetof(ZZPlayProvider, favourite) + sizeof(p->favourite)) {
    return 0;
  }
  return p->favourite != NULL;
}


static void worker_process(void)
{
  struct MsgPort *my_port;
  void *ctx = NULL;
  const ZZPlayProvider *curr_p = NULL;
  char curr_scheme[ZZPLAY_PROVIDER_SCHEME_MAX];
  ZZPlayHostBinding binding;

  curr_scheme[0] = '\0';
  my_port = CreateMsgPort();
  if (!my_port) {
    return;
  }

  /* Send handshake to GUI */
  startup_msg.mn_ReplyPort = my_port;
  startup_msg.mn_Length = sizeof(startup_msg);
  PutMsg(gui_port, &startup_msg);

  for (;;) {
    ZZPlayWorkerMsg *wmsg;

    WaitPort(my_port);
    while ((wmsg = (ZZPlayWorkerMsg *)GetMsg(my_port)) != NULL) {
      const ZZPlayProvider *p;
      ZZPlayBrowseRequest *req;
      ZZPlayBrowseReply *rep;

      if (wmsg->quit) {
        if (ctx && curr_p && curr_p->close) {
          curr_p->close(ctx);
          ctx = NULL;
        }
        Forbid();
        ReplyMsg(&wmsg->msg);
        DeleteMsgPort(my_port);
        return;
      }

      p = wmsg->provider;
      req = &wmsg->req;
      rep = &wmsg->reply;
      rep->kind = req->kind;
      rep->generation = req->generation;
      rep->start = req->start;
      rep->success = 0;
      rep->error[0] = '\0';

      if (!p) {
        (void)snprintf(rep->error, sizeof(rep->error), "no provider");
        ReplyMsg(&wmsg->msg);
        continue;
      }

      /* Ensure session open */
      if (!ctx || curr_p != p || strcmp(curr_scheme, req->scheme) != 0) {
        if (ctx && curr_p && curr_p->close) {
          curr_p->close(ctx);
          ctx = NULL;
        }
        curr_p = p;
        (void)snprintf(curr_scheme, sizeof(curr_scheme), "%s", req->scheme);
        zzplay_host_bind(&binding, curr_scheme, worker_keep_running,
                         (void *)&worker_cancel_flag);
        if (p->open) {
          ctx = p->open(&binding.table, rep->error, sizeof(rep->error));
        }
      }

      if (req->kind == ZZPLAY_BROWSE_REQ_OPEN) {
        rep->success = (ctx != NULL);
      } else if (!ctx) {
        rep->success = 0;
        if (rep->error[0] == '\0') {
          (void)snprintf(rep->error, sizeof(rep->error), "cannot open service");
        }
      } else if (req->kind == ZZPLAY_BROWSE_REQ_LIST) {
        WorkerCollector collector;
        ZZPlayItemSink sink;
        uint32_t want_count = req->count > 0U ? req->count : ZZPLAY_BROWSE_PAGE_SIZE;

        memset(&collector, 0, sizeof(collector));
        collector.capacity = want_count;
        collector.cancelled = &worker_cancel_flag;
        collector.items = (ZZPlayBrowseItem *)zzplay_alloc(
            (size_t)want_count * sizeof(ZZPlayBrowseItem));

        if (!collector.items) {
          (void)snprintf(rep->error, sizeof(rep->error), "out of memory");
        } else {
          int ret;
          sink.user = &collector;
          sink.item = worker_sink_item;
          sink.total = worker_sink_total;

          ret = p->list ? p->list(ctx, req->node_id, req->start, want_count,
                                  &sink, rep->error, sizeof(rep->error))
                        : 0;
          if (ret) {
            rep->success = 1;
            rep->items = collector.items;
            rep->item_count = collector.count;
            rep->has_total = collector.has_total;
            rep->total = collector.total;
          } else {
            rep->success = 0;
            zzplay_free(collector.items);
            if (rep->error[0] == '\0') {
              (void)snprintf(rep->error, sizeof(rep->error),
                             worker_cancel_flag ? "cancelled" : "cannot list items");
            }
          }
        }
      } else if (req->kind == ZZPLAY_BROWSE_REQ_SEARCH) {
        WorkerCollector collector;
        ZZPlayItemSink sink;
        uint32_t max_search = ZZPLAY_BROWSE_PAGE_SIZE;

        memset(&collector, 0, sizeof(collector));
        collector.capacity = max_search;
        collector.cancelled = &worker_cancel_flag;
        collector.items = (ZZPlayBrowseItem *)zzplay_alloc(
            (size_t)max_search * sizeof(ZZPlayBrowseItem));

        if (!collector.items) {
          (void)snprintf(rep->error, sizeof(rep->error), "out of memory");
        } else {
          int ret;
          sink.user = &collector;
          sink.item = worker_sink_item;
          sink.total = worker_sink_total;

          ret = p->search ? p->search(ctx, req->query, &sink, rep->error,
                                      sizeof(rep->error))
                          : 0;
          if (ret) {
            rep->success = 1;
            rep->items = collector.items;
            rep->item_count = collector.count;
            rep->has_total = collector.has_total;
            rep->total = collector.total;
          } else {
            rep->success = 0;
            zzplay_free(collector.items);
            if (rep->error[0] == '\0') {
              (void)snprintf(rep->error, sizeof(rep->error),
                             worker_cancel_flag ? "cancelled" : "search failed");
            }
          }
        }
      } else if (req->kind == ZZPLAY_BROWSE_REQ_RESOLVE) {
        int ret = p->resolve ? p->resolve(ctx, req->item_id, &rep->resolved,
                                          rep->error, sizeof(rep->error))
                             : 0;
        rep->success = ret;
        if (!ret && rep->error[0] == '\0') {
          (void)snprintf(rep->error, sizeof(rep->error), "cannot resolve item");
        }
      } else if (req->kind == ZZPLAY_BROWSE_REQ_FAVOURITE) {
        if (!worker_provider_has_favourite(p)) {
          rep->success = 0;
          (void)snprintf(rep->error, sizeof(rep->error),
                         "service does not support favourites");
        } else {
          ZZPlayProviderItem prov_item;
          memset(&prov_item, 0, sizeof(prov_item));
          prov_item.id = req->item.id;
          prov_item.title = req->item.title;
          prov_item.subtitle = req->item.subtitle;
          prov_item.duration_ms = req->item.duration_ms;
          prov_item.kind = req->item.kind;
          prov_item.playable = req->item.playable;
          prov_item.favourite = req->item.favourite;

          rep->fav_add = req->fav_add;
          rep->success = p->favourite(ctx, &prov_item, req->fav_add,
                                      rep->error, sizeof(rep->error));
          if (!rep->success && rep->error[0] == '\0') {
            (void)snprintf(rep->error, sizeof(rep->error),
                           req->fav_add ? "cannot add favourite"
                                        : "cannot remove favourite");
          }
        }
      } else if (req->kind == ZZPLAY_BROWSE_REQ_CLOSE) {
        if (ctx && curr_p && curr_p->close) {
          curr_p->close(ctx);
          ctx = NULL;
        }
        curr_p = NULL;
        curr_scheme[0] = '\0';
        rep->success = 1;
      }

      ReplyMsg(&wmsg->msg);
    }
  }
}

int zzplay_browse_worker_start(void)
{
  struct Message *msg;

  if (worker_proc != NULL) {
    return 1;
  }
  if (!gui_port) {
    gui_port = CreateMsgPort();
    if (!gui_port) {
      return 0;
    }
  }

  worker_cancel_flag = 0;
  worker_is_active = 0;
  worker_port = NULL;

  Forbid();
  worker_proc = CreateNewProcTags(
      NP_Entry, (ULONG)worker_process,
      NP_Name, (ULONG)"ZZPlay browse worker",
      NP_StackSize, 32768UL,
      TAG_DONE);
  Permit();

  if (!worker_proc) {
    DeleteMsgPort(gui_port);
    gui_port = NULL;
    return 0;
  }

  /* Wait for child handshake */
  WaitPort(gui_port);
  msg = GetMsg(gui_port);
  if (msg) {
    worker_port = msg->mn_ReplyPort;
  }

  return worker_port ? 1 : 0;
}

int zzplay_browse_worker_active(void)
{
  return worker_is_active;
}

uint32_t zzplay_browse_worker_signal_mask(void)
{
  return gui_port ? (1UL << (ULONG)gui_port->mp_SigBit) : 0U;
}

int zzplay_browse_worker_send(const ZZPlayBrowseRequest *req,
                              const ZZPlayProvider *provider)
{
  if (!req || !provider || !worker_port || worker_is_active) {
    return 0;
  }
  worker_cancel_flag = 0;
  memset(&active_msg, 0, sizeof(active_msg));
  active_msg.msg.mn_ReplyPort = gui_port;
  active_msg.msg.mn_Length = sizeof(active_msg);
  active_msg.quit = 0;
  active_msg.req = *req;
  active_msg.provider = provider;
  worker_is_active = 1;

  PutMsg(worker_port, &active_msg.msg);
  return 1;
}

void zzplay_browse_worker_cancel(void)
{
  worker_cancel_flag = 1;
}

int zzplay_browse_worker_take_reply(ZZPlayBrowseReply *reply)
{
  ZZPlayWorkerMsg *wmsg;

  if (!gui_port || !reply) {
    return 0;
  }
  wmsg = (ZZPlayWorkerMsg *)GetMsg(gui_port);
  if (!wmsg) {
    return 0;
  }
  *reply = wmsg->reply;
  worker_is_active = 0;
  return 1;
}

void zzplay_browse_worker_release_reply(ZZPlayBrowseReply *reply)
{
  if (reply && reply->items) {
    zzplay_free((void *)reply->items);
    reply->items = NULL;
    reply->item_count = 0U;
  }
}

void zzplay_browse_worker_stop(void)
{
  ZZPlayWorkerMsg *wmsg;

  if (!worker_proc) {
    return;
  }

  /* If a request is active, cancel and wait for its reply */
  if (worker_is_active) {
    worker_cancel_flag = 1;
    if (gui_port) {
      WaitPort(gui_port);
      wmsg = (ZZPlayWorkerMsg *)GetMsg(gui_port);
      if (wmsg) {
        zzplay_browse_worker_release_reply(&wmsg->reply);
      }
    }
    worker_is_active = 0;
  }

  /* Send quit message and wait for worker exit */
  if (worker_port && gui_port) {
    memset(&quit_msg, 0, sizeof(quit_msg));
    quit_msg.msg.mn_ReplyPort = gui_port;
    quit_msg.msg.mn_Length = sizeof(quit_msg);
    quit_msg.quit = 1;

    PutMsg(worker_port, &quit_msg.msg);
    WaitPort(gui_port);
    (void)GetMsg(gui_port);
  }

  if (gui_port) {
    DeleteMsgPort(gui_port);
    gui_port = NULL;
  }
  worker_port = NULL;
  worker_proc = NULL;
  worker_is_active = 0;
  worker_cancel_flag = 0;
}

#else /* !defined(__amigaos__) && !defined(AMIGA) */

int zzplay_browse_worker_start(void) { return 0; }
int zzplay_browse_worker_active(void) { return 0; }
uint32_t zzplay_browse_worker_signal_mask(void) { return 0U; }
int zzplay_browse_worker_send(const ZZPlayBrowseRequest *req,
                              const ZZPlayProvider *provider)
{
  (void)req; (void)provider; return 0;
}
void zzplay_browse_worker_cancel(void) {}
int zzplay_browse_worker_take_reply(ZZPlayBrowseReply *reply)
{
  (void)reply; return 0;
}
void zzplay_browse_worker_release_reply(ZZPlayBrowseReply *reply)
{
  if (reply && reply->items) {
    zzplay_free((void *)reply->items);
    reply->items = NULL;
    reply->item_count = 0U;
  }
}
void zzplay_browse_worker_stop(void) {}

#endif
