/* Asynchronous worker process for service browsing and network transfers.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-browse-worker.h"

#include <stdio.h>
#include <string.h>

#include "zzplay-host.h"
#include "zzplay-http.h"
#include "zzplay-sys.h"
#include "zzplay-tags.h"
#include "zzplay-browse-gather.h"
#if defined(__amigaos__) || defined(AMIGA)

#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/dos.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include "zzplay-net.h"

typedef struct ZZPlayWorkerMsg {
  struct Message msg;
  int quit;
  ZZPlayBrowseRequest req;
  const ZZPlayProvider *provider;
  ZZPlayBrowseReply reply;
} ZZPlayWorkerMsg;

/* A mid-request message: rows as they arrive (see ZZPLAY_BROWSE_PARTIAL). */
typedef struct ZZPlayWorkerPartialMsg {
  struct Message msg;
  ZZPlayBrowseReply reply;
} ZZPlayWorkerPartialMsg;

#define ZZPLAY_BROWSE_PARTIAL 10U

typedef struct WorkerCollector {
  ZZPlayBrowseItem *items;      /* full-page buffer, freed with the reply */
  ZZPlayBrowseItem *chunk;      /* ZZPLAY_BROWSE_PARTIAL rows being grouped */
  uint32_t chunk_count;
  uint32_t capacity;
  uint32_t limit;               /* grow no further than this */
  uint32_t count;
  uint32_t total;
  int has_total;
  int grow;                     /* double the buffer while below limit */
  int oom;                      /* a growth allocation failed */
  volatile int *cancelled;
  /* Set for list and search requests, so rows are sent as they arrive. */
  struct MsgPort *gui_port;
  ZZPlayBrowseReqKind kind;
  uint32_t generation;
  uint32_t start;
} WorkerCollector;

static struct MsgPort *gui_port = NULL;
static struct MsgPort *worker_port = NULL;
static struct Process *worker_proc = NULL;
static volatile int worker_cancel_flag = 0;
static volatile int worker_stage = 0;
static char worker_stage_host[ZZPLAY_HTTP_HOST_MAX + 1U];
static int worker_is_active = 0;
static ZZPlayWorkerMsg active_msg;
static ZZPlayWorkerMsg quit_msg;
static struct Message startup_msg;

static int worker_keep_running(void *user)
{
  volatile int *cancelled = (volatile int *)user;
  return cancelled ? !(*cancelled) : 1;
}

static void worker_stage_hook(void *user, int stage, const char *host)
{
  (void)user;
  worker_stage = stage;
  if (host) {
    (void)snprintf(worker_stage_host, sizeof(worker_stage_host), "%s", host);
  } else {
    worker_stage_host[0] = '\0';
  }
}

/* Send the rows gathered so far to the GUI as a partial reply. The final
 * reply (with any remaining rows) follows when the request ends. */
static void worker_flush_chunk(WorkerCollector *c)
{
  ZZPlayWorkerPartialMsg *pmsg;
  ZZPlayBrowseItem *copy;

  if (!c || !c->gui_port || c->chunk_count == 0U) {
    return;
  }
  pmsg = (ZZPlayWorkerPartialMsg *)zzplay_alloc(sizeof(*pmsg));
  copy = (ZZPlayBrowseItem *)zzplay_alloc(
      (size_t)c->chunk_count * sizeof(ZZPlayBrowseItem));
  if (!pmsg || !copy) {
    zzplay_free(pmsg);
    zzplay_free(copy);
    c->chunk_count = 0U;
    return;
  }
  memcpy(copy, c->chunk, (size_t)c->chunk_count * sizeof(ZZPlayBrowseItem));
  memset(pmsg, 0, sizeof(*pmsg));
  pmsg->msg.mn_ReplyPort = c->gui_port;
  pmsg->msg.mn_Length = sizeof(*pmsg);
  pmsg->reply.kind = c->kind;
  pmsg->reply.generation = c->generation;
  pmsg->reply.success = 1;
  pmsg->reply.items = copy;
  pmsg->reply.item_count = c->chunk_count;
  pmsg->reply.start = c->start;
  pmsg->reply.total = c->total;
  pmsg->reply.has_total = c->has_total;
  pmsg->reply.is_partial = 1;
  PutMsg(c->gui_port, &pmsg->msg);
  c->chunk_count = 0U;
}
/* Double the row buffer of a growing collector (GATHER), capped at its
 * limit. Returns 1 on success. */
