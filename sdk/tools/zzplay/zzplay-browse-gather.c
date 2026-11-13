/* Collection gathering for ZZPlay's Browse window.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <string.h>

#include "zzplay-browse-gather.h"
#include "zzplay-sys.h"

typedef struct GatherState {
  const ZZPlayGather *g;
  const ZZPlayItemSink *out;
  uint32_t limit;
  uint32_t collected;
  uint32_t depth;        /* 0: the collection, 1: a folder inside it */
  uint32_t rows;         /* rows of the page being listed */
  uint32_t total;        /* folder total when the page reported one */
  int has_total;
  int more_after_limit;  /* a playable item existed past the limit */
  int out_stopped;       /* out->item() asked to stop */
  char *subs;            /* playable folders found in the collection */
  uint32_t sub_count;
  uint32_t sub_capacity;
  int sub_oom;
  char *err;
  uint32_t err_size;
} GatherState;

static void gather_err(GatherState *st, const char *text)
{
  if (st->err && st->err_size > 0U && st->err[0] == '\0') {
    (void)snprintf(st->err, st->err_size, "%s", text);
  }
}

static void gather_subs_add(GatherState *st, const char *id)
{
  char *dst;

  if (st->sub_count >= st->sub_capacity) {
    uint32_t grown = st->sub_capacity == 0U ? 16U : st->sub_capacity * 2U;
    char *fresh;

    fresh = (char *)zzplay_alloc((size_t)grown *
                                 ZZPLAY_PROVIDER_TEXT_MAX);
    if (!fresh) {
      st->sub_oom = 1;
      return;
    }
    if (st->subs && st->sub_count > 0U) {
      memcpy(fresh, st->subs,
             (size_t)st->sub_count * ZZPLAY_PROVIDER_TEXT_MAX);
    }
    zzplay_free(st->subs);
    st->subs = fresh;
    st->sub_capacity = grown;
  }
  dst = st->subs + (size_t)st->sub_count * ZZPLAY_PROVIDER_TEXT_MAX;
  memset(dst, 0, ZZPLAY_PROVIDER_TEXT_MAX);
  (void)snprintf(dst, ZZPLAY_PROVIDER_TEXT_MAX, "%s",
                 id ? id : "");
  st->sub_count++;
}

static int gather_walk_over(const GatherState *st)
{
  return st->more_after_limit || st->out_stopped || st->sub_oom ||
         (st->g->cancelled && *(st->g->cancelled));
}

static int gather_sink_item(void *user, const ZZPlayProviderItem *item)
{
  GatherState *st = (GatherState *)user;

  if (!item) {
    return 0;
  }
  st->rows++;
  if (st->g->cancelled && *(st->g->cancelled)) {
    return 1;
  }
  if (item->kind == ZZPLAY_ITEM_FOLDER) {
    /* Only folders directly in the collection are listed (two levels). */
    if (st->depth == 0U && item->playable != 0U && item->id) {
      if (st->collected >= st->limit) {
        st->more_after_limit = 1;
        return 1;
      }
      gather_subs_add(st, item->id);
    }
    return gather_walk_over(st) ? 1 : 0;
  }
  if (item->playable == 0U) {
    return 0;
  }
  if (st->collected >= st->limit) {
    st->more_after_limit = 1;
    return 1;
  }
  st->collected++;
  if (!st->out->item || st->out->item(st->out->user, item) != 0) {
    st->out_stopped = 1;
    return 1;
  }
  return 0;
}

static void gather_sink_total(void *user, uint32_t total)
{
  GatherState *st = (GatherState *)user;
  st->total = total;
  st->has_total = 1;
}

/* List every page of one folder. Returns 0 when a listing failed or the
 * walk was cancelled; 1 when the folder's rows are exhausted or the walk
 * asked to stop (see gather_walk_over). */
static int gather_list_pages(GatherState *st, const char *node,
                             uint32_t depth)
{
  ZZPlayItemSink sink;
  uint32_t start = 0U;

  st->depth = depth;
  sink.user = st;
  sink.item = gather_sink_item;
  sink.total = gather_sink_total;

  for (;;) {
    int ret;

    if (st->g->cancelled && *(st->g->cancelled)) {
      return 0;
    }
    st->rows = 0U;
    st->total = 0U;
    st->has_total = 0;
    ret = st->g->list(st->g->user, node, start, ZZPLAY_GATHER_PAGE_SIZE,
                      &sink, st->err, st->err_size);
    if (st->g->progress) {
      st->g->progress(st->g->user, st->collected);
    }
    if (!ret) {
      return 0;
    }
    if (gather_walk_over(st)) {
      return 1;
    }
    start += st->rows;
    if (st->rows == 0U) {
      return 1;
    }
    if (st->has_total) {
      if (start >= st->total) {
        return 1;
      }
    } else if (st->rows < ZZPLAY_GATHER_PAGE_SIZE) {
      return 1;
    }
  }
}

int zzplay_gather_collection(const ZZPlayGather *g, const char *node,
                             uint32_t limit, const ZZPlayItemSink *out,
                             int *truncated, char *err, uint32_t err_size)
{
  GatherState st;
  uint32_t i;
  int ok;

  if (truncated) {
    *truncated = 0;
  }
  if (err && err_size > 0U) {
    err[0] = '\0';
  }
  if (!g || !g->list || !node || node[0] == '\0' || !out) {
    if (err && err_size > 0U) {
      (void)snprintf(err, err_size, "invalid request");
    }
    return 0;
  }

  memset(&st, 0, sizeof(st));
  st.g = g;
  st.out = out;
  st.limit = limit;
  st.err = err;
  st.err_size = err_size;

  ok = gather_list_pages(&st, node, 0U);
  for (i = 0U; ok && !st.more_after_limit && i < st.sub_count; i++) {
    ok = gather_list_pages(&st,
                           st.subs + (size_t)i * ZZPLAY_PROVIDER_TEXT_MAX,
                           1U);
  }

  if (st.sub_oom) {
    gather_err(&st, "out of memory");
    ok = 0;
  }
  if (g->cancelled && *(g->cancelled)) {
    gather_err(&st, "cancelled");
    ok = 0;
  }
  zzplay_free(st.subs);
  if (truncated) {
    *truncated = st.more_after_limit;
  }
  return ok;
}
