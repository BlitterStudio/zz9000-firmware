/* Host-testable state for ZZPlay's service browser.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-browse-model.h"

#include <stdio.h>
#include <string.h>

#include "zzplay-sys.h"

static void copy_string(char *dst, size_t dst_size, const char *src)
{
  if (!dst || dst_size == 0U) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  (void)snprintf(dst, dst_size, "%s", src);
}

static void free_frame(ZZPlayBrowseFrame *frame)
{
  if (frame && frame->items) {
    zzplay_free(frame->items);
    frame->items = NULL;
    frame->item_count = 0U;
    frame->item_capacity = 0U;
  }
}

/* --- Session list cache -------------------------------------------- */

static ZZPlayBrowseCacheEntry *cache_lookup(ZZPlayBrowseModel *model,
                                            const char *scheme, int is_search,
                                            const char *node_or_query,
                                            uint32_t start, int touch)
{
  uint32_t i;

  for (i = 0U; i < ZZPLAY_BROWSE_CACHE_MAX; i++) {
    ZZPlayBrowseCacheEntry *e = &model->cache[i];
    if (e->items == NULL) {
      continue;
    }
    if (e->is_search == (is_search ? 1 : 0) && e->start == start &&
        strcmp(e->scheme, scheme) == 0 &&
        strcmp(e->is_search ? e->query : e->node_id, node_or_query) == 0) {
      if (touch) {
        e->last_used = ++model->cache_clock;
      }
      return e;
    }
  }
  return NULL;
}

static ZZPlayBrowseCacheEntry *cache_find(ZZPlayBrowseModel *model,
                                          const char *scheme, int is_search,
                                          const char *node_or_query,
                                          uint32_t start)
{
  return cache_lookup(model, scheme, is_search, node_or_query, start, 1);
}

/* Keep the remembered copy of the current view in step with the
 * selection and scroll the user leaves it in. */
static void cache_save_view_state(const ZZPlayBrowseModel *model)
{
  const ZZPlayBrowseServiceEntry *svc;
  ZZPlayBrowseCacheEntry *e;

  if (model->current_service < 0) {
    return;
  }
  svc = &model->services[model->current_service];
  e = cache_lookup((ZZPlayBrowseModel *)model, svc->scheme, model->is_search,
                   model->is_search ? model->search_query : model->current_node,
                   0U, 0);
  if (e) {
    e->selection = model->selection;
    e->scroll = model->scroll;
  }
}

/* Remember `count` items for (scheme, node or query, start). The least
 * recently used entry is dropped when the cache is full. */
static void cache_put(ZZPlayBrowseModel *model, const char *scheme,
                      int is_search, const char *node_or_query,
                      uint32_t start, const ZZPlayBrowseItem *items,
                      uint32_t count, int32_t selection, uint32_t scroll)
{
  ZZPlayBrowseCacheEntry *e = cache_find(model, scheme, is_search,
                                         node_or_query, start);
  ZZPlayBrowseItem *copy = NULL;

  if (count > 0U) {
    copy = (ZZPlayBrowseItem *)zzplay_alloc(
        (size_t)count * sizeof(ZZPlayBrowseItem));
    if (!copy) {
      return;
    }
    memcpy(copy, items, (size_t)count * sizeof(ZZPlayBrowseItem));
  }

  if (!e) {
    uint32_t i;
    uint32_t oldest = 0U;

    for (i = 0U; i < ZZPLAY_BROWSE_CACHE_MAX; i++) {
      if (model->cache[i].items == NULL) {
        oldest = i;
        break;
      }
      if (model->cache[i].last_used < model->cache[oldest].last_used) {
        oldest = i;
      }
    }
    e = &model->cache[oldest];
    if (e->items) {
      zzplay_free(e->items);
    }
  } else if (e->items) {
    zzplay_free(e->items);
  }

  memset(e, 0, sizeof(*e));
  copy_string(e->scheme, sizeof(e->scheme), scheme);
  if (is_search) {
    copy_string(e->query, sizeof(e->query), node_or_query);
  } else {
    copy_string(e->node_id, sizeof(e->node_id), node_or_query);
  }
  e->is_search = is_search ? 1 : 0;
  e->start = start;
  e->items = copy;
  e->item_count = count;
  e->selection = selection;
  e->scroll = scroll;
  e->last_used = ++model->cache_clock;
}

/* Show a remembered list at once. Returns 1 on a hit. The model gets
 * its own copy, so the remembered entry stays untouched. */
