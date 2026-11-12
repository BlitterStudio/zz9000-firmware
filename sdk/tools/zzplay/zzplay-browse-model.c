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
                      uint32_t count, int32_t selection, uint32_t scroll,
                      int folder_is_collection)
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
  e->folder_is_collection = folder_is_collection ? 1 : 0;
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
  model->folder_is_collection = e->folder_is_collection;
  model->total = 0U;
  model->has_total = 0;
  model->has_more = 0;
  return 1;
}

/* --- Recently played ----------------------------------------------- */

static int is_recent_row(const ZZPlayBrowseItem *item)
{
  return item->kind == ZZPLAY_ITEM_FOLDER &&
         strcmp(item->id, ZZPLAY_BROWSE_RECENT_ID) == 0;
}

/* The service's top level, where the Recently played row is shown. */
static int model_at_top(const ZZPlayBrowseModel *model)
{
  return model->current_service >= 0 && !model->is_search &&
         model->current_node[0] == '\0';
}

/* 1 when the shown rows start with the Recently played row. */
static uint32_t model_lead(const ZZPlayBrowseModel *model)
{
  return (model->item_count > 0U && is_recent_row(&model->items[0])) ? 1U
                                                                      : 0U;
}

/* Show the Recently played row first at the top level while the list has
 * entries, and nowhere else. It waits for the service's first rows (or
 * the end of its request), so a view still loading stays empty. The
 * selection stays on the same row. */
static void recent_sync(ZZPlayBrowseModel *model)
{
  int has = model_lead(model) != 0U;
  int want = model_at_top(model) && model->recent.count > 0U &&
             (model->items != NULL || !model->busy);
  ZZPlayBrowseItem *grown;

  if (has == want) {
    return;
  }
  if (!want) {
    if (model->item_count > 1U) {
      memmove(&model->items[0], &model->items[1],
              (size_t)(model->item_count - 1U) * sizeof(ZZPlayBrowseItem));
    }
    model->item_count--;
    if (model->selection == 0) {
      model->selection = -1;
    } else if (model->selection > 0) {
      model->selection--;
    }
    return;
  }
  grown = (ZZPlayBrowseItem *)zzplay_alloc(
      (size_t)(model->item_count + 1U) * sizeof(ZZPlayBrowseItem));
  if (!grown) {
    return;
  }
  memset(&grown[0], 0, sizeof(grown[0]));
  copy_string(grown[0].id, sizeof(grown[0].id), ZZPLAY_BROWSE_RECENT_ID);
  copy_string(grown[0].title, sizeof(grown[0].title),
              ZZPLAY_BROWSE_RECENT_TITLE);
  grown[0].kind = ZZPLAY_ITEM_FOLDER;
  if (model->item_count > 0U) {
    memcpy(&grown[1], model->items,
           (size_t)model->item_count * sizeof(ZZPlayBrowseItem));
  }
  if (model->items) {
    zzplay_free(model->items);
  }
  model->items = grown;
  model->item_count++;
  model->item_capacity = model->item_count;
  if (model->selection >= 0) {
    model->selection++;
  }
}

/* Fill the view with a copy of the Recently played list. */
static void recent_show(ZZPlayBrowseModel *model)
{
  ZZPlayBrowseItem *copy = NULL;
  uint32_t count = model->recent.count;

  if (count > 0U) {
    copy = (ZZPlayBrowseItem *)zzplay_alloc(
        (size_t)count * sizeof(ZZPlayBrowseItem));
    if (!copy) {
      count = 0U;
    } else {
      memcpy(copy, model->recent.items,
             (size_t)count * sizeof(ZZPlayBrowseItem));
    }
  }
  if (model->items) {
    zzplay_free(model->items);
  }
  model->items = copy;
  model->item_count = count;
  model->item_capacity = count;
  if (model->selection >= 0 && (uint32_t)model->selection >= count) {
    model->selection = count > 0U ? (int32_t)(count - 1U) : -1;
  }
  model->total = 0U;
  model->has_total = 0;
  model->has_more = 0;
  model->busy = 0;
}

