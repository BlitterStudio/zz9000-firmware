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
/* Recently played: what was last started from Browse, per service. */
#define ZZPLAY_BROWSE_RECENT_MAX 25U
/* The id of the "Recently played" folder row at a service's top level.
 * The control character keeps it apart from every module id. */
#define ZZPLAY_BROWSE_RECENT_ID "\037recent"
#define ZZPLAY_BROWSE_RECENT_TITLE "Recently played"

typedef struct ZZPlayBrowseItem {
  char id[ZZPLAY_PROVIDER_TEXT_MAX];
  char title[ZZPLAY_PROVIDER_TEXT_MAX];
  char subtitle[ZZPLAY_PROVIDER_TEXT_MAX];
  uint32_t duration_ms;
  uint32_t kind;             /* ZZPLAY_ITEM_* */
  uint32_t playable;
  uint32_t favourite;        /* 1 when in favourites */
  uint32_t editable;         /* opens the provider form from Edit */
} ZZPlayBrowseItem;

/* A service's Recently played list, newest first. */
typedef struct ZZPlayBrowseRecent {
  ZZPlayBrowseItem items[ZZPLAY_BROWSE_RECENT_MAX];
  uint32_t count;
} ZZPlayBrowseRecent;

typedef enum ZZPlayBrowseAction {
  ZZPLAY_BROWSE_ACTION_NONE = 0,
  ZZPLAY_BROWSE_ACTION_ENTER_FOLDER,
  ZZPLAY_BROWSE_ACTION_PLAY,
  ZZPLAY_BROWSE_ACTION_PAGE_MORE,
  ZZPLAY_BROWSE_ACTION_OPEN_SETTINGS,
  ZZPLAY_BROWSE_ACTION_OPEN_FORM
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
  ZZPLAY_BROWSE_REQ_CLOSE,
  ZZPLAY_BROWSE_REQ_LINK_START,
  ZZPLAY_BROWSE_REQ_LINK_POLL,
  ZZPLAY_BROWSE_REQ_LINK_CHOOSE,
  ZZPLAY_BROWSE_REQ_FORM_GET,
  ZZPLAY_BROWSE_REQ_FORM_SUBMIT,
  ZZPLAY_BROWSE_REQ_FORM_DELETE
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
  int reopen;                /* close and reopen the module first: its
                                settings changed */
  ZZPlayBrowseItem item;
  char link_id[ZZPLAY_PROVIDER_TEXT_MAX];
  char choice_id[ZZPLAY_PROVIDER_TEXT_MAX];
  char form_values[ZZPLAY_PROVIDER_FORM_FIELDS_MAX][ZZPLAY_PROVIDER_VALUE_MAX];
  uint32_t form_value_count;
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
  ZZPlayProviderLinkStart link_start;
  ZZPlayProviderLinkResult link_result;
  ZZPlayProviderForm form;
  char form_title[ZZPLAY_PROVIDER_TEXT_MAX];
  char form_values[ZZPLAY_PROVIDER_FORM_FIELDS_MAX][ZZPLAY_PROVIDER_VALUE_MAX];
} ZZPlayBrowseReply;