static int cache_restore(ZZPlayBrowseModel *model, const char *scheme,
                         int is_search, const char *node_or_query)
{
  ZZPlayBrowseCacheEntry *e = cache_find(model, scheme, is_search,
                                         node_or_query, 0U);
  ZZPlayBrowseItem *copy = NULL;

  if (!e) {
    return 0;
  }
  if (e->item_count > 0U) {
    copy = (ZZPlayBrowseItem *)zzplay_alloc(
        (size_t)e->item_count * sizeof(ZZPlayBrowseItem));
    if (!copy) {
      return 0;
    }
    memcpy(copy, e->items, (size_t)e->item_count * sizeof(ZZPlayBrowseItem));
  }
  if (model->items) {
    zzplay_free(model->items);
  }
  model->items = copy;
  model->item_count = e->item_count;
  model->item_capacity = e->item_count;
  model->selection = e->selection;
  model->scroll = e->scroll;
  model->total = 0U;
  model->has_total = 0;
  model->has_more = 0;
  return 1;
}

void zzplay_browse_model_init(ZZPlayBrowseModel *model)
{
  if (!model) {
    return;
  }
  memset(model, 0, sizeof(*model));
  model->current_service = -1;
  model->selection = -1;
  model->saved_selection = -1;
}

void zzplay_browse_model_free(ZZPlayBrowseModel *model)
{
  uint32_t i;
  if (!model) {
    return;
  }
  if (model->items) {
    zzplay_free(model->items);
    model->items = NULL;
    model->item_count = 0U;
    model->item_capacity = 0U;
  }
  for (i = 0U; i < model->stack_depth; i++) {
    free_frame(&model->stack[i]);
  }
  model->stack_depth = 0U;
  for (i = 0U; i < ZZPLAY_BROWSE_CACHE_MAX; i++) {
    if (model->cache[i].items) {
      zzplay_free(model->cache[i].items);
      model->cache[i].items = NULL;
    }
  }
}

int zzplay_browse_model_add_service(ZZPlayBrowseModel *model,
                                    const char *scheme,
                                    const char *name,
                                    const ZZPlayProvider *provider)
{
  ZZPlayBrowseServiceEntry *entry;

  if (!model || !scheme || !name ||
      model->service_count >= ZZPLAY_BROWSE_SERVICES_MAX) {
    return 0;
  }
  entry = &model->services[model->service_count];
  copy_string(entry->scheme, sizeof(entry->scheme), scheme);
  copy_string(entry->name, sizeof(entry->name), name);
  entry->provider = provider;
  model->service_count++;
  return 1;
}

int zzplay_browse_model_select_service(ZZPlayBrowseModel *model, uint32_t index,
                                       ZZPlayBrowseRequest *out_req)
{
  uint32_t i;

  if (!model || index >= model->service_count) {
    return 0;
  }
  cache_save_view_state(model);
  model->generation++;
  if (model->items) {
    zzplay_free(model->items);
    model->items = NULL;
    model->item_count = 0U;
    model->item_capacity = 0U;
  }
  for (i = 0U; i < model->stack_depth; i++) {
    free_frame(&model->stack[i]);
  }
  model->stack_depth = 0U;

  model->current_service = (int32_t)index;
  model->current_node[0] = '\0';
  model->current_title[0] = '\0';
  model->is_search = 0;
  model->search_query[0] = '\0';
  model->selection = -1;
  model->scroll = 0U;
  model->total = 0U;
  model->has_total = 0;
  model->has_more = 0;
  model->busy = 1;
  model->status_message[0] = '\0';

  if (cache_restore(model, model->services[index].scheme, 0, "")) {
    model->busy = 0;
    if (out_req) {
      memset(out_req, 0, sizeof(*out_req));
      out_req->kind = ZZPLAY_BROWSE_REQ_NONE;
    }
    return 1;
  }

  if (out_req) {
    memset(out_req, 0, sizeof(*out_req));
    out_req->kind = ZZPLAY_BROWSE_REQ_LIST;
    out_req->generation = model->generation;
    copy_string(out_req->scheme, sizeof(out_req->scheme),
                model->services[index].scheme);
    out_req->node_id[0] = '\0';
    out_req->start = 0U;
    out_req->count = ZZPLAY_BROWSE_PAGE_SIZE;
  }
  return 1;
}

const ZZPlayBrowseServiceEntry *zzplay_browse_model_current_service(
    const ZZPlayBrowseModel *model)
{
  if (!model || model->current_service < 0 ||
      (uint32_t)model->current_service >= model->service_count) {
    return NULL;
  }
  return &model->services[model->current_service];
}