static int worker_collector_grow(WorkerCollector *c)
{
  uint32_t grown = c->capacity * 2U;
  ZZPlayBrowseItem *fresh;

  if (grown > c->limit) {
    grown = c->limit;
  }
  if (grown <= c->capacity) {
    return 0;
  }
  fresh = (ZZPlayBrowseItem *)zzplay_alloc(
      (size_t)grown * sizeof(ZZPlayBrowseItem));
  if (!fresh) {
    return 0;
  }
  memcpy(fresh, c->items, (size_t)c->count * sizeof(ZZPlayBrowseItem));
  zzplay_free(c->items);
  c->items = fresh;
  c->capacity = grown;
  return 1;
}


static int worker_sink_item(void *user, const ZZPlayProviderItem *item)
{
  WorkerCollector *c = (WorkerCollector *)user;
  ZZPlayBrowseItem *dst;

  if (!c || !item || *(c->cancelled)) {
    return 1;
  }
  if (c->count >= c->capacity) {
    if (!c->grow) {
      return 1;
    }
    if (!worker_collector_grow(c)) {
      c->oom = 1;
      return 1;
    }
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
  dst->editable = item->editable;
  c->count++;

  /* Group rows so the GUI can show them while the rest arrive. */
  if (c->chunk) {
    memcpy(&c->chunk[c->chunk_count], dst, sizeof(*dst));
    c->chunk_count++;
    if (c->chunk_count >= ZZPLAY_BROWSE_PARTIAL) {
      worker_flush_chunk(c);
    }
  }
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

static int worker_provider_has_link(const ZZPlayProvider *p)
{
  if (!p ||
      p->size < offsetof(ZZPlayProvider, link_choose) +
                    sizeof(p->link_choose)) {
    return 0;
  }
  return p->link_start != NULL && p->link_poll != NULL &&
         p->link_choose != NULL;
}

static int worker_provider_has_form(const ZZPlayProvider *p)
{
  if (!p || p->size < offsetof(ZZPlayProvider, form_delete) +
                        sizeof(p->form_delete)) {
    return 0;
  }
  return p->form_get != NULL && p->form_submit != NULL;
}


/* The provider list call, shaped for ZZPlayGather. */
typedef struct WorkerGatherLister {
  const ZZPlayProvider *provider;
  void *ctx;
} WorkerGatherLister;

static int worker_gather_list(void *user, const char *node, uint32_t start,
                              uint32_t count, const ZZPlayItemSink *sink,
                              char *err, uint32_t err_size)
{
  WorkerGatherLister *lister = (WorkerGatherLister *)user;

  if (!lister || !lister->provider || !lister->provider->list) {
    if (err && err_size > 0U) {
      (void)snprintf(err, err_size, "service cannot list items");
    }
    return 0;
  }
  return lister->provider->list(lister->ctx, node, start, count, sink,
                                err, err_size);
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

  /* Always answer the handshake, with no port when this process has
   * none, so the GUI never waits for a worker that is not there. The
   * Forbid lasts until the process ends when it gives up here. */
  if (!my_port) {
    Forbid();
  }
  startup_msg.mn_ReplyPort = my_port;
  startup_msg.mn_Length = sizeof(startup_msg);
  PutMsg(gui_port, &startup_msg);
  if (!my_port) {
    return;
  }

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
        zzplay_net_release_process();
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
      rep->is_partial = 0;
      rep->error[0] = '\0';
      worker_stage = 0;
      worker_stage_host[0] = '\0';

      if (!p) {
        (void)snprintf(rep->error, sizeof(rep->error), "no provider");
        ReplyMsg(&wmsg->msg);
        continue;
      }

      /* Ensure session open; changed settings reopen it. */
      if (!ctx || curr_p != p || strcmp(curr_scheme, req->scheme) != 0 ||
          req->reopen) {
        if (ctx && curr_p && curr_p->close) {
          curr_p->close(ctx);
          ctx = NULL;
        }
        curr_p = p;
        (void)snprintf(curr_scheme, sizeof(curr_scheme), "%s", req->scheme);
        zzplay_host_bind(&binding, curr_scheme, worker_keep_running,
                         (void *)&worker_cancel_flag);
        binding.stage = worker_stage_hook;
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
        collector.kind = req->kind;
        collector.generation = req->generation;
        collector.start = req->start;
        collector.gui_port = gui_port;
        collector.items = (ZZPlayBrowseItem *)zzplay_alloc(
            (size_t)want_count * sizeof(ZZPlayBrowseItem));
        collector.chunk = (ZZPlayBrowseItem *)zzplay_alloc(
            (size_t)ZZPLAY_BROWSE_PARTIAL * sizeof(ZZPlayBrowseItem));

        if (!collector.items || !collector.chunk) {
          zzplay_free(collector.items);
          zzplay_free(collector.chunk);
          collector.items = NULL;
          collector.chunk = NULL;
          (void)snprintf(rep->error, sizeof(rep->error), "out of memory");
        } else {
          int ret;
          sink.user = &collector;
          sink.item = worker_sink_item;
          sink.total = worker_sink_total;

          ret = p->list ? p->list(ctx, req->node_id, req->start, want_count,
                                  &sink, rep->error, sizeof(rep->error))
                        : 0;
          worker_flush_chunk(&collector);
          zzplay_free(collector.chunk);
          collector.chunk = NULL;
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
        collector.kind = req->kind;
        collector.generation = req->generation;
        collector.start = 0U;
        collector.gui_port = gui_port;
        collector.items = (ZZPlayBrowseItem *)zzplay_alloc(
            (size_t)max_search * sizeof(ZZPlayBrowseItem));
        collector.chunk = (ZZPlayBrowseItem *)zzplay_alloc(
            (size_t)ZZPLAY_BROWSE_PARTIAL * sizeof(ZZPlayBrowseItem));

        if (!collector.items || !collector.chunk) {
          zzplay_free(collector.items);
          zzplay_free(collector.chunk);
          collector.items = NULL;
          collector.chunk = NULL;
          (void)snprintf(rep->error, sizeof(rep->error), "out of memory");
        } else {
          int ret;
          char query[ZZPLAY_PROVIDER_TEXT_MAX];

          sink.user = &collector;
          sink.item = worker_sink_item;
          sink.total = worker_sink_total;

          /* Typed text is Latin-1; the services expect UTF-8. */
          (void)zzplay_tags_text_to_utf8(req->query, query, sizeof(query));
          ret = p->search ? p->search(ctx, query, &sink, rep->error,
                                      sizeof(rep->error))
                          : 0;
          worker_flush_chunk(&collector);
          zzplay_free(collector.chunk);
          collector.chunk = NULL;
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
          (void)snprintf(rep->item_id, sizeof(rep->item_id), "%s",
                         req->item.id);
          rep->success = p->favourite(ctx, &prov_item, req->fav_add,
                                      rep->error, sizeof(rep->error));
          if (!rep->success && rep->error[0] == '\0') {
            (void)snprintf(rep->error, sizeof(rep->error),
                           req->fav_add ? "cannot add favourite"
                                        : "cannot remove favourite");
          }
        }
      } else if (req->kind == ZZPLAY_BROWSE_REQ_FORM_GET) {
        ZZPlayProviderForm form;
        uint32_t i;
        memset(&form, 0, sizeof(form));
        if (!worker_provider_has_form(p)) {
          (void)snprintf(rep->error, sizeof(rep->error),
                         "service does not support forms");
        } else {
          (void)snprintf(rep->item_id, sizeof(rep->item_id), "%s",
                         req->item_id);
          rep->success = p->form_get(ctx, req->item_id, &form,
                                     rep->error, sizeof(rep->error));
          if (rep->success) {
            rep->form = form;
            rep->form.values = NULL;
            if (rep->form.field_count > ZZPLAY_PROVIDER_FORM_FIELDS_MAX) {
              rep->form.field_count = ZZPLAY_PROVIDER_FORM_FIELDS_MAX;
            }
            (void)snprintf(rep->form_title, sizeof(rep->form_title), "%s",
                           form.title ? form.title : "Edit");
            rep->form.title = rep->form_title;
            for (i = 0U; i < rep->form.field_count; i++) {
              (void)snprintf(rep->form_values[i],
                             sizeof(rep->form_values[i]), "%s",
                             form.values && form.values[i] ? form.values[i] : "");
            }
          }
        }
      } else if (req->kind == ZZPLAY_BROWSE_REQ_FORM_SUBMIT) {
        const char *values[ZZPLAY_PROVIDER_FORM_FIELDS_MAX];
        uint32_t i;
        if (!worker_provider_has_form(p)) {
          (void)snprintf(rep->error, sizeof(rep->error),
                         "service does not support forms");
        } else {
          for (i = 0U; i < ZZPLAY_PROVIDER_FORM_FIELDS_MAX; i++) {
            values[i] = req->form_values[i];
          }
          rep->success = p->form_submit(ctx, req->item_id, values,
                                        req->form_value_count, rep->error,
                                        sizeof(rep->error));
        }
      } else if (req->kind == ZZPLAY_BROWSE_REQ_FORM_DELETE) {
        if (!worker_provider_has_form(p) || p->form_delete == NULL) {
          (void)snprintf(rep->error, sizeof(rep->error),
                         "service does not support deleting this row");
        } else {
          rep->success = p->form_delete(ctx, req->item_id, rep->error,
                                        sizeof(rep->error));
        }
      } else if (req->kind == ZZPLAY_BROWSE_REQ_LINK_START) {
        if (!worker_provider_has_link(p)) {
          (void)snprintf(rep->error, sizeof(rep->error),
                         "service does not support sign-in");
        } else {
          rep->success = p->link_start(ctx, &rep->link_start,
                                       rep->error, sizeof(rep->error));
          if (!rep->success && rep->error[0] == '\0') {
            (void)snprintf(rep->error, sizeof(rep->error),
                           "cannot start sign-in");
          }
        }
      } else if (req->kind == ZZPLAY_BROWSE_REQ_LINK_POLL) {
        if (!worker_provider_has_link(p)) {
          (void)snprintf(rep->error, sizeof(rep->error),
                         "service does not support sign-in");
        } else {
          rep->success = p->link_poll(ctx, req->link_id,
                                      &rep->link_result,
                                      rep->error, sizeof(rep->error));
          if (!rep->success && rep->error[0] == '\0') {
            (void)snprintf(rep->error, sizeof(rep->error),
                           worker_cancel_flag ? "Cancelled"
                                              : "sign-in failed");
          }
        }
      } else if (req->kind == ZZPLAY_BROWSE_REQ_LINK_CHOOSE) {
        if (!worker_provider_has_link(p)) {
          (void)snprintf(rep->error, sizeof(rep->error),
                         "service does not support sign-in");
        } else {
          rep->success = p->link_choose(ctx, req->choice_id,
                                        &rep->link_result,
                                        rep->error, sizeof(rep->error));
          if (!rep->success && rep->error[0] == '\0') {
            (void)snprintf(rep->error, sizeof(rep->error),
                           "cannot select server");
          }
        }
      } else if (req->kind == ZZPLAY_BROWSE_REQ_GATHER) {
        WorkerCollector collector;
        ZZPlayItemSink sink;
        ZZPlayGather gather;
        WorkerGatherLister lister;
        uint32_t limit = req->count > 0U ? req->count : 1U;
        uint32_t first = limit < ZZPLAY_BROWSE_PAGE_SIZE
                             ? limit
                             : ZZPLAY_BROWSE_PAGE_SIZE;
        int truncated = 0;

        memset(&collector, 0, sizeof(collector));
        collector.capacity = first;
        collector.limit = limit;
        collector.grow = 1;
        collector.cancelled = &worker_cancel_flag;
        collector.kind = req->kind;
        collector.generation = req->generation;
        collector.start = 0U;
        collector.gui_port = gui_port;
        collector.items = (ZZPlayBrowseItem *)zzplay_alloc(
            (size_t)first * sizeof(ZZPlayBrowseItem));
        collector.chunk = (ZZPlayBrowseItem *)zzplay_alloc(
            (size_t)ZZPLAY_BROWSE_PARTIAL * sizeof(ZZPlayBrowseItem));

        if (!collector.items || !collector.chunk) {
          zzplay_free(collector.items);
          zzplay_free(collector.chunk);
          collector.items = NULL;
          collector.chunk = NULL;
          (void)snprintf(rep->error, sizeof(rep->error), "out of memory");
        } else {
          int ret;

          sink.user = &collector;
          sink.item = worker_sink_item;
          sink.total = worker_sink_total;
          lister.provider = p;
          lister.ctx = ctx;

          memset(&gather, 0, sizeof(gather));
          gather.user = &lister;
          gather.list = worker_gather_list;
          gather.cancelled = &worker_cancel_flag;

          ret = zzplay_gather_collection(&gather, req->node_id, limit,
                                         &sink, &truncated, rep->error,
                                         sizeof(rep->error));
          worker_flush_chunk(&collector);
          zzplay_free(collector.chunk);
          collector.chunk = NULL;
          if (ret && !collector.oom) {
            rep->success = 1;
            rep->items = collector.items;
            rep->item_count = collector.count;
            rep->truncated = truncated;
          } else {
            zzplay_free(collector.items);
            rep->success = 0;
            if (collector.oom) {
              (void)snprintf(rep->error, sizeof(rep->error),
                             "out of memory");
            } else if (rep->error[0] == '\0') {
              (void)snprintf(rep->error, sizeof(rep->error),
                             worker_cancel_flag ? "cancelled"
                                                : "cannot collect tracks");
            }
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

  /* Wait for the child's handshake; it always comes, with no port when
   * the child could not make one (it has ended by then). */
  for (;;) {
    WaitPort(gui_port);
    msg = GetMsg(gui_port);
    if (msg == &startup_msg) {
      break;
    }
  }
  worker_port = startup_msg.mn_ReplyPort;
  if (!worker_port) {
    worker_proc = NULL;
    DeleteMsgPort(gui_port);
    gui_port = NULL;
    return 0;
  }
  return 1;
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

int zzplay_browse_worker_get_stage(char *host, uint32_t host_size)
{
  if (host && host_size > 0U) {
    Forbid();
    (void)snprintf(host, host_size, "%s", worker_stage_host);
    Permit();
  }
  return worker_is_active ? worker_stage : 0;
}

int zzplay_browse_worker_take_reply(ZZPlayBrowseReply *reply)
{
  struct Message *m;

  if (!gui_port || !reply) {
    return 0;
  }
  m = GetMsg(gui_port);
  if (!m) {
    return 0;
  }
  if (m->mn_Length == sizeof(ZZPlayWorkerPartialMsg)) {
    /* Rows that arrived while the request runs; more follow. */
    ZZPlayWorkerPartialMsg *pmsg = (ZZPlayWorkerPartialMsg *)m;
    *reply = pmsg->reply;
    zzplay_free(pmsg);
    return 1;
  }
  {
    ZZPlayWorkerMsg *wmsg = (ZZPlayWorkerMsg *)m;
    *reply = wmsg->reply;
    worker_is_active = 0;
  }
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

/* Wait until `want` comes back to the GUI port. Rows arriving meanwhile
 * (partial replies) and the replies of other messages are released. */
static void wait_for_worker_message(struct Message *want)
{
  for (;;) {
    struct Message *m;

    while ((m = GetMsg(gui_port)) != NULL) {
      if (m->mn_Length == sizeof(ZZPlayWorkerPartialMsg)) {
        ZZPlayWorkerPartialMsg *pmsg = (ZZPlayWorkerPartialMsg *)m;
        zzplay_browse_worker_release_reply(&pmsg->reply);
        zzplay_free(pmsg);
        continue;
      }
      if (m == &active_msg.msg) {
        zzplay_browse_worker_release_reply(&active_msg.reply);
      }
      if (m == want) {
        return;
      }
    }
    WaitPort(gui_port);
  }
}

void zzplay_browse_worker_stop(void)
{
  if (!worker_proc) {
    return;
  }

  /* A running request is cancelled and its final reply awaited: the
   * worker still owns the message (and its rows) until then. */
  if (worker_is_active && gui_port) {
    worker_cancel_flag = 1;
    wait_for_worker_message(&active_msg.msg);
    worker_is_active = 0;
  }

  /* Quit, and wait for that very message: once it is back the worker
   * has deleted its port and never sends again. */
  if (worker_port && gui_port) {
    memset(&quit_msg, 0, sizeof(quit_msg));
    quit_msg.msg.mn_ReplyPort = gui_port;
    quit_msg.msg.mn_Length = sizeof(quit_msg);
    quit_msg.quit = 1;

    PutMsg(worker_port, &quit_msg.msg);
    wait_for_worker_message(&quit_msg.msg);
  }

  if (gui_port) {
    DeleteMsgPort(gui_port);
    gui_port = NULL;
  }
  worker_port = NULL;
  worker_proc = NULL;
  worker_is_active = 0;
  worker_cancel_flag = 0;
  worker_stage = 0;
  worker_stage_host[0] = '\0';
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
int zzplay_browse_worker_get_stage(char *host, uint32_t host_size)
{
  (void)host; (void)host_size; return 0;
}
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