static const char *const recent_kind_words[] = {"folder", "track",
                                                "station"};

/* No byte below a space: a field of the saved form holds no tab or line
 * end. */
static int recent_field_clean(const char *s, size_t len)
{
  size_t i;

  for (i = 0U; i < len; i++) {
    if ((unsigned char)s[i] < 0x20U) {
      return 0;
    }
  }
  return 1;
}

static void recent_clean_text(char *s)
{
  for (; *s != '\0'; s++) {
    if ((unsigned char)*s < 0x20U) {
      *s = ' ';
    }
  }
}

static uint32_t recent_find(const ZZPlayBrowseRecent *recent, uint32_t kind,
                            const char *id)
{
  uint32_t i;

  for (i = 0U; i < recent->count; i++) {
    if (recent->items[i].kind == kind &&
        strcmp(recent->items[i].id, id) == 0) {
      return i;
    }
  }
  return recent->count;
}

void zzplay_browse_recent_clear(ZZPlayBrowseRecent *recent)
{
  if (recent) {
    memset(recent, 0, sizeof(*recent));
  }
}

int zzplay_browse_recent_add(ZZPlayBrowseRecent *recent,
                             const ZZPlayBrowseItem *item)
{
  ZZPlayBrowseItem copy;
  uint32_t at;

  if (!recent || !item || item->id[0] == '\0' ||
      item->kind > ZZPLAY_ITEM_STATION || is_recent_row(item) ||
      memchr(item->id, '\0', sizeof(item->id)) == NULL ||
      !recent_field_clean(item->id, strlen(item->id))) {
    return 0;
  }
  memset(&copy, 0, sizeof(copy));
  copy_string(copy.id, sizeof(copy.id), item->id);
  copy_string(copy.title, sizeof(copy.title), item->title);
  copy_string(copy.subtitle, sizeof(copy.subtitle), item->subtitle);
  recent_clean_text(copy.title);
  recent_clean_text(copy.subtitle);
  copy.kind = item->kind;
  copy.playable = item->playable != 0U ? 1U : 0U;

  at = recent_find(recent, copy.kind, copy.id);
  if (at == recent->count) {
    /* New: the oldest drops off a full list. */
    if (recent->count < ZZPLAY_BROWSE_RECENT_MAX) {
      recent->count++;
    }
    at = recent->count - 1U;
  }
  if (at > 0U) {
    memmove(&recent->items[1], &recent->items[0],
            (size_t)at * sizeof(ZZPlayBrowseItem));
  }
  recent->items[0] = copy;
  return 1;
}