int zzplay_browse_model_enter_folder(ZZPlayBrowseModel *model, int32_t index,
                                     ZZPlayBrowseRequest *out_req)
{
  ZZPlayBrowseFrame *frame;
  const ZZPlayBrowseItem *item;

  if (!model || index < 0 || (uint32_t)index >= model->item_count ||
      model->current_service < 0) {
    return 0;
  }
  item = &model->items[index];
  if (item->kind != ZZPLAY_ITEM_FOLDER) {
    return 0;
  }
  if (model->stack_depth >= ZZPLAY_BROWSE_STACK_MAX) {
    return 0;
  }

  cache_save_view_state(model);
  model->generation++;
  frame = &model->stack[model->stack_depth];
  memset(frame, 0, sizeof(*frame));
  copy_string(frame->node_id, sizeof(frame->node_id), model->current_node);
  copy_string(frame->title, sizeof(frame->title), model->current_title);
  frame->is_search = model->is_search;
  copy_string(frame->search_query, sizeof(frame->search_query),
              model->search_query);
  frame->selection = model->selection;
  frame->scroll = model->scroll;
  frame->items = model->items;
  frame->item_count = model->item_count;
  frame->item_capacity = model->item_capacity;
  frame->total = model->total;
  frame->has_total = model->has_total;
  frame->has_more = model->has_more;
  model->stack_depth++;

  /* Adopt new folder */
  copy_string(model->current_node, sizeof(model->current_node), item->id);
  copy_string(model->current_title, sizeof(model->current_title), item->title);
  model->is_search = 0;
  model->search_query[0] = '\0';
  model->items = NULL;
  model->item_count = 0U;
  model->item_capacity = 0U;
  model->total = 0U;
  model->has_total = 0;
  model->has_more = 0;
  model->selection = -1;
  model->scroll = 0U;
  model->busy = 1;
  model->status_message[0] = '\0';

  if (cache_restore(model, model->services[model->current_service].scheme, 0,
                    model->current_node)) {
    model->busy = 0;
    if (out_req) {
      memset(out_req, 0, sizeof(*out_req));
      out_req->kind = ZZPLAY_BROWSE_REQ_NONE;
    }
    return 1;
  }

  if (out_req) {
    memset(out_req, 0, sizeof(*out_req));
    out_req->kind = ZZPLAY_BROWSE_REQ_LIST;
    out_req->generation = model->generation;
    copy_string(out_req->scheme, sizeof(out_req->scheme),
                model->services[model->current_service].scheme);
    copy_string(out_req->node_id, sizeof(out_req->node_id), model->current_node);
    out_req->start = 0U;
    out_req->count = ZZPLAY_BROWSE_PAGE_SIZE;
  }
  return 1;
}

int zzplay_browse_model_can_back(const ZZPlayBrowseModel *model)
{
  return model && model->stack_depth > 0U;
}

int zzplay_browse_model_back(ZZPlayBrowseModel *model,
                             ZZPlayBrowseRequest *out_req)
{
  ZZPlayBrowseFrame *frame;

  if (!model || model->stack_depth == 0U) {
    return 0;
  }
  model->generation++;
  if (model->items) {
    zzplay_free(model->items);
    model->items = NULL;
    model->item_count = 0U;
    model->item_capacity = 0U;
  }

  model->stack_depth--;
  frame = &model->stack[model->stack_depth];

  copy_string(model->current_node, sizeof(model->current_node), frame->node_id);
  copy_string(model->current_title, sizeof(model->current_title), frame->title);
  model->is_search = frame->is_search;
  copy_string(model->search_query, sizeof(model->search_query),
              frame->search_query);
  model->items = frame->items;
  model->item_count = frame->item_count;
  model->item_capacity = frame->item_capacity;
  model->total = frame->total;
  model->has_total = frame->has_total;
  model->has_more = frame->has_more;
  model->selection = frame->selection;
  model->scroll = frame->scroll;
  model->busy = 0;
  model->status_message[0] = '\0';

  memset(frame, 0, sizeof(*frame));

  if (out_req) {
    memset(out_req, 0, sizeof(*out_req));
    out_req->kind = ZZPLAY_BROWSE_REQ_NONE;
  }
  return 1;
}

