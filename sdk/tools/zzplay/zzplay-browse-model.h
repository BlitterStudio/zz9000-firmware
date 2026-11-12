/* Host-testable state for ZZPlay's service browser.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_BROWSE_MODEL_H
#define ZZPLAY_BROWSE_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "zzplay-provider.h"
#include "zzplay-providers.h"

#define ZZPLAY_BROWSE_PAGE_SIZE 100U
#define ZZPLAY_BROWSE_STACK_MAX 16U
#define ZZPLAY_BROWSE_SERVICES_MAX ZZPLAY_PROVIDERS_MAX
/* How many completed lists and searches the session remembers. The least
 * recently used one is dropped when the cache is full. */
#define ZZPLAY_BROWSE_CACHE_MAX 16U

typedef struct ZZPlayBrowseItem {
  char id[ZZPLAY_PROVIDER_TEXT_MAX];
  char title[ZZPLAY_PROVIDER_TEXT_MAX];
  char subtitle[ZZPLAY_PROVIDER_TEXT_MAX];
  uint32_t duration_ms;
  uint32_t kind;             /* ZZPLAY_ITEM_* */
  uint32_t playable;
  uint32_t favourite;        /* 1 when in favourites */
} ZZPlayBrowseItem;

typedef enum ZZPlayBrowseAction {
  ZZPLAY_BROWSE_ACTION_NONE = 0,
  ZZPLAY_BROWSE_ACTION_ENTER_FOLDER,
  ZZPLAY_BROWSE_ACTION_PLAY,
  ZZPLAY_BROWSE_ACTION_PAGE_MORE
} ZZPlayBrowseAction;

/* What Play or Add does with the current selection. */
typedef enum ZZPlayBrowseTarget {
  ZZPLAY_BROWSE_TARGET_NONE = 0,
  ZZPLAY_BROWSE_TARGET_ITEM,       /* a playable track or station */
  ZZPLAY_BROWSE_TARGET_COLLECTION  /* a folder the module marks playable */
} ZZPlayBrowseTarget;

typedef enum ZZPlayBrowseReqKind {
  ZZPLAY_BROWSE_REQ_NONE = 0,
  ZZPLAY_BROWSE_REQ_OPEN,
  ZZPLAY_BROWSE_REQ_LIST,
  ZZPLAY_BROWSE_REQ_SEARCH,
  ZZPLAY_BROWSE_REQ_RESOLVE,
  ZZPLAY_BROWSE_REQ_FAVOURITE,
  ZZPLAY_BROWSE_REQ_GATHER,
  ZZPLAY_BROWSE_REQ_CLOSE
} ZZPlayBrowseReqKind;

typedef struct ZZPlayBrowseRequest {
  ZZPlayBrowseReqKind kind;
  uint32_t generation;
  char scheme[ZZPLAY_PROVIDER_SCHEME_MAX];
  char node_id[ZZPLAY_PROVIDER_TEXT_MAX];
  char query[ZZPLAY_PROVIDER_TEXT_MAX];
  char item_id[ZZPLAY_PROVIDER_TEXT_MAX];
  uint32_t start;
  uint32_t count;            /* page size, or the gather item limit */
  int fav_add;               /* 1 to add, 0 to remove */
  ZZPlayBrowseItem item;
} ZZPlayBrowseRequest;

typedef struct ZZPlayBrowseReply {
  ZZPlayBrowseReqKind kind;
  uint32_t generation;
  int success;
  char error[ZZPLAY_PROVIDER_ERROR_MAX];
  const ZZPlayBrowseItem *items;
  uint32_t item_count;
  uint32_t start;
  uint32_t total;
  int has_total;
  int fav_add;
  /* 1 when more replies for the same request follow: the worker sends
   * rows in groups while a list or search loads. */
  int is_partial;
  /* GATHER: the limit stopped the walk before the collection was
   * exhausted. */
  int truncated;
  ZZPlayResolved resolved;
} ZZPlayBrowseReply;

typedef struct ZZPlayBrowseFrame {
  char node_id[ZZPLAY_PROVIDER_TEXT_MAX];
  char title[ZZPLAY_PROVIDER_TEXT_MAX];
  int is_search;
  char search_query[ZZPLAY_PROVIDER_TEXT_MAX];
  int32_t selection;
  uint32_t scroll;
  ZZPlayBrowseItem *items;
  uint32_t item_count;
  uint32_t item_capacity;
  uint32_t total;
  int has_total;
  int has_more;
} ZZPlayBrowseFrame;

typedef struct ZZPlayBrowseServiceEntry {
  char scheme[ZZPLAY_PROVIDER_SCHEME_MAX];
  char name[ZZPLAY_PROVIDER_TEXT_MAX];
  const ZZPlayProvider *provider;
} ZZPlayBrowseServiceEntry;
/* One remembered completed list or search page. */
typedef struct ZZPlayBrowseCacheEntry {
  char scheme[ZZPLAY_PROVIDER_SCHEME_MAX];
  char node_id[ZZPLAY_PROVIDER_TEXT_MAX];  /* folder when not a search */
  char query[ZZPLAY_PROVIDER_TEXT_MAX];    /* used when is_search */
  int is_search;
  uint32_t start;
  ZZPlayBrowseItem *items;
  uint32_t item_count;
  int32_t selection;
  uint32_t scroll;
  uint32_t last_used;
} ZZPlayBrowseCacheEntry;

