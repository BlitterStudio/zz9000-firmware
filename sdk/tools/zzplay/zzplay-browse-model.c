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
}

void zzplay_browse_model_set_scroll(ZZPlayBrowseModel *model, uint32_t scroll)
{
  if (model) {
    model->scroll = scroll;
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

void zzplay_browse_model_stop(ZZPlayBrowseModel *model)
{
  if (!model) {
    return;
  }
  model->generation++;
  model->busy = 0;
  copy_string(model->status_message, sizeof(model->status_message), "Stopped");
}

int zzplay_browse_model_apply_reply(ZZPlayBrowseModel *model,
                                    const ZZPlayBrowseReply *reply)
{
  if (!model || !reply) {
    return 0;
  }
  /* Drop replies for abandoned generations */
  if (reply->generation != model->generation) {
    return 0;
  }

  model->busy = 0;
  if (!reply->success) {
    copy_string(model->status_message, sizeof(model->status_message),
                reply->error[0] ? reply->error : "Operation failed");
    return 1;
  }

  model->status_message[0] = '\0';

  if (reply->kind == ZZPLAY_BROWSE_REQ_LIST) {
    if (reply->start == 0U) {
      /* Replace whole list */
      if (model->items) {
        zzplay_free(model->items);
        model->items = NULL;
      }
      model->item_count = 0U;
      model->item_capacity = 0U;

      if (reply->item_count > 0U) {
        size_t bytes = (size_t)reply->item_count * sizeof(ZZPlayBrowseItem);
        model->items = (ZZPlayBrowseItem *)zzplay_alloc(bytes);
        if (model->items) {
          memcpy(model->items, reply->items, bytes);
          model->item_count = reply->item_count;
          model->item_capacity = reply->item_count;
        }
      }
    } else {
      /* Append page */
      if (reply->item_count > 0U) {
        uint32_t new_count = model->item_count + reply->item_count;
        size_t new_bytes = (size_t)new_count * sizeof(ZZPlayBrowseItem);
        ZZPlayBrowseItem *new_items = (ZZPlayBrowseItem *)zzplay_alloc(new_bytes);

        if (new_items) {
          if (model->items && model->item_count > 0U) {
            memcpy(new_items, model->items,
                   (size_t)model->item_count * sizeof(ZZPlayBrowseItem));
            zzplay_free(model->items);
          }
          memcpy(&new_items[model->item_count], reply->items,
                 (size_t)reply->item_count * sizeof(ZZPlayBrowseItem));
          model->items = new_items;
          model->item_count = new_count;
          model->item_capacity = new_count;
        }
      }
    }

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
    return 1;
  }

  if (reply->kind == ZZPLAY_BROWSE_REQ_SEARCH) {
    if (model->items) {
      zzplay_free(model->items);
      model->items = NULL;
    }
    model->item_count = 0U;
    model->item_capacity = 0U;

    if (reply->item_count > 0U) {
      size_t bytes = (size_t)reply->item_count * sizeof(ZZPlayBrowseItem);
      model->items = (ZZPlayBrowseItem *)zzplay_alloc(bytes);
      if (model->items) {
        memcpy(model->items, reply->items, bytes);
        model->item_count = reply->item_count;
        model->item_capacity = reply->item_count;
      }
    }
    model->has_total = reply->has_total;
    model->total = reply->total;
    model->has_more = 0;
    return 1;
  }

  return 1;
}