int zzplay_browse_model_search(ZZPlayBrowseModel *model, const char *query,
                               ZZPlayBrowseRequest *out_req)
{
  ZZPlayBrowseFrame *frame;

  if (!model || !query || model->current_service < 0) {
    return 0;
  }
  cache_save_view_state(model);
  model->generation++;

  /* If starting a search from a browse view, save view on stack */
  if (!model->is_search && model->stack_depth < ZZPLAY_BROWSE_STACK_MAX) {
    frame = &model->stack[model->stack_depth];
    memset(frame, 0, sizeof(*frame));
    copy_string(frame->node_id, sizeof(frame->node_id), model->current_node);
    copy_string(frame->title, sizeof(frame->title), model->current_title);
    frame->is_search = 0;
    frame->search_query[0] = '\0';
    frame->selection = model->selection;
    frame->scroll = model->scroll;
    frame->items = model->items;
    frame->item_count = model->item_count;
    frame->item_capacity = model->item_capacity;
    frame->total = model->total;
    frame->has_total = model->has_total;
    frame->has_more = model->has_more;
    model->stack_depth++;
    model->items = NULL;
    model->item_count = 0U;
    model->item_capacity = 0U;
  } else if (model->items) {
    zzplay_free(model->items);
    model->items = NULL;
    model->item_count = 0U;
    model->item_capacity = 0U;
  }

  model->is_search = 1;
  copy_string(model->search_query, sizeof(model->search_query), query);
  model->selection = -1;
  model->scroll = 0U;
  model->total = 0U;
  model->has_total = 0;
  model->has_more = 0;
  model->busy = 1;
  model->status_message[0] = '\0';

  if (cache_restore(model, model->services[model->current_service].scheme, 1,
                    query)) {
    model->busy = 0;
    if (out_req) {
      memset(out_req, 0, sizeof(*out_req));
      out_req->kind = ZZPLAY_BROWSE_REQ_NONE;
    }
    return 1;
  }

  if (out_req) {
    memset(out_req, 0, sizeof(*out_req));
    out_req->kind = ZZPLAY_BROWSE_REQ_SEARCH;
    out_req->generation = model->generation;
    copy_string(out_req->scheme, sizeof(out_req->scheme),
                model->services[model->current_service].scheme);
    copy_string(out_req->query, sizeof(out_req->query), query);
  }
  return 1;
}

int zzplay_browse_model_request_more(ZZPlayBrowseModel *model,
                                     ZZPlayBrowseRequest *out_req)
{
  if (!model || model->busy || !model->has_more ||
      model->current_service < 0 || model->is_search) {
    return 0;
  }
  model->generation++;
  model->busy = 1;

  if (out_req) {
    memset(out_req, 0, sizeof(*out_req));
    out_req->kind = ZZPLAY_BROWSE_REQ_LIST;
    out_req->generation = model->generation;
    copy_string(out_req->scheme, sizeof(out_req->scheme),
                model->services[model->current_service].scheme);
    copy_string(out_req->node_id, sizeof(out_req->node_id), model->current_node);
    out_req->start = model->item_count;
    out_req->count = ZZPLAY_BROWSE_PAGE_SIZE;
  }
  return 1;
}

void zzplay_browse_model_set_selection(ZZPlayBrowseModel *model, int32_t index)
{
  if (!model) {
    return;
  }
  if (index < 0 || (uint32_t)index >= model->item_count) {
    model->selection = -1;
  } else {
    model->selection = index;
  }
  cache_save_view_state(model);
}

void zzplay_browse_model_set_scroll(ZZPlayBrowseModel *model, uint32_t scroll)
{
  if (model) {
    model->scroll = scroll;
    cache_save_view_state(model);
  }
}

int zzplay_browse_model_can_play(const ZZPlayBrowseModel *model)
{
  const ZZPlayBrowseItem *item;

  if (!model || model->selection < 0 ||
      (uint32_t)model->selection >= model->item_count) {
    return 0;
  }
  item = &model->items[model->selection];
  if (item->kind == ZZPLAY_ITEM_FOLDER || item->playable == 0U) {
    return 0;
  }
  return 1;
}

int zzplay_browse_model_can_add(const ZZPlayBrowseModel *model)
{
  return zzplay_browse_model_can_play(model);
}
int zzplay_browse_model_provider_has_favourite(const ZZPlayProvider *provider)
{
  if (!provider) {
    return 0;
  }
  if (provider->size < offsetof(ZZPlayProvider, favourite) + sizeof(provider->favourite)) {
    return 0;
  }
  return provider->favourite != NULL;
}