typedef struct ZZPlayBrowseModel {
  /* Installed services */
  ZZPlayBrowseServiceEntry services[ZZPLAY_BROWSE_SERVICES_MAX];
  uint32_t service_count;
  int32_t current_service;

  /* Current view */
  char current_node[ZZPLAY_PROVIDER_TEXT_MAX];
  char current_title[ZZPLAY_PROVIDER_TEXT_MAX];
  int is_search;
  char search_query[ZZPLAY_PROVIDER_TEXT_MAX];

  /* Items */
  ZZPlayBrowseItem *items;
  uint32_t item_count;
  uint32_t item_capacity;
  uint32_t total;
  int has_total;
  int has_more;

  /* Selection and scroll */
  int32_t selection;
  uint32_t scroll;

  int32_t saved_selection;   /* for restoring selection across reload */
  /* The request whose partial rows are being shown (see apply_reply). */
  uint32_t rows_generation;
  int rows_open;
  /* Navigation stack */
  ZZPlayBrowseFrame stack[ZZPLAY_BROWSE_STACK_MAX];
  uint32_t stack_depth;
  /* Generation and pending request */
  uint32_t generation;
  int busy;
  ZZPlayBrowseRequest pending_request;
  char status_message[ZZPLAY_PROVIDER_ERROR_MAX];

  /* Session list cache: remembered lists and searches. */
  ZZPlayBrowseCacheEntry cache[ZZPLAY_BROWSE_CACHE_MAX];
  uint32_t cache_clock;
} ZZPlayBrowseModel;

void zzplay_browse_model_init(ZZPlayBrowseModel *model);
void zzplay_browse_model_free(ZZPlayBrowseModel *model);

/* Service management */
int zzplay_browse_model_add_service(ZZPlayBrowseModel *model,
                                    const char *scheme,
                                    const char *name,
                                    const ZZPlayProvider *provider);
int zzplay_browse_model_select_service(ZZPlayBrowseModel *model, uint32_t index,
                                       ZZPlayBrowseRequest *out_req);
const ZZPlayBrowseServiceEntry *zzplay_browse_model_current_service(
    const ZZPlayBrowseModel *model);

/* Navigation and paging */
int zzplay_browse_model_enter_folder(ZZPlayBrowseModel *model, int32_t index,
                                     ZZPlayBrowseRequest *out_req);
int zzplay_browse_model_back(ZZPlayBrowseModel *model,
                             ZZPlayBrowseRequest *out_req);
int zzplay_browse_model_can_back(const ZZPlayBrowseModel *model);

int zzplay_browse_model_search(ZZPlayBrowseModel *model, const char *query,
                               ZZPlayBrowseRequest *out_req);
int zzplay_browse_model_request_more(ZZPlayBrowseModel *model,
                                     ZZPlayBrowseRequest *out_req);

/* Actions and selection */
void zzplay_browse_model_set_selection(ZZPlayBrowseModel *model, int32_t index);
void zzplay_browse_model_set_scroll(ZZPlayBrowseModel *model, uint32_t scroll);
int zzplay_browse_model_can_play(const ZZPlayBrowseModel *model);
int zzplay_browse_model_can_add(const ZZPlayBrowseModel *model);
/* What Play or Add does with the current selection: a playable track or
 * station plays as one entry, a folder the module marks playable is a
 * collection gathered whole, anything else does nothing. */
ZZPlayBrowseTarget zzplay_browse_model_play_target(const ZZPlayBrowseModel *model);

/* Start gathering the selected collection into a GATHER request that asks
 * for at most `limit` items (the playlist's free space for Add, its
 * capacity for Play). Returns 0 when the selection is no collection. */
int zzplay_browse_model_gather(ZZPlayBrowseModel *model, uint32_t limit,
                               ZZPlayBrowseRequest *out_req);
int zzplay_browse_model_provider_has_favourite(const ZZPlayProvider *provider);
int zzplay_browse_model_can_favourite(const ZZPlayBrowseModel *model);
int zzplay_browse_model_favourite(ZZPlayBrowseModel *model,
                                  ZZPlayBrowseRequest *out_req);
ZZPlayBrowseAction zzplay_browse_model_activate(ZZPlayBrowseModel *model,
                                                int32_t index,
                                                ZZPlayBrowseRequest *out_req);
int zzplay_browse_model_format_uri(const ZZPlayBrowseModel *model,
                                   int32_t index,
                                   char *uri, size_t uri_size,
                                   char *title, size_t title_size);

/* Reload: fetch the current list or search again from start 0. */
void zzplay_browse_model_stop(ZZPlayBrowseModel *model);
int zzplay_browse_model_can_reload(const ZZPlayBrowseModel *model);
int zzplay_browse_model_reload(ZZPlayBrowseModel *model,
                              ZZPlayBrowseRequest *out_req);

/* Applying worker replies. Returns 1 if accepted, 0 if dropped. */
int zzplay_browse_model_apply_reply(ZZPlayBrowseModel *model,
                                    const ZZPlayBrowseReply *reply);

#endif /* ZZPLAY_BROWSE_MODEL_H */