typedef struct ZZPlayBrowseFrame {
  char node_id[ZZPLAY_PROVIDER_TEXT_MAX];
  char title[ZZPLAY_PROVIDER_TEXT_MAX];
  char subtitle[ZZPLAY_PROVIDER_TEXT_MAX];
  int is_search;
  char search_query[ZZPLAY_PROVIDER_TEXT_MAX];
  /* 1 when the folder this frame returns to is a playable collection. */
  int folder_is_collection;
  int32_t selection;
  uint32_t scroll;
  ZZPlayBrowseItem *items;
  uint32_t item_count;
  uint32_t item_capacity;
  uint32_t total;
  int has_total;
  int has_more;
  /* 1 when the list was still loading as it was left: Back asks again. */
  int incomplete;
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
  /* 1 when a remembered folder is a playable collection. */
  int folder_is_collection;
  uint32_t start;
  ZZPlayBrowseItem *items;
  uint32_t item_count;
  /* Paging state of the list when it was remembered. */
  uint32_t total;
  int has_total;
  int has_more;
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
  char current_subtitle[ZZPLAY_PROVIDER_TEXT_MAX];
  int is_search;
  char search_query[ZZPLAY_PROVIDER_TEXT_MAX];

  /* 1 when the folder being shown is one the module marks playable, so
   * Play all / Add all work on it. Never set for a search or the top
   * level. */
  int folder_is_collection;

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

  /* Recently played list of the current service. */
  ZZPlayBrowseRecent recent;

  /* The GATHER request in flight, while it is the latest request. */
  int gathering;
  uint32_t gather_generation;
  /* The last Recently played save failed and was reported. */
  int recent_save_failed;
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
/* The service's settings changed: forget its remembered lists and show
 * its top level again through a LIST request that reopens the module. */
int zzplay_browse_model_reopen_service(ZZPlayBrowseModel *model,
                                       ZZPlayBrowseRequest *out_req);

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

/* Play all / Add all act on the folder being shown: true only when that
 * folder is a collection the module marks playable, its rows are shown
 * and no request is running. */
int zzplay_browse_model_can_play_all(const ZZPlayBrowseModel *model);
int zzplay_browse_model_can_add_all(const ZZPlayBrowseModel *model);

/* Start gathering the folder being shown into a GATHER request that asks
 * for at most `limit` items (the playlist's free space for Add all, its
 * capacity for Play all). Returns 0 when the shown folder is no
 * collection, has no rows, or a request is running. */
int zzplay_browse_model_gather_shown(ZZPlayBrowseModel *model, uint32_t limit,
                                     ZZPlayBrowseRequest *out_req);
/* Replies of a gather count only while it is the latest request: going
 * to another folder, back, searching, reloading, stopping, choosing a
 * service or starting another gather supersedes it. */
int zzplay_browse_model_gather_live(const ZZPlayBrowseModel *model,
                                    uint32_t generation);
/* The live gather's final reply has been handled. */
void zzplay_browse_model_gather_done(ZZPlayBrowseModel *model);
int zzplay_browse_model_provider_has_favourite(const ZZPlayProvider *provider);
int zzplay_browse_model_provider_has_link(const ZZPlayProvider *provider);
int zzplay_browse_model_can_favourite(const ZZPlayBrowseModel *model);
int zzplay_browse_model_favourite(ZZPlayBrowseModel *model,
                                  ZZPlayBrowseRequest *out_req);
/* A favourite change for `id` was saved: Recently played keeps the new
 * state, in its list and in its shown rows. Returns 1 when the list
 * changed and should be saved. */
int zzplay_browse_model_favourite_done(ZZPlayBrowseModel *model,
                                       const char *id, int favourite);
/* True when the selected row is editable by the provider. */
int zzplay_browse_model_can_edit(const ZZPlayBrowseModel *model);
ZZPlayBrowseAction zzplay_browse_model_activate(ZZPlayBrowseModel *model,
                                                int32_t index,
                                                ZZPlayBrowseRequest *out_req);
int zzplay_browse_model_format_uri(const ZZPlayBrowseModel *model,
                                   int32_t index,
                                   char *uri, size_t uri_size,
                                   char *title, size_t title_size);

/* Reload: fetch the current list or search again from start 0. In the
 * Recently played folder it shows the list again and asks for nothing
 * (`out_req` kind NONE). */
void zzplay_browse_model_stop(ZZPlayBrowseModel *model);
int zzplay_browse_model_can_reload(const ZZPlayBrowseModel *model);
int zzplay_browse_model_reload(ZZPlayBrowseModel *model,
                              ZZPlayBrowseRequest *out_req);

/* Applying worker replies. Returns 1 if accepted, 0 if dropped. */
int zzplay_browse_model_apply_reply(ZZPlayBrowseModel *model,
                                    const ZZPlayBrowseReply *reply);

/* Keyboard selection. Move the selection `delta` rows (negative: up),
 * stopping at the first and the last row; with nothing selected, moving
 * down selects the first row and up the last. Returns the new selection,
 * -1 for an empty list. */
int32_t zzplay_browse_model_move_selection(ZZPlayBrowseModel *model,
                                           int32_t delta);
/* Select the next row after the selection (from the top when nothing is
 * selected, wrapping round) whose title starts with `letter`, in either
 * case. Returns its index, or -1 with the selection unchanged. */
int32_t zzplay_browse_model_jump_to_letter(ZZPlayBrowseModel *model,
                                           int letter);

/* Rows the service sent for the view: the Recently played row is not
 * counted. */
uint32_t zzplay_browse_model_service_rows(const ZZPlayBrowseModel *model);
/* Format the view's row count ("1 item", "6 items", or "X of Y items"
 * when total is known and larger than the rows shown); writes "" and
 * returns 0 when no rows are shown. Counts what is shown (including the
 * "Recently played" folder row at the top level). */
size_t zzplay_browse_model_format_count(const ZZPlayBrowseModel *model,
                                        char *buf, size_t size);
/* True while the Recently played folder is shown. */
int zzplay_browse_model_in_recent(const ZZPlayBrowseModel *model);
/* The folder being shown as a row, for Play all and Play all next:
 * returns 1 and fills `out` when it is a playable collection. */
int zzplay_browse_model_shown_collection(const ZZPlayBrowseModel *model,
                                         ZZPlayBrowseItem *out);
/* Replace the current service's Recently played list with the lines of
 * its saved file (NULL or empty: none), and show or hide the folder row
 * at the top level to match. */
void zzplay_browse_model_load_recent(ZZPlayBrowseModel *model,
                                     const char *text, size_t length);
/* `item` was started from Browse: put it first in the current service's
 * Recently played list. Returns 1 when the list changed (save it). */
int zzplay_browse_model_note_started(ZZPlayBrowseModel *model,
                                     const ZZPlayBrowseItem *item);
/* A Recently played save ended (`saved` 1 when it worked). Returns 1
 * when a failure should be reported: the first since the last save that
 * worked. The list itself stays, so the next change saves it again. */
int zzplay_browse_model_note_recent_save(ZZPlayBrowseModel *model,
                                         int saved);

/* Format the list and hand it to `write`; 1 when it was written. */
typedef int (*ZZPlayBrowseRecentWriter)(void *user, const char *text,
                                        uint32_t length);
int zzplay_browse_recent_save(const ZZPlayBrowseRecent *recent,
                              ZZPlayBrowseRecentWriter writer, void *user);

/* Recently played list logic. add puts `item` first; an entry with the
 * same kind and id moves there instead of appearing twice, and the oldest
 * drops off past ZZPLAY_BROWSE_RECENT_MAX. Returns 0 for an item with no
 * id. */
void zzplay_browse_recent_clear(ZZPlayBrowseRecent *recent);
int zzplay_browse_recent_add(ZZPlayBrowseRecent *recent,
                             const ZZPlayBrowseItem *item);
/* Read the saved form: one entry per line, newest first, as
 * "kind<TAB>id<TAB>title<TAB>subtitle<TAB>flags" with kind "folder",
 * "track" or "station" and flags one digit: 1 playable plus 2 a
 * favourite. Damaged lines (missing or extra fields, an unknown kind, an
 * empty or over-long id) are skipped, and so are repeats. Returns the
 * number of entries read. */
uint32_t zzplay_browse_recent_parse(ZZPlayBrowseRecent *recent,
                                    const char *text, size_t length);
/* Write the saved form. Returns the bytes needed, excluding the NUL; the
 * output is cut (still NUL-terminated) when `capacity` is too small. */
size_t zzplay_browse_recent_format(const ZZPlayBrowseRecent *recent,
                                   char *out, size_t capacity);

#endif /* ZZPLAY_BROWSE_MODEL_H */