int zzplay_browse_model_can_favourite(const ZZPlayBrowseModel *model)
{
  const ZZPlayBrowseServiceEntry *svc;
  const ZZPlayBrowseItem *item;

  if (!model || model->selection < 0 ||
      (uint32_t)model->selection >= model->item_count) {
    return 0;
  }
  svc = zzplay_browse_model_current_service(model);
  if (!svc || !zzplay_browse_model_provider_has_favourite(svc->provider)) {
    return 0;
  }
  item = &model->items[model->selection];
  if (item->kind == ZZPLAY_ITEM_FOLDER || item->playable == 0U) {
    return 0;
  }
  return 1;
}

int zzplay_browse_model_favourite(ZZPlayBrowseModel *model,
                                  ZZPlayBrowseRequest *out_req)
{
  const ZZPlayBrowseServiceEntry *svc;
  const ZZPlayBrowseItem *item;

  if (!zzplay_browse_model_can_favourite(model)) {
    return 0;
  }
  svc = zzplay_browse_model_current_service(model);
  item = &model->items[model->selection];

  model->generation++;
  model->busy = 1;

  if (out_req) {
    memset(out_req, 0, sizeof(*out_req));
    out_req->kind = ZZPLAY_BROWSE_REQ_FAVOURITE;
    out_req->generation = model->generation;
    copy_string(out_req->scheme, sizeof(out_req->scheme), svc->scheme);
    out_req->item = *item;
    out_req->fav_add = (item->favourite == 0U) ? 1 : 0;
  }
  return 1;
}


ZZPlayBrowseAction zzplay_browse_model_activate(ZZPlayBrowseModel *model,
                                                int32_t index,
                                                ZZPlayBrowseRequest *out_req)
{
  const ZZPlayBrowseItem *item;

  if (!model || index < 0 || (uint32_t)index >= model->item_count) {
    return ZZPLAY_BROWSE_ACTION_NONE;
  }
  item = &model->items[index];
  if (item->kind == ZZPLAY_ITEM_FOLDER) {
    if (zzplay_browse_model_enter_folder(model, index, out_req)) {
      return ZZPLAY_BROWSE_ACTION_ENTER_FOLDER;
    }
    return ZZPLAY_BROWSE_ACTION_NONE;
  }
  if (item->playable != 0U) {
    model->selection = index;
    return ZZPLAY_BROWSE_ACTION_PLAY;
  }
  return ZZPLAY_BROWSE_ACTION_NONE;
}

int zzplay_browse_model_format_uri(const ZZPlayBrowseModel *model,
                                   int32_t index,
                                   char *uri, size_t uri_size,
                                   char *title, size_t title_size)
{
  const ZZPlayBrowseServiceEntry *service;
  const ZZPlayBrowseItem *item;
  int written;

  if (!model || index < 0 || (uint32_t)index >= model->item_count ||
      !uri || uri_size == 0U) {
    return 0;
  }
  service = zzplay_browse_model_current_service(model);
  if (!service) {
    return 0;
  }
  item = &model->items[index];
  written = snprintf(uri, uri_size, "%s://%s", service->scheme, item->id);
  if (written < 0 || (size_t)written >= uri_size) {
    return 0;
  }
  if (title && title_size > 0U) {
    copy_string(title, title_size, item->title);
  }
  return 1;
}

int zzplay_browse_model_can_reload(const ZZPlayBrowseModel *model)
{
  const ZZPlayBrowseServiceEntry *svc;

  if (!model || model->busy || model->current_service < 0) {
    return 0;
  }
  /* Disabled while nothing is shown for the current service. */
  svc = &model->services[model->current_service];
  if (model->item_count > 0U) {
    return 1;
  }
  return cache_lookup((ZZPlayBrowseModel *)model, svc->scheme,
                      model->is_search,
                      model->is_search ? model->search_query
                                       : model->current_node,
                      0U, 0) != NULL;
}

int zzplay_browse_model_reload(ZZPlayBrowseModel *model,
                              ZZPlayBrowseRequest *out_req)
{
  const ZZPlayBrowseServiceEntry *svc;

  if (!zzplay_browse_model_can_reload(model)) {
    return 0;
  }
  svc = &model->services[model->current_service];
  model->generation++;
  model->busy = 1;
  model->status_message[0] = '\0';
  model->saved_selection = model->selection;

  if (out_req) {
    memset(out_req, 0, sizeof(*out_req));
    out_req->generation = model->generation;
    copy_string(out_req->scheme, sizeof(out_req->scheme), svc->scheme);
    if (model->is_search) {
      out_req->kind = ZZPLAY_BROWSE_REQ_SEARCH;
      copy_string(out_req->query, sizeof(out_req->query),
                  model->search_query);
    } else {
      out_req->kind = ZZPLAY_BROWSE_REQ_LIST;
      copy_string(out_req->node_id, sizeof(out_req->node_id),
                  model->current_node);
      out_req->start = 0U;
      out_req->count = ZZPLAY_BROWSE_PAGE_SIZE;
    }
  }
  return 1;
}