uint32_t zzplay_browse_recent_parse(ZZPlayBrowseRecent *recent,
                                    const char *text, size_t length)
{
  size_t pos = 0U;

  if (!recent) {
    return 0U;
  }
  zzplay_browse_recent_clear(recent);
  if (!text) {
    return 0U;
  }
  while (pos < length && recent->count < ZZPLAY_BROWSE_RECENT_MAX) {
    const char *line = text + pos;
    size_t len = 0U;
    const char *field[5];
    size_t field_len[5];
    uint32_t nfields = 0U;
    uint32_t kind;
    size_t i;
    size_t start;
    ZZPlayBrowseItem *it;

    while (pos + len < length && line[len] != '\n') {
      len++;
    }
    pos += len;
    if (pos < length) {
      pos++; /* the line end */
    }
    if (len > 0U && line[len - 1U] == '\r') {
      len--;
    }
    if (len == 0U || line[0] == '#') {
      continue;
    }

    /* Exactly five tab-separated fields. */
    start = 0U;
    for (i = 0U; i <= len; i++) {
      if (i == len || line[i] == '\t') {
        if (nfields == 5U) {
          nfields = 6U; /* one too many */
          break;
        }
        field[nfields] = line + start;
        field_len[nfields] = i - start;
        nfields++;
        start = i + 1U;
      }
    }
    if (nfields != 5U) {
      continue;
    }
    for (kind = 0U; kind <= ZZPLAY_ITEM_STATION; kind++) {
      if (field_len[0] == strlen(recent_kind_words[kind]) &&
          memcmp(field[0], recent_kind_words[kind], field_len[0]) == 0) {
        break;
      }
    }
    if (kind > ZZPLAY_ITEM_STATION || field_len[1] == 0U ||
        field_len[1] >= ZZPLAY_PROVIDER_TEXT_MAX ||
        field_len[4] != 1U || (field[4][0] != '0' && field[4][0] != '1') ||
        !recent_field_clean(field[1], field_len[1]) ||
        !recent_field_clean(field[2], field_len[2]) ||
        !recent_field_clean(field[3], field_len[3])) {
      continue;
    }

    it = &recent->items[recent->count];
    memset(it, 0, sizeof(*it));
    memcpy(it->id, field[1], field_len[1]);
    /* Over-long titles are cut. */
    memcpy(it->title, field[2],
           field_len[2] < sizeof(it->title) ? field_len[2]
                                            : sizeof(it->title) - 1U);
    memcpy(it->subtitle, field[3],
           field_len[3] < sizeof(it->subtitle) ? field_len[3]
                                               : sizeof(it->subtitle) - 1U);
    it->kind = kind;
    it->playable = field[4][0] == '1' ? 1U : 0U;
    if (strcmp(it->id, ZZPLAY_BROWSE_RECENT_ID) == 0 ||
        recent_find(recent, kind, it->id) < recent->count) {
      continue; /* a repeat: the newer line already counts */
    }
    recent->count++;
  }
  return recent->count;
}

/* Append `len` bytes at `*used`, keeping what fits below `capacity`. */
static void recent_put(char *out, size_t capacity, size_t *used,
                       const char *s, size_t len)
{
  if (out && capacity > 0U && *used < capacity - 1U) {
    size_t room = capacity - 1U - *used;
    memcpy(out + *used, s, len < room ? len : room);
  }
  *used += len;
}