void zzplay_browse_model_stop(ZZPlayBrowseModel *model)
{
  if (!model) {
    return;
  }
  model->generation++;
  model->busy = 0;
  copy_string(model->status_message, sizeof(model->status_message), "Stopped");
}

/* Put a reply's rows in place. A request's rows replace whatever was shown
 * from its `start` on, once: by its first partial group, or by the final
 * reply, which carries the whole page again. Later partial groups of the
 * same request append. */
static void model_take_rows(ZZPlayBrowseModel *model,
                            const ZZPlayBrowseReply *reply)
{
  uint32_t keep;
  uint32_t new_count;
  ZZPlayBrowseItem *new_items;

  if (!reply->is_partial || !model->rows_open ||
      model->rows_generation != reply->generation) {
    keep = reply->start < model->item_count ? reply->start : model->item_count;
  } else {
    keep = model->item_count;
  }
  model->rows_open = reply->is_partial;
  model->rows_generation = reply->generation;

  new_count = keep + reply->item_count;
  new_items = NULL;
  if (new_count > 0U) {
    new_items = (ZZPlayBrowseItem *)zzplay_alloc(
        (size_t)new_count * sizeof(ZZPlayBrowseItem));
    if (new_items == NULL) {
      return;
    }
    if (keep > 0U) {
      memcpy(new_items, model->items, (size_t)keep * sizeof(ZZPlayBrowseItem));
    }
    if (reply->item_count > 0U) {
      memcpy(&new_items[keep], reply->items,
             (size_t)reply->item_count * sizeof(ZZPlayBrowseItem));
    }
  }
  if (model->items) {
    zzplay_free(model->items);
  }
  model->items = new_items;
  model->item_count = new_count;
  model->item_capacity = new_count;
}

int zzplay_browse_model_apply_reply(ZZPlayBrowseModel *model,
                                    const ZZPlayBrowseReply *reply)
{
  if (!model || !reply) {
    return 0;
  }
  /* Drop replies for abandoned generations, stale partials included */
  if (reply->generation != model->generation) {
    return 0;
  }

  /* A partial reply only adds rows; the request stays busy. */
  if (!reply->is_partial) {
    model->busy = 0;
  }
  if (!reply->success) {
    copy_string(model->status_message, sizeof(model->status_message),
                reply->error[0] ? reply->error : "Operation failed");
    return 1;
  }

  model->status_message[0] = '\0';

  if (reply->kind == ZZPLAY_BROWSE_REQ_LIST) {
    model_take_rows(model, reply);

    if (reply->has_total) {
      model->total = reply->total;
      model->has_total = 1;
      model->has_more = (model->item_count < reply->total);
    } else {
      model->has_total = 0;
      model->has_more = (reply->item_count >= ZZPLAY_BROWSE_PAGE_SIZE);
    }
    if (model->saved_selection >= 0) {
      if ((uint32_t)model->saved_selection < model->item_count) {
        model->selection = model->saved_selection;
      } else if (model->item_count > 0U) {
        model->selection = (int32_t)(model->item_count - 1U);
      } else {
        model->selection = -1;
      }
      model->saved_selection = -1;
    }
    /* Remember completed lists (a Reload replaces its copy) */
    if (!reply->is_partial && reply->start == 0U && reply->items != NULL) {
      cache_put(model, model->services[model->current_service].scheme, 0,
                model->current_node, 0U, model->items, model->item_count,
                model->selection, model->scroll);
    }
    return 1;
  }

  if (reply->kind == ZZPLAY_BROWSE_REQ_SEARCH) {
    model_take_rows(model, reply);
    if (!reply->is_partial) {
      model->has_total = reply->has_total;
      model->total = reply->total;
      model->has_more = 0;
      cache_put(model, model->services[model->current_service].scheme, 1,
                model->search_query, 0U, model->items, model->item_count,
                model->selection, model->scroll);
    }
    return 1;
  }

  return 1;
}