size_t zzplay_browse_recent_format(const ZZPlayBrowseRecent *recent,
                                   char *out, size_t capacity)
{
  size_t used = 0U;
  uint32_t i;

  if (out && capacity > 0U) {
    out[0] = '\0';
  }
  if (!recent) {
    return 0U;
  }
  for (i = 0U; i < recent->count && i < ZZPLAY_BROWSE_RECENT_MAX; i++) {
    const ZZPlayBrowseItem *it = &recent->items[i];
    const char *word = recent_kind_words[it->kind <= ZZPLAY_ITEM_STATION
                                             ? it->kind
                                             : ZZPLAY_ITEM_TRACK];

    recent_put(out, capacity, &used, word, strlen(word));
    recent_put(out, capacity, &used, "\t", 1U);
    recent_put(out, capacity, &used, it->id, strlen(it->id));
    recent_put(out, capacity, &used, "\t", 1U);
    recent_put(out, capacity, &used, it->title, strlen(it->title));
    recent_put(out, capacity, &used, "\t", 1U);
    recent_put(out, capacity, &used, it->subtitle, strlen(it->subtitle));
    recent_put(out, capacity, &used, it->playable ? "\t1\n" : "\t0\n", 3U);
  }
  if (out && capacity > 0U) {
    out[used < capacity ? used : capacity - 1U] = '\0';
  }
  return used;
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
  model->current_subtitle[0] = '\0';
  model->is_search = 0;
  model->search_query[0] = '\0';
  model->folder_is_collection = 0;
  model->selection = -1;
  model->scroll = 0U;
  model->total = 0U;
  model->has_total = 0;
  model->has_more = 0;
  model->busy = 1;
  model->status_message[0] = '\0';
  /* The caller loads the new service's list (load_recent). */
  zzplay_browse_recent_clear(&model->recent);

  if (cache_restore(model, model->services[index].scheme, 0, "")) {
    model->busy = 0;
    recent_sync(model);
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

int zzplay_browse_model_reopen_service(ZZPlayBrowseModel *model,
                                       ZZPlayBrowseRequest *out_req)
{
  const ZZPlayBrowseServiceEntry *svc =
      zzplay_browse_model_current_service(model);
  uint32_t i;

  if (!svc) {
    return 0;
  }
  /* What was listed under the old settings may be wrong now. */
  for (i = 0U; i < ZZPLAY_BROWSE_CACHE_MAX; i++) {
    ZZPlayBrowseCacheEntry *e = &model->cache[i];

    if (e->items != NULL && strcmp(e->scheme, svc->scheme) == 0) {
      zzplay_free(e->items);
      memset(e, 0, sizeof(*e));
    }
  }
  if (!zzplay_browse_model_select_service(
          model, (uint32_t)model->current_service, out_req)) {
    return 0;
  }
  if (out_req) {
    out_req->reopen = 1;
  }
  return 1;
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
  copy_string(frame->subtitle, sizeof(frame->subtitle),
              model->current_subtitle);
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
  frame->folder_is_collection = model->folder_is_collection;
  model->stack_depth++;

  /* Adopt new folder */
  copy_string(model->current_node, sizeof(model->current_node), item->id);
  copy_string(model->current_title, sizeof(model->current_title), item->title);
  copy_string(model->current_subtitle, sizeof(model->current_subtitle),
              item->subtitle);
  model->is_search = 0;
  model->search_query[0] = '\0';
  /* Play all / Add all work on this folder when the module marks it
   * playable. */
  model->folder_is_collection = item->playable != 0U ? 1 : 0;
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

  if (is_recent_row(item)) {
    /* ZZPlay's own list: shown at once, nothing to ask the service. */
    recent_show(model);
    if (out_req) {
      memset(out_req, 0, sizeof(*out_req));
      out_req->kind = ZZPLAY_BROWSE_REQ_NONE;
    }
    return 1;
  }

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

  /* Step past frames whose view never got rows: their list was
   * superseded before anything arrived, so restoring one would show an
   * empty list (and, for a collection, enable Play all on nothing). */
  while (model->stack_depth > 0U) {
    frame = &model->stack[model->stack_depth - 1U];
    if (frame->items != NULL || frame->item_count != 0U) {
      break;
    }
    memset(frame, 0, sizeof(*frame));
    model->stack_depth--;
  }
  if (model->stack_depth == 0U) {
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
  copy_string(model->current_subtitle, sizeof(model->current_subtitle),
              frame->subtitle);
  model->is_search = frame->is_search;
  copy_string(model->search_query, sizeof(model->search_query),
              frame->search_query);
  model->items = frame->items;
  model->item_count = frame->item_count;
  model->item_capacity = frame->item_capacity;
  model->total = frame->total;
  model->has_total = frame->has_total;
  model->has_more = frame->has_more;
  model->folder_is_collection = frame->folder_is_collection;
  model->selection = frame->selection;
  model->scroll = frame->scroll;
  model->busy = 0;
  model->status_message[0] = '\0';

  memset(frame, 0, sizeof(*frame));
  /* The list may have changed while a folder was open. */
  recent_sync(model);

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

  /* If starting a search from a browse view, save that view on the
   * stack. A view whose rows have not arrived is not saved: the search
   * supersedes its request, so it could never be shown again and Back
   * would land on an empty list. */
  if (!model->is_search && model->items != NULL &&
      model->stack_depth < ZZPLAY_BROWSE_STACK_MAX) {
    frame = &model->stack[model->stack_depth];
    memset(frame, 0, sizeof(*frame));
    copy_string(frame->node_id, sizeof(frame->node_id), model->current_node);
    copy_string(frame->title, sizeof(frame->title), model->current_title);
    copy_string(frame->subtitle, sizeof(frame->subtitle),
                model->current_subtitle);
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
    frame->folder_is_collection = model->folder_is_collection;
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
  /* Search results are not a collection. */
  model->folder_is_collection = 0;
  model->current_subtitle[0] = '\0';
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
    out_req->start = model->item_count - model_lead(model);
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

int32_t zzplay_browse_model_move_selection(ZZPlayBrowseModel *model,
                                           int32_t delta)
{
  int32_t sel;
  int32_t last;

  if (!model || model->item_count == 0U) {
    return -1;
  }
  last = (int32_t)(model->item_count - 1U);
  sel = model->selection;
  if (sel < 0 || sel > last) {
    sel = delta < 0 ? last : 0;
  } else if (delta < 0) {
    sel = delta < -sel ? 0 : sel + delta;
  } else {
    sel = delta > last - sel ? last : sel + delta;
  }
  zzplay_browse_model_set_selection(model, sel);
  return sel;
}

/* Case-folded byte: ASCII and Latin-1 capitals map to small letters. */
static int fold_letter(int c)
{
  c &= 0xFF;
  if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) {
    return c + 0x20;
  }
  return c;
}

int32_t zzplay_browse_model_jump_to_letter(ZZPlayBrowseModel *model,
                                           int letter)
{
  uint32_t start;
  uint32_t n;
  int want;

  if (!model || model->item_count == 0U) {
    return -1;
  }
  want = fold_letter(letter);
  if (want <= ' ') {
    return -1;
  }
  start = (model->selection >= 0 &&
           (uint32_t)model->selection < model->item_count)
              ? (uint32_t)model->selection + 1U
              : 0U;
  for (n = 0U; n < model->item_count; n++) {
    uint32_t i = (start + n) % model->item_count;

    if (fold_letter((unsigned char)model->items[i].title[0]) == want) {
      zzplay_browse_model_set_selection(model, (int32_t)i);
      return (int32_t)i;
    }
  }
  return -1;
}

ZZPlayBrowseTarget zzplay_browse_model_play_target(const ZZPlayBrowseModel *model)
{
  const ZZPlayBrowseItem *item;

  if (!model || model->selection < 0 ||
      (uint32_t)model->selection >= model->item_count) {
    return ZZPLAY_BROWSE_TARGET_NONE;
  }
  item = &model->items[model->selection];
  if (item->playable == 0U) {
    return ZZPLAY_BROWSE_TARGET_NONE;
  }
  if (item->kind == ZZPLAY_ITEM_FOLDER) {
    return ZZPLAY_BROWSE_TARGET_COLLECTION;
  }
  return ZZPLAY_BROWSE_TARGET_ITEM;
}

int zzplay_browse_model_can_play(const ZZPlayBrowseModel *model)
{
  return zzplay_browse_model_play_target(model) != ZZPLAY_BROWSE_TARGET_NONE;
}

int zzplay_browse_model_can_add(const ZZPlayBrowseModel *model)
{
  return zzplay_browse_model_can_play(model);
}

int zzplay_browse_model_can_play_all(const ZZPlayBrowseModel *model)
{
  return (model && model->folder_is_collection && !model->busy &&
          model->current_service >= 0 && model->item_count > 0U)
             ? 1
             : 0;
}

int zzplay_browse_model_can_add_all(const ZZPlayBrowseModel *model)
{
  return zzplay_browse_model_can_play_all(model);
}

int zzplay_browse_model_gather_shown(ZZPlayBrowseModel *model, uint32_t limit,
                                     ZZPlayBrowseRequest *out_req)
{
  const ZZPlayBrowseServiceEntry *svc;

  if (!model || !model->folder_is_collection || model->busy ||
      model->current_service < 0 || model->item_count == 0U) {
    return 0;
  }
  svc = zzplay_browse_model_current_service(model);
  if (!svc) {
    return 0;
  }
  model->generation++;
  model->busy = 1;
  model->status_message[0] = '\0';
  model->gathering = 1;
  model->gather_generation = model->generation;

  if (out_req) {
    memset(out_req, 0, sizeof(*out_req));
    out_req->kind = ZZPLAY_BROWSE_REQ_GATHER;
    out_req->generation = model->generation;
    copy_string(out_req->scheme, sizeof(out_req->scheme), svc->scheme);
    copy_string(out_req->node_id, sizeof(out_req->node_id),
                model->current_node);
    out_req->count = limit;
  }
  return 1;
}

int zzplay_browse_model_gather(ZZPlayBrowseModel *model, uint32_t limit,
                               ZZPlayBrowseRequest *out_req)
{
  const ZZPlayBrowseServiceEntry *svc;

  if (zzplay_browse_model_play_target(model) !=
      ZZPLAY_BROWSE_TARGET_COLLECTION) {
    return 0;
  }
  svc = zzplay_browse_model_current_service(model);
  if (!svc) {
    return 0;
  }
  model->generation++;
  model->busy = 1;
  model->status_message[0] = '\0';
  model->gathering = 1;
  model->gather_generation = model->generation;

  if (out_req) {
    memset(out_req, 0, sizeof(*out_req));
    out_req->kind = ZZPLAY_BROWSE_REQ_GATHER;
    out_req->generation = model->generation;
    copy_string(out_req->scheme, sizeof(out_req->scheme), svc->scheme);
    copy_string(out_req->node_id, sizeof(out_req->node_id),
                model->items[model->selection].id);
    out_req->count = limit;
  }
  return 1;
}

int zzplay_browse_model_gather_live(const ZZPlayBrowseModel *model,
                                    uint32_t generation)
{
  return (model && model->gathering &&
          generation == model->gather_generation &&
          generation == model->generation)
             ? 1
             : 0;
}

void zzplay_browse_model_gather_done(ZZPlayBrowseModel *model)
{
  if (!model) {
    return;
  }
  if (zzplay_browse_model_gather_live(model, model->gather_generation)) {
    model->busy = 0;
  }
  model->gathering = 0;
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

int zzplay_browse_model_provider_has_link(const ZZPlayProvider *provider)
{
  if (!provider ||
      provider->size < offsetof(ZZPlayProvider, link_choose) +
                           sizeof(provider->link_choose)) {
    return 0;
  }
  return provider->link_start != NULL && provider->link_poll != NULL &&
         provider->link_choose != NULL;
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
  if (item->playable == 0U &&
      strcmp(item->id, ZZPLAY_PROVIDER_SETTINGS_ID) == 0) {
    model->selection = index;
    return ZZPLAY_BROWSE_ACTION_OPEN_SETTINGS;
  }
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
  if (zzplay_browse_model_in_recent(model)) {
    return 1;
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
  if (zzplay_browse_model_in_recent(model)) {
    /* Show the list as it is now; nothing to ask the service. */
    recent_show(model);
    if (out_req) {
      memset(out_req, 0, sizeof(*out_req));
      out_req->kind = ZZPLAY_BROWSE_REQ_NONE;
    }
    return 1;
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
  model->gathering = 0;
  /* A top level stopped before its rows came still offers Recently
   * played. */
  recent_sync(model);
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
  /* Service rows start after the Recently played row, if shown. */
  uint32_t lead = model_lead(model);
  uint32_t new_count;
  ZZPlayBrowseItem *new_items;

  if (!reply->is_partial || !model->rows_open ||
      model->rows_generation != reply->generation) {
    keep = reply->start < model->item_count - lead ? reply->start + lead
                                                   : model->item_count;
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
    recent_sync(model);
    return 1;
  }

  model->status_message[0] = '\0';

  if (reply->kind == ZZPLAY_BROWSE_REQ_LIST) {
    model_take_rows(model, reply);
    recent_sync(model);

    if (reply->has_total) {
      model->total = reply->total;
      model->has_total = 1;
      model->has_more =
          (zzplay_browse_model_service_rows(model) < reply->total);
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
                model->selection, model->scroll,
                model->folder_is_collection);
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
                model->selection, model->scroll, 0);
    }
    return 1;
  }

  return 1;
}

uint32_t zzplay_browse_model_service_rows(const ZZPlayBrowseModel *model)
{
  return model ? model->item_count - model_lead(model) : 0U;
}

size_t zzplay_browse_model_format_count(const ZZPlayBrowseModel *model,
                                        char *buf, size_t size)
{
  uint32_t count;
  int n;

  if (buf && size > 0U) {
    buf[0] = '\0';
  }
  if (!model || !buf || size == 0U) {
    return 0U;
  }
  count = model->item_count;
  if (count == 0U) {
    return 0U;
  }
  /* Count what is shown. When total is known and larger than the loaded
   * rows, show "X of Y items" (counting any folder row shown at the top). */
  if (model->has_total) {
    uint32_t lead = model_lead(model);
    uint32_t srows = count >= lead ? count - lead : 0U;

    if (model->total > srows) {
      uint32_t total = model->total + lead;
      n = snprintf(buf, size, "%lu of %lu items", (unsigned long)count,
                   (unsigned long)total);
      if (n < 0 || (size_t)n >= size) {
        buf[size - 1U] = '\0';
        return size - 1U;
      }
      return (size_t)n;
    }
  }
  n = snprintf(buf, size, "%lu %s", (unsigned long)count,
               count == 1U ? "item" : "items");
  if (n < 0 || (size_t)n >= size) {
    buf[size - 1U] = '\0';
    return size - 1U;
  }
  return (size_t)n;
}

int zzplay_browse_model_in_recent(const ZZPlayBrowseModel *model)
{
  return (model && model->current_service >= 0 && !model->is_search &&
          strcmp(model->current_node, ZZPLAY_BROWSE_RECENT_ID) == 0)
             ? 1
             : 0;
}

int zzplay_browse_model_shown_collection(const ZZPlayBrowseModel *model,
                                         ZZPlayBrowseItem *out)
{
  if (!model || !out || !model->folder_is_collection || model->is_search ||
      model->current_service < 0 || model->current_node[0] == '\0') {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  copy_string(out->id, sizeof(out->id), model->current_node);
  copy_string(out->title, sizeof(out->title), model->current_title);
  copy_string(out->subtitle, sizeof(out->subtitle), model->current_subtitle);
  out->kind = ZZPLAY_ITEM_FOLDER;
  out->playable = 1U;
  return 1;
}

void zzplay_browse_model_load_recent(ZZPlayBrowseModel *model,
                                     const char *text, size_t length)
{
  if (!model) {
    return;
  }
  (void)zzplay_browse_recent_parse(&model->recent, text, length);
  recent_sync(model);
}

int zzplay_browse_model_note_started(ZZPlayBrowseModel *model,
                                     const ZZPlayBrowseItem *item)
{
  /* `item` may be one of the shown rows, which recent_sync can move: the
   * list takes its copy first. */
  if (!model || !zzplay_browse_recent_add(&model->recent, item)) {
    return 0;
  }
  recent_sync(model);
  return 1;
}

int zzplay_browse_model_note_recent_save(ZZPlayBrowseModel *model,
                                         int saved)
{
  int report;

  if (!model) {
    return 0;
  }
  report = !saved && !model->recent_save_failed;
  model->recent_save_failed = saved ? 0 : 1;
  return report;
}

int zzplay_browse_recent_save(const ZZPlayBrowseRecent *recent,
                              ZZPlayBrowseRecentWriter writer, void *user)
{
  size_t need;
  char *text;
  int ok;

  if (!recent || !writer) {
    return 0;
  }
  need = zzplay_browse_recent_format(recent, NULL, 0U);
  text = (char *)zzplay_alloc(need + 1U);
  if (!text) {
    return 0;
  }
  (void)zzplay_browse_recent_format(recent, text, need + 1U);
  ok = writer(user, text, (uint32_t)need) ? 1 : 0;
  zzplay_free(text);
  return ok;
}
