/* Service browser window for ZZPlay (GadTools, AmigaOS 3.0+).
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-browse.h"

#include <stdio.h>
#include <string.h>

#include "zzplay-browse-model.h"
#include "zzplay-browse-rows.h"
#include "zzplay-browse-worker.h"
#include "zzplay-host.h"
#include "zzplay-http.h"
#include "zzplay-net.h"
#include "zzplay-providers.h"
#include "zzplay-sys.h"

#if defined(__amigaos__) || defined(AMIGA)

#include <SDI_compiler.h>
#include <graphics/gfxbase.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#define BROWSE_MARGIN 6
#define BROWSE_SPACING 4
/* Space between the marker, title, subtitle and length cells. */
#define BROWSE_COLUMN_GAP 6
#define BROWSE_FIELDS_MAX 8U
#define BROWSE_CHOICES_MAX 16U

enum BrowseGadgetID {
  BGID_SERVICE = 1,
  BGID_SEARCH,
  BGID_LIST,
  BGID_BACK,
  BGID_RELOAD,
  BGID_PLAY,
  BGID_ADD,
  BGID_FAVOURITE,
  BGID_STOP,
  BGID_SETTINGS,
  BGID_STATUS,
  SGID_SAVE,
  SGID_CANCEL,
  SGID_FIELD_BASE
};

static struct Window *browse_win = NULL;
static struct Window *settings_win = NULL;
static APTR browse_vi = NULL;
static APTR settings_vi = NULL;
static struct Gadget *browse_glist = NULL;
static struct Gadget *settings_glist = NULL;

static struct Gadget *service_gad = NULL;
static struct Gadget *search_gad = NULL;
static struct Gadget *list_gad = NULL;
static struct Gadget *back_gad = NULL;
static struct Gadget *reload_gad = NULL;
static struct Gadget *play_gad = NULL;
static struct Gadget *add_gad = NULL;
static struct Gadget *fav_gad = NULL;
static struct Gadget *stop_gad = NULL;
static struct Gadget *settings_btn_gad = NULL;
static struct Gadget *status_gad = NULL;
static struct Gadget *field_gads[BROWSE_FIELDS_MAX];
static struct TextAttr *browse_text_attr = NULL;
static struct List browse_exec_list;
static ZZPlayBrowseModel browse_model;
static int model_inited = 0;

static ZZPlayController *gui_ctl = NULL;
static ZZPlayPlaylist *gui_playlist = NULL;
static struct Screen *gui_screen = NULL;

static char service_labels_storage[ZZPLAY_BROWSE_SERVICES_MAX][ZZPLAY_PROVIDER_TEXT_MAX];
static const char *service_labels[ZZPLAY_BROWSE_SERVICES_MAX + 1U];

static char search_buf[ZZPLAY_PROVIDER_TEXT_MAX];
static char status_buf[ZZPLAY_PROVIDER_ERROR_MAX];
static char field_bufs[BROWSE_FIELDS_MAX][ZZPLAY_PROVIDER_TEXT_MAX];
static char secret_orig[BROWSE_FIELDS_MAX][ZZPLAY_PROVIDER_TEXT_MAX];
/* CHOICE fields: how many choices the form found and which is selected.
 * Zero means the field is shown as a text field instead. */
static int field_choice_n[BROWSE_FIELDS_MAX];
static int field_choice_sel[BROWSE_FIELDS_MAX];

static int32_t last_clicked_idx = -1;
static ZZPlayBrowseReply poll_worker_reply;
static ZZPlayBrowseRequest poll_req;
static char poll_uri[ZZPLAY_PLAYLIST_PATH_MAX];
static int last_fav_add = 0;
static char poll_title[ZZPLAY_PLAYLIST_TITLE_MAX];
static ULONG last_click_s = 0UL;
static ULONG last_click_m = 0UL;
/* The request waiting for the running one to be cancelled. */
static ZZPlayBrowseRequest pending_req;
static const ZZPlayProvider *pending_provider = NULL;
static int have_pending = 0;
/* Shown again once the list reloaded after a favourite changed. */
static const char *reload_note = NULL;
/* Last status text pushed to the gadget (dedupes polling refreshes). */
static char status_shown[ZZPLAY_PROVIDER_ERROR_MAX];
/* Stage text drawn from the worker's stage while a request runs. */
static char stage_host[96];
/* Collection gathering: the request in flight and what its rows do when
 * they have all arrived. The playlist is only touched at the end, so
 * Stop until then leaves it unchanged. */
static int gather_active = 0;
static int gather_add = 0;     /* 1: append to the playlist, 0: replace */
static uint32_t gather_generation = 0U;
static uint32_t gather_seen = 0U;
static char gather_scheme[ZZPLAY_PROVIDER_SCHEME_MAX];

/* The prefs the window remembers its place and size in, set at open. */
static ZZPlayPrefs *browse_prefs = NULL;
/* Aligned column rows through a GTLV_CallBack render hook; needs the
 * V39 gadtools. Without it the window keeps its one-line rows. */
static int browse_columns_ok = 0;
static int browse_marker_col_w = 0;
static int browse_len_col_w = 0;
static struct TextFont *browse_font = NULL;
/* What the list shows and where it is scrolled, kept across the gadget
 * rebuilds a resize causes. */
static int browse_list_top = 0;
static int built_inner_w = -1;
static int built_inner_h = -1;

/* One list row. The strings live here, not in the model: the render
 * hook runs where GadTools renders (this task, or Intuition's input
 * task during a scroll drag), which must not read memory the GUI task
 * is about to grow or replace. */
struct browse_row_node {
  struct Node node; /* ln_Name points at title */
  int32_t index;    /* row in browse_model.items, -1 = the More row */
  uint32_t kind;
  uint32_t playable;
  char duration[12]; /* "88:88:88" or "" when unknown */
  char title[ZZPLAY_PROVIDER_TEXT_MAX];
  char subtitle[ZZPLAY_PROVIDER_TEXT_MAX];
};

static LONG browse_row_render(REG(a0, struct Hook *hook),
                              REG(a2, struct Node *node),
                              REG(a1, struct LVDrawMsg *msg));

static struct Hook browse_list_hook = {
  {0, 0}, (ULONG (*)())browse_row_render, 0, 0
};

/* Text width in the font the rows render in. */
static int browse_text_len(struct RastPort *rp, const char *s)
{
  return (int)TextLength(rp, (CONST_STRPTR)s, (ULONG)strlen(s));
}

/* Draws `s` cut to `max_w` pixels: TextFit counts the characters that
 * fit, and the cut happens at that count, never inside a cell's
 * neighbour. */
static void browse_draw_text(struct RastPort *rp, const char *s, int x,
                             int y, int max_w)
{
  struct TextExtent fit;
  ULONG n;

  if (!s || s[0] == '\0' || max_w <= 0) {
    return;
  }
  n = TextFit(rp, (CONST_STRPTR)s, (ULONG)strlen(s), &fit, NULL, 1L,
              (ULONG)max_w, 0x3FFFU);
  if (n == 0U) {
    return;
  }
  Move(rp, (WORD)x, (WORD)y);
  Text(rp, (CONST_STRPTR)s, n);
}

/* The row marker, an eight by seven pixel glyph: an outline folder for a
 * plain folder, the folder with a filled play triangle for a collection
 * Play or Add works on whole, a note for a track and a mast with waves
 * for a station. */
static void browse_draw_marker(struct RastPort *rp, int x, int top,
                               uint32_t kind, uint32_t playable)
{
  if (kind == ZZPLAY_ITEM_FOLDER) {
    RectFill(rp, (WORD)x, (WORD)top, (WORD)x, (WORD)(top + 6));
    RectFill(rp, (WORD)(x + 7), (WORD)top, (WORD)(x + 7), (WORD)(top + 6));
    RectFill(rp, (WORD)(x + 1), (WORD)top, (WORD)(x + 6), (WORD)top);
    RectFill(rp, (WORD)(x + 1), (WORD)(top + 6), (WORD)(x + 6),
             (WORD)(top + 6));
    RectFill(rp, (WORD)(x + 1), (WORD)(top + 1), (WORD)(x + 3),
             (WORD)(top + 1));
    if (playable) {
      RectFill(rp, (WORD)(x + 2), (WORD)(top + 2), (WORD)(x + 3),
               (WORD)(top + 2));
      RectFill(rp, (WORD)(x + 2), (WORD)(top + 3), (WORD)(x + 4),
               (WORD)(top + 3));
      RectFill(rp, (WORD)(x + 2), (WORD)(top + 4), (WORD)(x + 4),
               (WORD)(top + 4));
      RectFill(rp, (WORD)(x + 2), (WORD)(top + 5), (WORD)(x + 3),
               (WORD)(top + 5));
    }
  } else if (kind == ZZPLAY_ITEM_STATION) {
    RectFill(rp, (WORD)(x + 3), (WORD)(top + 2), (WORD)(x + 4),
             (WORD)(top + 3));
    RectFill(rp, (WORD)(x + 3), (WORD)(top + 4), (WORD)(x + 3),
             (WORD)(top + 6));
    Move(rp, (WORD)(x + 1), (WORD)(top + 5));
    Draw(rp, (WORD)(x + 2), (WORD)(top + 4));
    Move(rp, (WORD)(x + 6), (WORD)(top + 5));
    Draw(rp, (WORD)(x + 5), (WORD)(top + 4));
  } else {
    RectFill(rp, (WORD)(x + 1), (WORD)(top + 4), (WORD)(x + 3),
             (WORD)(top + 6));
    RectFill(rp, (WORD)(x + 4), (WORD)(top + 1), (WORD)(x + 4),
             (WORD)(top + 6));
    RectFill(rp, (WORD)(x + 5), (WORD)(top + 1), (WORD)(x + 6),
             (WORD)(top + 2));
  }
}

/* Renders one row: marker, title, subtitle, then the length right
 * aligned when known. GadTools calls this hook wherever it renders the
 * list, which includes Intuition's input task during a scroll drag, so
 * it only reads the row node's own memory and draws: no allocation, no
 * waiting. Selected rows use the selection pens, as GadTools' own rows
 * do. */
static LONG browse_row_render(REG(a0, struct Hook *hook),
                              REG(a2, struct Node *node),
                              REG(a1, struct LVDrawMsg *msg))
{
  struct browse_row_node *row;
  struct RastPort *rp;
  struct DrawInfo *dri;
  ZZPlayBrowseColumns cols;
  int x, y, w, h, text_y, pen_text, len;

  (void)hook;
  if (!msg || !node || msg->lvdm_MethodID != (ULONG)LV_DRAW ||
      !msg->lvdm_RastPort || !msg->lvdm_DrawInfo || !browse_font) {
    return (LONG)LVCB_UNKNOWN;
  }
  rp = msg->lvdm_RastPort;
  dri = msg->lvdm_DrawInfo;
  x = (int)msg->lvdm_Bounds.MinX;
  y = (int)msg->lvdm_Bounds.MinY;
  w = (int)msg->lvdm_Bounds.MaxX - x + 1;
  h = (int)msg->lvdm_Bounds.MaxY - y + 1;
  if (w <= 0 || h <= 0) {
    return (LONG)LVCB_OK;
  }
  row = (struct browse_row_node *)node;

  SetAPen(rp, (ULONG)((msg->lvdm_State == (ULONG)LVR_SELECTED)
                          ? dri->dri_Pens[FILLPEN]
                          : dri->dri_Pens[BACKGROUNDPEN]));
  RectFill(rp, (WORD)x, (WORD)y, (WORD)(x + w - 1), (WORD)(y + h - 1));
  pen_text = (int)((msg->lvdm_State == (ULONG)LVR_SELECTED)
                       ? dri->dri_Pens[FILLTEXTPEN]
                       : dri->dri_Pens[TEXTPEN]);
  SetAPen(rp, (ULONG)pen_text);
  SetFont(rp, browse_font);
  text_y = y + ((h - (int)browse_font->tf_YSize) >> 1) +
           (int)browse_font->tf_Baseline;

  zzplay_browse_layout_columns(w, browse_marker_col_w, browse_len_col_w,
                               BROWSE_COLUMN_GAP, &cols);

  if (row->index < 0) {
    /* "--- More items ---" stays a plain centred row. */
    len = browse_text_len(rp, row->title);
    x += (w - len) >> 1;
    if (x < (int)msg->lvdm_Bounds.MinX) {
      x = (int)msg->lvdm_Bounds.MinX;
    }
    browse_draw_text(rp, row->title, x, text_y, w);
    return (LONG)LVCB_OK;
  }

  browse_draw_marker(rp, (int)msg->lvdm_Bounds.MinX + cols.marker_x,
                     y + ((h - 7) >> 1), row->kind, row->playable);
  browse_draw_text(rp, row->title, (int)msg->lvdm_Bounds.MinX + cols.title_x,
                   text_y, cols.title_w);
  browse_draw_text(rp, row->subtitle,
                   (int)msg->lvdm_Bounds.MinX + cols.sub_x, text_y,
                   cols.sub_w);
  if (row->duration[0] != '\0') {
    len = browse_text_len(rp, row->duration);
    if (len > cols.len_w) {
      len = cols.len_w;
    }
    browse_draw_text(rp, row->duration,
                     (int)msg->lvdm_Bounds.MinX + cols.len_x +
                         (cols.len_w - len),
                     text_y, cols.len_w);
  }
  return (LONG)LVCB_OK;
}

extern struct IntuitionBase *IntuitionBase;

/* Busy pointer on the Browse window and the settings form while a
 * request runs. V39+ only; older Intuition simply keeps the arrow. */
static void set_window_busy(struct Window *win, int busy)
{
  struct TagItem tags[3];

  if (!win || !IntuitionBase ||
      IntuitionBase->LibNode.lib_Version < 39) {
    return;
  }
  if (busy) {
    tags[0].ti_Tag = WA_BusyPointer;
    tags[0].ti_Data = TRUE;
    tags[1].ti_Tag = WA_PointerDelay;
    tags[1].ti_Data = TRUE;
    tags[2].ti_Tag = TAG_DONE;
  } else {
    tags[0].ti_Tag = TAG_DONE;
  }
  SetWindowPointerA(win, tags);
}

static void update_busy_pointer(void)
{
  int busy = browse_model.busy || zzplay_browse_worker_active();

  set_window_busy(browse_win, busy);
  set_window_busy(settings_win, busy);
}

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

static void init_exec_list(struct List *list)
{
  list->lh_TailPred = (struct Node *)list;
  list->lh_Head = (struct Node *)&list->lh_Tail;
  list->lh_Tail = 0;
}

static void free_exec_list_nodes(struct List *list)
{
  struct Node *node;

  if (!list || !list->lh_Head) {
    return;
  }
  /* Nodes are struct browse_row_node; ln_Name points into the node, so
   * freeing the node frees its strings too. */
  while ((node = RemHead(list)) != 0) {
    zzplay_free(node);
  }
}

static struct Gadget *create_gadget(struct Window *win, ULONG kind,
                                    struct Gadget *prev, struct NewGadget *ng,
                                    int x, int y, int w, int h,
                                    const char *label, UWORD id, ULONG flags,
                                    const struct TagItem *tags)
{
  if (!prev || !win) {
    return NULL;
  }
  ng->ng_LeftEdge = (WORD)(win->BorderLeft + x);
  ng->ng_TopEdge = (WORD)(win->BorderTop + y);
  ng->ng_Width = (WORD)w;
  ng->ng_Height = (WORD)h;
  ng->ng_GadgetText = (CONST_STRPTR)label;
  ng->ng_GadgetID = id;
  ng->ng_Flags = flags;
  return CreateGadgetA(kind, prev, ng, (struct TagItem *)tags);
}

static void update_status_text(const char *msg)
{
  if (!msg) {
    msg = "";
  }
  if (strcmp(status_shown, msg) == 0) {
    return;
  }
  (void)snprintf(status_shown, sizeof(status_shown), "%s", msg);
  if (!browse_win || !status_gad) {
    return;
  }
  (void)snprintf(status_buf, sizeof(status_buf), "%s", msg);
  GT_SetGadgetAttrs(status_gad, browse_win, NULL,
                    GTTX_Text, (ULONG)status_buf,
                    TAG_DONE);
}
/* True while the gathering request is still the one being waited for. */
static int gather_live(void)
{
  return gather_active && gather_generation == browse_model.generation;
}


/* While a request runs, name what it waits for; once rows arrive, count
 * them. Returns 1 when stage text was drawn. */
static int update_stage_status(void)
{
  int stage;

  if (gather_live()) {
    if (gather_seen > 0U) {
      (void)snprintf(status_buf, sizeof(status_buf),
                     "Adding %lu tracks...", (unsigned long)gather_seen);
    } else {
      (void)snprintf(status_buf, sizeof(status_buf), "Adding tracks...");
    }
    update_status_text(status_buf);
    return 1;
  }
  if (!browse_model.busy) {
    return 0;
  }
  if (browse_model.item_count > 0U) {
    if (browse_model.has_total) {
      (void)snprintf(status_buf, sizeof(status_buf),
                     "Loading %lu of %lu items...",
                     (unsigned long)browse_model.item_count,
                     (unsigned long)browse_model.total);
    } else {
      (void)snprintf(status_buf, sizeof(status_buf), "Loading %lu items...",
                     (unsigned long)browse_model.item_count);
    }
    update_status_text(status_buf);
    return 1;
  }
  stage = zzplay_browse_worker_get_stage(stage_host, sizeof(stage_host));
  if (stage == ZZPLAY_NET_STAGE_LOOKUP) {
    (void)snprintf(status_buf, sizeof(status_buf), "Looking up %s...",
                   stage_host);
  } else if (stage == ZZPLAY_NET_STAGE_CONNECT) {
    (void)snprintf(status_buf, sizeof(status_buf), "Connecting to %s...",
                   stage_host);
  } else if (stage == ZZPLAY_NET_STAGE_SECURE) {
    (void)snprintf(status_buf, sizeof(status_buf), "Connecting securely...");
  } else if (stage == ZZPLAY_NET_STAGE_WAIT) {
    (void)snprintf(status_buf, sizeof(status_buf), "Waiting for %s...",
                   stage_host);
  } else if (stage == ZZPLAY_NET_STAGE_BODY) {
    (void)snprintf(status_buf, sizeof(status_buf), "Loading items...");
  } else {
    return 0;
  }
  update_status_text(status_buf);
  return 1;
}

/* The worker runs one request at a time. A new request (the user opened
 * a folder, searched, went back) supersedes a running one: cancel that and
 * send the new one once its reply, now stale, has come back. */
static void browse_send(const ZZPlayBrowseRequest *req,
                        const ZZPlayProvider *provider)
{
  if (!req || !provider) {
    return;
  }
  if (!zzplay_browse_worker_active()) {
    have_pending = 0;
    (void)zzplay_browse_worker_send(req, provider);
    update_busy_pointer();
    return;
  }
  pending_provider = provider;
  have_pending = 1;
  zzplay_browse_worker_cancel();
}

static void update_buttons(void)
{
  if (!browse_win) {
    return;
  }
  if (back_gad) {
    GT_SetGadgetAttrs(back_gad, browse_win, NULL,
                      GA_Disabled, (ULONG)!zzplay_browse_model_can_back(&browse_model),
                      TAG_DONE);
  }
  if (reload_gad) {
    GT_SetGadgetAttrs(reload_gad, browse_win, NULL,
                      GA_Disabled,
                      (ULONG)!zzplay_browse_model_can_reload(&browse_model),
                      TAG_DONE);
  }
  if (play_gad) {
    GT_SetGadgetAttrs(play_gad, browse_win, NULL,
                      GA_Disabled, (ULONG)!zzplay_browse_model_can_play(&browse_model),
                      TAG_DONE);
  }
  if (add_gad) {
    GT_SetGadgetAttrs(add_gad, browse_win, NULL,
                      GA_Disabled, (ULONG)!zzplay_browse_model_can_add(&browse_model),
                      TAG_DONE);
  }
  if (fav_gad) {
    GT_SetGadgetAttrs(fav_gad, browse_win, NULL,
                      GA_Disabled, (ULONG)!zzplay_browse_model_can_favourite(&browse_model),
                      TAG_DONE);
  }
  if (stop_gad) {
    GT_SetGadgetAttrs(stop_gad, browse_win, NULL,
                      GA_Disabled, (ULONG)!browse_model.busy,
                      TAG_DONE);
  }
}

static void rebuild_list_nodes(void)
{
  /* Which list the gadget shows, so a different list starts at its top
   * (or its remembered selection) while rows appended to the same list
   * leave the view where it is. */
  static char shown_key[2 * ZZPLAY_PROVIDER_TEXT_MAX + 16];
  static uint32_t shown_count;
  static char key[2 * ZZPLAY_PROVIDER_TEXT_MAX + 16];
  struct TagItem tags[4];
  int fresh;
  uint32_t i;
  uint32_t rows = 0U;

  if (!browse_win || !list_gad) {
    return;
  }

  GT_SetGadgetAttrs(list_gad, browse_win, NULL,
                    GTLV_Labels, (ULONG)~0UL,
                    TAG_DONE);

  free_exec_list_nodes(&browse_exec_list);
  init_exec_list(&browse_exec_list);

  for (i = 0U; i < browse_model.item_count; i++) {
    const ZZPlayBrowseItem *it = &browse_model.items[i];
    struct browse_row_node *row =
        (struct browse_row_node *)zzplay_alloc(sizeof(*row));

    if (!row) {
      break;
    }
    memset(row, 0, sizeof(*row));
    row->index = (int32_t)i;
    row->kind = it->kind;
    row->playable = it->playable;
    zzplay_browse_format_duration(it->duration_ms, row->duration,
                                  sizeof(row->duration));
    if (browse_columns_ok) {
      /* The render hook draws the columns from these. */
      copy_string(row->title, sizeof(row->title), it->title);
      copy_string(row->subtitle, sizeof(row->subtitle), it->subtitle);
    } else {
      /* One-line rows, what the window showed before the column
       * layout: "[+] folder" or "title - subtitle". */
      if (it->kind == ZZPLAY_ITEM_FOLDER) {
        (void)snprintf(row->title, sizeof(row->title), "[+] %s", it->title);
      } else if (it->subtitle[0] != '\0') {
        (void)snprintf(row->title, sizeof(row->title), "%s - %s",
                       it->title, it->subtitle);
      } else {
        copy_string(row->title, sizeof(row->title), it->title);
      }
    }
    row->node.ln_Name = row->title;
    AddTail(&browse_exec_list, &row->node);
    rows++;
  }

  /* Synthetic More row, a plain centred row either way */
  if (browse_model.has_more) {
    struct browse_row_node *row =
        (struct browse_row_node *)zzplay_alloc(sizeof(*row));
    if (row) {
      memset(row, 0, sizeof(*row));
      row->index = -1;
      copy_string(row->title, sizeof(row->title), "--- More items ---");
      row->node.ln_Name = row->title;
      AddTail(&browse_exec_list, &row->node);
      rows++;
    }
  }

  (void)snprintf(key, sizeof(key), "%ld/%d/%s/%s",
                 (long)browse_model.current_service, browse_model.is_search,
                 browse_model.current_node,
                 browse_model.is_search ? browse_model.search_query : "");
  fresh = strcmp(key, shown_key) != 0 ||
          browse_model.item_count < shown_count;
  (void)snprintf(shown_key, sizeof(shown_key), "%s", key);
  shown_count = browse_model.item_count;

  tags[0].ti_Tag = GTLV_Labels;
  tags[0].ti_Data = (ULONG)&browse_exec_list;
  tags[1].ti_Tag = GTLV_Selected;
  tags[1].ti_Data = browse_model.selection >= 0
                        ? (ULONG)browse_model.selection : ~0UL;
  if (fresh) {
    browse_list_top = browse_model.selection >= 0
                          ? (int)browse_model.selection : 0;
  } else if (browse_list_top > 0 && rows > 0U &&
             (uint32_t)browse_list_top > rows - 1U) {
    browse_list_top = (int)(rows - 1U);
  }
  tags[2].ti_Tag = fresh ? GTLV_Top : TAG_IGNORE;
  tags[2].ti_Data = (ULONG)browse_list_top;
  tags[3].ti_Tag = TAG_DONE;
  tags[3].ti_Data = 0;
  GT_SetGadgetAttrsA(list_gad, browse_win, NULL, tags);
}

static void close_settings_window(void)
{
  if (settings_win) {
    if (settings_win->UserPort) {
      struct IntuiMessage *msg;
      while ((msg = GT_GetIMsg(settings_win->UserPort)) != NULL) {
        GT_ReplyIMsg(msg);
      }
    }
    CloseWindow(settings_win);
    settings_win = NULL;
  }
  if (settings_glist) {
    FreeGadgets(settings_glist);
    settings_glist = NULL;
  }
  if (settings_vi) {
    FreeVisualInfo(settings_vi);
    settings_vi = NULL;
  }
}

/* Width of `text` in the screen font, which every gadget here uses. */
static int browse_text_w(const char *text)
{
  return (int)TextLength(&gui_screen->RastPort, (CONST_STRPTR)text,
                         (ULONG)strlen(text));
}

static void __attribute__((noinline)) open_settings_window(void)
{
  const ZZPlayBrowseServiceEntry *svc;
  const ZZPlayProvider *p;
  struct NewGadget ng;
  struct Gadget *gad;
  int font_h;
  int btn_h;
  int win_w;
  int cur_y;
  int label_w;
  int val_w;
  int val_min_w;
  int btn_w;
  uint32_t i;
  uint32_t count;

  if (settings_win) {
    WindowToFront(settings_win);
    ActivateWindow(settings_win);
    return;
  }

  svc = zzplay_browse_model_current_service(&browse_model);
  p = svc ? svc->provider : NULL;
  if (!p || p->field_count == 0U) {
    update_status_text("Service has no settings");
    return;
  }

  settings_vi = GetVisualInfo(gui_screen, TAG_DONE);
  if (!settings_vi) {
    return;
  }

  count = p->field_count;
  if (count > BROWSE_FIELDS_MAX) {
    count = BROWSE_FIELDS_MAX;
  }
  struct TextFont *font = gui_screen->RastPort.Font;
  font_h = (font && font->tf_YSize > 0) ? (int)font->tf_YSize : 8;
  btn_h = font_h + 6;
  label_w = 0;
  val_min_w = 260; /* room to type an address, as the form always had */
  for (i = 0U; i < count; i++) {
    const ZZPlayProviderField *f = &p->fields[i];
    int w = browse_text_w(f->label);
    if (w > label_w) {
      label_w = w;
    }
    if (f->kind == ZZPLAY_FIELD_CHOICE && f->choice_labels != NULL) {
      /* The cycle gadget needs its widest choice plus its glyph */
      uint32_t c;
      w = 0;
      for (c = 0U; c < BROWSE_CHOICES_MAX && f->choice_labels[c] != NULL; c++) {
        int cw = browse_text_w(f->choice_labels[c]);
        if (cw > w) {
          w = cw;
        }
      }
      w += 32;
      if (w > val_min_w) {
        val_min_w = w;
      }
    }
  }
  label_w += 12;
  btn_w = browse_text_w("Cancel") + 24;
  win_w = label_w + val_min_w + 2 * BROWSE_MARGIN;
  if (win_w < 2 * btn_w + BROWSE_SPACING + 2 * BROWSE_MARGIN) {
    win_w = 2 * btn_w + BROWSE_SPACING + 2 * BROWSE_MARGIN;
  }
  val_w = win_w - 2 * BROWSE_MARGIN - label_w;

  settings_glist = NULL;
  gad = CreateContext(&settings_glist);
  if (!gad) {
    close_settings_window();
    return;
  }

  memset(&ng, 0, sizeof(ng));
  ng.ng_VisualInfo = settings_vi;
  ng.ng_TextAttr = browse_text_attr;

  /* Sizing window */
  cur_y = BROWSE_MARGIN;
  for (i = 0U; i < count; i++) {
    cur_y += btn_h + BROWSE_SPACING;
  }
  cur_y += btn_h + BROWSE_MARGIN;

  ng.ng_TextAttr = browse_text_attr;

  settings_win = OpenWindowTags(
      NULL,
      WA_Title, (ULONG)"Service Settings",
      WA_Left, browse_win ? (browse_win->LeftEdge + 30) : 60,
      WA_Top, browse_win ? (browse_win->TopEdge + 30) : 60,
      WA_InnerWidth, (ULONG)win_w,
      WA_InnerHeight, (ULONG)cur_y,
      WA_DragBar, TRUE,
      WA_DepthGadget, TRUE,
      WA_CloseGadget, TRUE,
      WA_Activate, TRUE,
      WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW,
      WA_CustomScreen, (ULONG)gui_screen,
      TAG_DONE);

  if (!settings_win) {
    close_settings_window();
    return;
  }

  cur_y = BROWSE_MARGIN;
  for (i = 0U; i < count; i++) {
    const ZZPlayProviderField *f = &p->fields[i];
    struct TagItem cyc_tags[3];
    struct TagItem str_tags[4];
    int is_choice;

    secret_orig[i][0] = '\0';
    field_bufs[i][0] = '\0';
    zzplay_host_pref_get(p->scheme, f->key, field_bufs[i], sizeof(field_bufs[i]));

    /* A CHOICE field becomes a cycle gadget; a module without a list
     * falls back to a plain text field. */
    field_choice_n[i] = 0;
    field_choice_sel[i] = 0;
    is_choice = (f->kind == ZZPLAY_FIELD_CHOICE &&
                 f->choice_labels != NULL && f->choice_labels[0] != NULL &&
                 f->choice_values != NULL && f->choice_values[0] != NULL);
    if (is_choice) {
      uint32_t c;
      int sel = 0;
      for (c = 0U; c < BROWSE_CHOICES_MAX &&
                   f->choice_labels[c] != NULL && f->choice_values[c] != NULL;
           c++) {
        field_choice_n[i] = (int)c + 1;
        if (strcmp(field_bufs[i], f->choice_values[c]) == 0) {
          sel = (int)c;
        }
      }
      field_choice_sel[i] = sel;

      cyc_tags[0].ti_Tag = GTCY_Labels;
      cyc_tags[0].ti_Data = (ULONG)f->choice_labels;
      cyc_tags[1].ti_Tag = GTCY_Active;
      cyc_tags[1].ti_Data = (ULONG)sel;
      cyc_tags[2].ti_Tag = TAG_DONE;

      field_gads[i] = gad = create_gadget(
          settings_win, CYCLE_KIND, gad, &ng,
          BROWSE_MARGIN + label_w, cur_y, val_w, btn_h,
          f->label, (UWORD)(SGID_FIELD_BASE + i), PLACETEXT_LEFT, cyc_tags);

      cur_y += btn_h + BROWSE_SPACING;
      continue;
    }

    if (f->kind == ZZPLAY_FIELD_SECRET) {
      if (field_bufs[i][0] != '\0') {
        strncpy(secret_orig[i], field_bufs[i], sizeof(secret_orig[i]) - 1U);
        secret_orig[i][sizeof(secret_orig[i]) - 1U] = '\0';
        strcpy(field_bufs[i], "********");
      }
    }

    str_tags[0].ti_Tag = GTST_String;
    str_tags[0].ti_Data = (ULONG)field_bufs[i];
    str_tags[1].ti_Tag = GTST_MaxChars;
    str_tags[1].ti_Data = (ULONG)(sizeof(field_bufs[i]) - 1U);
    str_tags[2].ti_Tag = GA_RelVerify;
    str_tags[2].ti_Data = TRUE;
    str_tags[3].ti_Tag = TAG_DONE;

    field_gads[i] = gad = create_gadget(
        settings_win, STRING_KIND, gad, &ng,
        BROWSE_MARGIN + label_w, cur_y, val_w, btn_h,
        f->label, (UWORD)(SGID_FIELD_BASE + i), PLACETEXT_LEFT, str_tags);

    cur_y += btn_h + BROWSE_SPACING;
  }

  gad = create_gadget(settings_win, BUTTON_KIND, gad, &ng,
                      BROWSE_MARGIN, cur_y, btn_w, btn_h,
                      "Save", SGID_SAVE, PLACETEXT_IN, NULL);

  gad = create_gadget(settings_win, BUTTON_KIND, gad, &ng,
                      win_w - BROWSE_MARGIN - btn_w, cur_y, btn_w, btn_h,
                      "Cancel", SGID_CANCEL, PLACETEXT_IN, NULL);

  if (!gad) {
    close_settings_window();
    return;
  }

  AddGList(settings_win, settings_glist, (UWORD)-1, (UWORD)-1, NULL);
  RefreshGList(settings_glist, settings_win, NULL, (UWORD)-1);
  GT_RefreshWindow(settings_win, NULL);
}

static void __attribute__((noinline)) poll_settings(void)
{
  struct IntuiMessage *imsg;
  const ZZPlayBrowseServiceEntry *svc;
  const ZZPlayProvider *p;

  if (!settings_win || !settings_win->UserPort) {
    return;
  }

  svc = zzplay_browse_model_current_service(&browse_model);
  p = svc ? svc->provider : NULL;

  while ((imsg = GT_GetIMsg(settings_win->UserPort)) != NULL) {
    ULONG class = imsg->Class;
    UWORD code = imsg->Code;
    struct Gadget *gad = (struct Gadget *)imsg->IAddress;

    GT_ReplyIMsg(imsg);

    if (class == IDCMP_CLOSEWINDOW) {
      close_settings_window();
      return;
    }
    if (class == IDCMP_REFRESHWINDOW) {
      GT_BeginRefresh(settings_win);
      GT_EndRefresh(settings_win, TRUE);
      continue;
    }
    if (class == IDCMP_GADGETUP && gad) {
      /* A cycle gadget reports the entry it now shows in Code */
      if (gad->GadgetID >= SGID_FIELD_BASE &&
          gad->GadgetID < (UWORD)(SGID_FIELD_BASE + BROWSE_FIELDS_MAX)) {
        uint32_t idx = (uint32_t)(gad->GadgetID - SGID_FIELD_BASE);
        if (field_choice_n[idx] > 0 && (int)code < field_choice_n[idx]) {
          field_choice_sel[idx] = (int)code;
        }
      }
      if (gad->GadgetID == SGID_CANCEL) {
        close_settings_window();
        return;
      }
      if (gad->GadgetID == SGID_SAVE && p) {
        uint32_t count = p->field_count > BROWSE_FIELDS_MAX ? BROWSE_FIELDS_MAX : p->field_count;
        uint32_t i;
        ZZPlayBrowseRequest req;

        for (i = 0U; i < count; i++) {
          const ZZPlayProviderField *f = &p->fields[i];
          const char *val = "";
          if (field_choice_n[i] > 0) {
            /* Save the value of the shown choice, not its label */
            val = f->choice_values[field_choice_sel[i]];
          } else {
            if (field_gads[i] && field_gads[i]->SpecialInfo) {
              val = (const char *)((struct StringInfo *)field_gads[i]->SpecialInfo)->Buffer;
            }
            if (f->kind == ZZPLAY_FIELD_SECRET) {
              if (strcmp(val, "********") == 0) {
                val = secret_orig[i];
              }
            }
          }
          zzplay_host_pref_set(p->scheme, f->key, val);
        }

        close_settings_window();

        /* Reopen service */
        if (browse_model.current_service >= 0) {
          zzplay_browse_model_select_service(&browse_model, (uint32_t)browse_model.current_service, &req);
          browse_send(&req, p);
          rebuild_list_nodes();
          update_status_text("Reloading service...");
          update_buttons();
        }
        return;
      }
    }
  }
}

/* Widest of the button labels plus their frame. */
static int browse_button_min_width(void)
{
  static const char *const labels[] = {
    "Back", "Reload", "Play", "Add", "Favourite", "Stop", "Settings"
  };
  int btn_w = 0;
  uint32_t i;

  for (i = 0U; i < sizeof(labels) / sizeof(labels[0]); i++) {
    int w = browse_text_w(labels[i]);
    if (w > btn_w) {
      btn_w = w;
    }
  }
  return btn_w + 16;
}

/* The service cycle gadget: the longest name plus its glyph. */
static int browse_cycle_width(void)
{
  int cycle_w = browse_text_w("(No services)");
  uint32_t i;

  for (i = 0U; i < ZZPLAY_BROWSE_SERVICES_MAX && service_labels[i] != NULL;
       i++) {
    int w = browse_text_w(service_labels[i]);
    if (w > cycle_w) {
      cycle_w = w;
    }
  }
  return cycle_w + 32;
}

/* Builds every Browse gadget for the window's current size. GadTools
 * gadgets cannot be resized, so a size change removes the attached
 * list, frees it and calls this again; the rows, the selection, the
 * shown top row and the status text live in this module's state and
 * are reapplied here. */
static int __attribute__((noinline)) build_browse_gadgets(void)
{
  struct NewGadget ng;
  struct Gadget *gad;
  struct TagItem tags[6];
  struct TextFont *font;
  int font_h;
  int btn_h;
  int win_w;
  int win_h;
  int content_w;
  int cur_y;
  int list_h;
  int btn_w;
  int cycle_w;
  int search_w;
  int search_label_w;

  if (!browse_win) {
    return 0;
  }

  font = gui_screen->RastPort.Font;
  font_h = (font && font->tf_YSize > 0) ? (int)font->tf_YSize : 8;
  btn_h = font_h + 6;
  win_w = (int)browse_win->Width - browse_win->BorderLeft -
          browse_win->BorderRight;
  win_h = (int)browse_win->Height - browse_win->BorderTop -
          browse_win->BorderBottom;
  content_w = win_w - 2 * BROWSE_MARGIN;
  if (content_w < 100) {
    content_w = 100;
  }

  cycle_w = browse_cycle_width();
  search_label_w = browse_text_w("Search:") + 8;
  search_w = content_w - cycle_w - BROWSE_SPACING - search_label_w;
  if (search_w < 40) {
    search_w = 40;
  }

  browse_glist = NULL;
  gad = CreateContext(&browse_glist);
  if (!gad) {
    return 0;
  }

  memset(&ng, 0, sizeof(ng));
  ng.ng_TextAttr = browse_text_attr;
  ng.ng_VisualInfo = browse_vi;

  cur_y = BROWSE_MARGIN;

  /* Service cycle + Search string; the search buffer keeps what was
   * typed across a rebuild. */
  tags[0].ti_Tag = GTCY_Labels;
  tags[0].ti_Data = (ULONG)service_labels;
  tags[1].ti_Tag = TAG_DONE;
  tags[1].ti_Data = 0;

  service_gad = gad = create_gadget(
      browse_win, CYCLE_KIND, gad, &ng,
      BROWSE_MARGIN, cur_y, cycle_w, btn_h,
      NULL, BGID_SERVICE, 0, tags);

  tags[0].ti_Tag = GTST_String;
  tags[0].ti_Data = (ULONG)search_buf;
  tags[1].ti_Tag = GTST_MaxChars;
  tags[1].ti_Data = (ULONG)(sizeof(search_buf) - 1U);
  tags[2].ti_Tag = GA_RelVerify;
  tags[2].ti_Data = TRUE;
  tags[3].ti_Tag = TAG_DONE;
  tags[3].ti_Data = 0;

  search_gad = gad = create_gadget(
      browse_win, STRING_KIND, gad, &ng,
      BROWSE_MARGIN + cycle_w + BROWSE_SPACING + search_label_w, cur_y,
      search_w, btn_h, "Search:", BGID_SEARCH, PLACETEXT_LEFT, tags);

  cur_y += btn_h + BROWSE_SPACING;

  /* The list takes all height between the search row and the button
   * row, and the full width. GadTools sizes it to whole rows. */
  list_h = win_h - cur_y - (2 * BROWSE_SPACING + 2 * btn_h + BROWSE_MARGIN);
  if (list_h < 2 * font_h + 4) {
    list_h = 2 * font_h + 4;
  }

  /* The contents, the selection and the shown top row are reapplied by
   * the tags; on V39 the render hook draws the column rows. */
  tags[0].ti_Tag = GTLV_Labels;
  tags[0].ti_Data = (ULONG)&browse_exec_list;
  tags[1].ti_Tag = GTLV_ShowSelected;
  tags[1].ti_Data = 0;
  if (browse_columns_ok) {
    tags[2].ti_Tag = GTLV_CallBack;
    tags[2].ti_Data = (ULONG)&browse_list_hook;
  } else {
    tags[2].ti_Tag = TAG_IGNORE;
    tags[2].ti_Data = 0;
  }
  tags[3].ti_Tag = GTLV_Selected;
  tags[3].ti_Data =
      (browse_model.item_count > 0U && browse_model.selection >= 0)
          ? (ULONG)browse_model.selection
          : ~0UL;
  tags[4].ti_Tag = GTLV_Top;
  tags[4].ti_Data = (ULONG)browse_list_top;
  tags[5].ti_Tag = TAG_DONE;
  tags[5].ti_Data = 0;

  list_gad = gad = create_gadget(
      browse_win, LISTVIEW_KIND, gad, &ng,
      BROWSE_MARGIN, cur_y, content_w, list_h,
      NULL, BGID_LIST, 0, tags);

  cur_y += list_h + BROWSE_SPACING;

  /* Buttons: Back, Reload, Play, Add, Favourite, Stop, Settings,
   * spread across the width. */
  btn_w = (content_w - 6 * BROWSE_SPACING) / 7;
  if (btn_w < 1) {
    btn_w = 1;
  }

  back_gad = gad = create_gadget(
      browse_win, BUTTON_KIND, gad, &ng,
      BROWSE_MARGIN, cur_y, btn_w, btn_h,
      "Back", BGID_BACK, PLACETEXT_IN, NULL);

  reload_gad = gad = create_gadget(
      browse_win, BUTTON_KIND, gad, &ng,
      BROWSE_MARGIN + (btn_w + BROWSE_SPACING), cur_y, btn_w, btn_h,
      "Reload", BGID_RELOAD, PLACETEXT_IN, NULL);

  play_gad = gad = create_gadget(
      browse_win, BUTTON_KIND, gad, &ng,
      BROWSE_MARGIN + 2 * (btn_w + BROWSE_SPACING), cur_y, btn_w, btn_h,
      "Play", BGID_PLAY, PLACETEXT_IN, NULL);

  add_gad = gad = create_gadget(
      browse_win, BUTTON_KIND, gad, &ng,
      BROWSE_MARGIN + 3 * (btn_w + BROWSE_SPACING), cur_y, btn_w, btn_h,
      "Add", BGID_ADD, PLACETEXT_IN, NULL);

  fav_gad = gad = create_gadget(
      browse_win, BUTTON_KIND, gad, &ng,
      BROWSE_MARGIN + 4 * (btn_w + BROWSE_SPACING), cur_y, btn_w, btn_h,
      "Favourite", BGID_FAVOURITE, PLACETEXT_IN, NULL);

  stop_gad = gad = create_gadget(
      browse_win, BUTTON_KIND, gad, &ng,
      BROWSE_MARGIN + 5 * (btn_w + BROWSE_SPACING), cur_y, btn_w, btn_h,
      "Stop", BGID_STOP, PLACETEXT_IN, NULL);

  settings_btn_gad = gad = create_gadget(
      browse_win, BUTTON_KIND, gad, &ng,
      BROWSE_MARGIN + 6 * (btn_w + BROWSE_SPACING), cur_y, btn_w, btn_h,
      "Settings", BGID_SETTINGS, PLACETEXT_IN, NULL);

  cur_y += btn_h + BROWSE_SPACING;

  /* Status line at the bottom */
  tags[0].ti_Tag = GTTX_Text;
  tags[0].ti_Data = (ULONG)status_buf;
  tags[1].ti_Tag = GTTX_CopyText;
  tags[1].ti_Data = TRUE;
  tags[2].ti_Tag = TAG_DONE;
  tags[2].ti_Data = 0;

  status_gad = gad = create_gadget(
      browse_win, TEXT_KIND, gad, &ng,
      BROWSE_MARGIN, cur_y, content_w, btn_h,
      NULL, BGID_STATUS, 0, tags);

  if (!gad || !service_gad || !search_gad || !list_gad || !back_gad ||
      !reload_gad || !play_gad || !add_gad || !fav_gad || !stop_gad ||
      !settings_btn_gad || !status_gad) {
    return 0;
  }

  AddGList(browse_win, browse_glist, (UWORD)-1, (UWORD)-1, NULL);
  RefreshGList(browse_glist, browse_win, NULL, (UWORD)-1);
  GT_RefreshWindow(browse_win, NULL);

  built_inner_w = win_w;
  built_inner_h = win_h;

  update_buttons();
  update_busy_pointer();
  /* The status gadget is new and empty; show the last text again. */
  status_shown[0] = '\0';
  update_status_text(status_buf);
  return 1;
}

/* A size change rebuilds the gadget list at the new size, keeping the
 * rows, the selection, the shown top row and the status text. */
static void browse_handle_resize(void)
{
  int w;
  int h;

  if (!browse_win || !browse_glist) {
    return;
  }
  w = (int)browse_win->Width - browse_win->BorderLeft -
      browse_win->BorderRight;
  h = (int)browse_win->Height - browse_win->BorderTop -
      browse_win->BorderBottom;
  if (w == built_inner_w && h == built_inner_h) {
    return;
  }

  /* Where the list is scrolled can only be read back through
   * GT_GetGadgetAttrs (V39); on V37 the last value this module set is
   * reused. */
  if (browse_columns_ok && list_gad) {
    struct TagItem get_tags[2];
    ULONG top = (ULONG)browse_list_top;

    get_tags[0].ti_Tag = GTLV_Top;
    get_tags[0].ti_Data = (ULONG)&top;
    get_tags[1].ti_Tag = TAG_DONE;
    get_tags[1].ti_Data = 0;
    if (GT_GetGadgetAttrsA(list_gad, browse_win, NULL, get_tags) != 0) {
      browse_list_top = (int)top;
    }
  }

  RemoveGList(browse_win, browse_glist, (UWORD)-1);
  FreeGadgets(browse_glist);
  browse_glist = NULL;
  list_gad = NULL;
  service_gad = NULL;
  search_gad = NULL;
  back_gad = NULL;
  reload_gad = NULL;
  play_gad = NULL;
  add_gad = NULL;
  fav_gad = NULL;
  stop_gad = NULL;
  settings_btn_gad = NULL;
  status_gad = NULL;
  (void)build_browse_gadgets();
}

int zzplay_browse_open(struct Screen *screen, ZZPlayController *ctl,
                       ZZPlayPlaylist *playlist, ZZPlayPrefs *prefs)
{
  ZZPlayProviders *providers;
  struct TextFont *font;
  int font_h;
  int btn_h;
  int min_w;
  int min_h;
  int min_outer_w;
  int min_outer_h;
  int border_l;
  int border_r;
  int border_t;
  int border_b;
  int left;
  int top;
  int w;
  int h;
  int cycle_w;
  int search_label_w;
  uint32_t i;
  uint32_t loaded_count;

  if (browse_win) {
    WindowToFront(browse_win);
    ActivateWindow(browse_win);
    return 1;
  }

  gui_screen = screen;
  gui_ctl = ctl;
  gui_playlist = playlist;
  browse_prefs = prefs;

  if (!gui_screen || !gui_ctl || !gui_playlist) {
    return 0;
  }

  browse_vi = GetVisualInfo(gui_screen, TAG_DONE);
  if (!browse_vi) {
    return 0;
  }

  browse_text_attr = gui_screen->Font;

  /* Load providers on first open */
  providers = zzplay_providers_shared();
  if (providers->count == 0U) {
    char why[ZZPLAY_PROVIDER_ERROR_MAX];
    why[0] = '\0';
    (void)zzplay_providers_load(providers, why, sizeof(why));
  }

  if (!model_inited) {
    zzplay_browse_model_init(&browse_model);
    model_inited = 1;
  }

  /* Refresh services in model */
  browse_model.service_count = 0U;
  loaded_count = providers->count;
  for (i = 0U; i < loaded_count; i++) {
    const ZZPlayProvider *p = providers->slot[i].provider;
    if (p) {
      zzplay_browse_model_add_service(&browse_model, p->scheme, p->name, p);
      (void)snprintf(service_labels_storage[i], sizeof(service_labels_storage[i]), "%s", p->name);
      service_labels[i] = service_labels_storage[i];
    }
  }
  if (loaded_count == 0U) {
    (void)snprintf(service_labels_storage[0], sizeof(service_labels_storage[0]), "(No services)");
    service_labels[0] = service_labels_storage[0];
    service_labels[1] = NULL;
  } else {
    service_labels[loaded_count] = NULL;
  }

  /* Start worker child process */
  (void)zzplay_browse_worker_start();

  font = gui_screen->RastPort.Font;
  font_h = (font && font->tf_YSize > 0) ? (int)font->tf_YSize : 8;
  browse_font = font;
  btn_h = font_h + 6;

  /* Aligned column rows need the V39 gadtools render hook; older
   * systems keep the window usable with the one-line rows. */
  browse_columns_ok =
      (GadToolsBase != NULL && GadToolsBase->lib_Version >= 39 &&
       IntuitionBase != NULL && IntuitionBase->LibNode.lib_Version >= 39)
          ? 1
          : 0;
  browse_marker_col_w = font_h + 4;
  browse_len_col_w = browse_text_w(ZZPLAY_BROWSE_LEN_TEXT);
  browse_list_top = 0;

  /* The smallest usable window: every button and the search row fit,
   * and the list shows two whole rows. */
  min_w = 7 * browse_button_min_width() + 6 * BROWSE_SPACING +
          2 * BROWSE_MARGIN;
  cycle_w = browse_cycle_width();
  search_label_w = browse_text_w("Search:") + 8;
  if (min_w < cycle_w + BROWSE_SPACING + search_label_w + 160 +
                  2 * BROWSE_MARGIN) {
    min_w = cycle_w + BROWSE_SPACING + search_label_w + 160 +
            2 * BROWSE_MARGIN;
  }
  if (min_w < 460) {
    min_w = 460;
  }
  min_h = BROWSE_MARGIN + btn_h + BROWSE_SPACING + (2 * font_h + 4) +
          BROWSE_SPACING + btn_h + BROWSE_SPACING + btn_h + BROWSE_MARGIN;

  /* Window borders, for the size limits and for clamping a remembered
   * geometry; the sizing border's height is close enough for a limit. */
  border_l = (int)gui_screen->WBorLeft;
  border_r = (int)gui_screen->WBorRight;
  border_t = (int)gui_screen->WBorTop + font_h + 1;
  border_b = 10;
  min_outer_w = min_w + border_l + border_r;
  min_outer_h = min_h + border_t + border_b;
  if (min_outer_w > (int)gui_screen->Width) {
    min_outer_w = (int)gui_screen->Width;
  }
  if (min_outer_h > (int)gui_screen->Height) {
    min_outer_h = (int)gui_screen->Height;
  }

  /* Today's defaults, or what the prefs remember of this window,
   * clamped so the whole window is on the screen it opens on. */
  left = 40;
  top = 40;
  w = min_w;
  h = BROWSE_MARGIN + btn_h + BROWSE_SPACING + (font_h * 8 + 8) +
      BROWSE_SPACING + btn_h + BROWSE_SPACING + btn_h + BROWSE_MARGIN;
  if (browse_prefs) {
    if (browse_prefs->browse_width >= 0 && browse_prefs->browse_height >= 0) {
      w = (int)browse_prefs->browse_width;
      h = (int)browse_prefs->browse_height;
    }
    if (browse_prefs->browse_left >= 0) {
      left = (int)browse_prefs->browse_left;
      top = (int)browse_prefs->browse_top;
    }
  }
  {
    int outer_w = w + border_l + border_r;
    int outer_h = h + border_t + border_b;
    zzplay_browse_clamp_window(&left, &top, &outer_w, &outer_h,
                               (int)gui_screen->Width,
                               (int)gui_screen->Height, min_outer_w,
                               min_outer_h);
    w = outer_w - border_l - border_r;
    h = outer_h - border_t - border_b;
  }

  /* A fresh window starts with an empty search field and status line;
   * rebuilds later keep whatever they hold. */
  search_buf[0] = '\0';
  status_buf[0] = '\0';
  status_shown[0] = '\0';

  init_exec_list(&browse_exec_list);
  browse_win = OpenWindowTags(
      NULL,
      WA_Title, (ULONG)"Browse Services",
      WA_Left, (ULONG)left,
      WA_Top, (ULONG)top,
      WA_InnerWidth, (ULONG)w,
      WA_InnerHeight, (ULONG)h,
      WA_DragBar, TRUE,
      WA_DepthGadget, TRUE,
      WA_CloseGadget, TRUE,
      WA_Activate, TRUE,
      WA_NewLookMenus, TRUE,
      /* Resizable: the list takes the extra height and width, the
       * buttons spread across the width, the status line stays at the
       * bottom, and the size can be remembered in the prefs. */
      WA_SizeGadget, TRUE,
      WA_SizeBBottom, TRUE,
      WA_MinWidth, (ULONG)min_outer_w,
      WA_MinHeight, (ULONG)min_outer_h,
      WA_MaxWidth, (ULONG)gui_screen->Width,
      WA_MaxHeight, (ULONG)gui_screen->Height,
      WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW |
                    IDCMP_NEWSIZE,
      WA_CustomScreen, (ULONG)gui_screen,
      TAG_DONE);

  if (!browse_win) {
    zzplay_browse_close();
    return 0;
  }

  if (!build_browse_gadgets()) {
    zzplay_browse_close();
    return 0;
  }

  /* Select initial service if available */
  if (browse_model.service_count > 0U && browse_model.current_service < 0) {
    ZZPlayBrowseRequest req;
    zzplay_browse_model_select_service(&browse_model, 0U, &req);
    browse_send(&req, browse_model.services[0].provider);
    update_status_text("Loading service...");
  }

  update_buttons();
  return 1;
}

void zzplay_browse_close(void)
{
  int remember = (browse_win != NULL && browse_prefs != NULL);

  close_settings_window();

  /* The window's place and size are remembered when it closes, and
   * again when ZZPlay quits with it open (this runs then too, before
   * the app writes the session). */
  if (remember) {
    browse_prefs->browse_left = (int16_t)browse_win->LeftEdge;
    browse_prefs->browse_top = (int16_t)browse_win->TopEdge;
    browse_prefs->browse_width =
        (int16_t)(browse_win->Width - browse_win->BorderLeft -
                  browse_win->BorderRight);
    browse_prefs->browse_height =
        (int16_t)(browse_win->Height - browse_win->BorderTop -
                  browse_win->BorderBottom);
  }

  if (browse_win) {
    if (browse_win->UserPort) {
      struct IntuiMessage *msg;
      while ((msg = GT_GetIMsg(browse_win->UserPort)) != NULL) {
        GT_ReplyIMsg(msg);
      }
    }
    CloseWindow(browse_win);
    browse_win = NULL;
  }

  if (browse_glist) {
    FreeGadgets(browse_glist);
    browse_glist = NULL;
  }
  list_gad = NULL;
  service_gad = NULL;
  search_gad = NULL;
  back_gad = NULL;
  reload_gad = NULL;
  play_gad = NULL;
  add_gad = NULL;
  fav_gad = NULL;
  stop_gad = NULL;
  settings_btn_gad = NULL;
  status_gad = NULL;
  built_inner_w = -1;
  built_inner_h = -1;
  if (browse_vi) {
    FreeVisualInfo(browse_vi);
    browse_vi = NULL;
  }

  free_exec_list_nodes(&browse_exec_list);
  zzplay_browse_worker_stop();

  if (model_inited) {
    zzplay_browse_model_free(&browse_model);
    model_inited = 0;
  }

  if (remember) {
    (void)zzplay_prefs_save_session(browse_prefs, ZZPLAY_PREFS_ENV_PATH);
    (void)zzplay_prefs_save_session(browse_prefs,
                                    ZZPLAY_PREFS_ENVARC_PATH);
  }
}

int zzplay_browse_is_open(void)
{
  return browse_win != NULL;
}

uint32_t zzplay_browse_signal_mask(void)
{
  uint32_t mask = 0U;

  if (browse_win && browse_win->UserPort) {
    mask |= (1UL << (ULONG)browse_win->UserPort->mp_SigBit);
  }
  if (settings_win && settings_win->UserPort) {
    mask |= (1UL << (ULONG)settings_win->UserPort->mp_SigBit);
  }
  mask |= zzplay_browse_worker_signal_mask();
  return mask;
}

/* Play or Add on a folder the module marks playable: collect its tracks
 * in the worker (up to the room the playlist offers) before anything is
 * replaced or appended. */
static void browse_start_gather(int add, const ZZPlayProvider *provider)
{
  uint32_t limit;

  if (!gui_playlist || !gui_ctl) {
    return;
  }
  limit = add ? ZZPLAY_PLAYLIST_MAX_ENTRIES - gui_playlist->count
              : ZZPLAY_PLAYLIST_MAX_ENTRIES;
  if (limit == 0U) {
    update_status_text("The playlist is full");
    return;
  }
  if (!zzplay_browse_model_gather(&browse_model, limit, &poll_req)) {
    return;
  }
  gather_add = add;
  gather_seen = 0U;
  gather_generation = poll_req.generation;
  gather_active = 1;
  copy_string(gather_scheme, sizeof(gather_scheme), poll_req.scheme);
  browse_send(&poll_req, provider);
  update_status_text("Adding tracks...");
  update_buttons();
  update_busy_pointer();
}

/* The gathered rows have all arrived: put them in the playlist (replacing
 * it for Play, appending for Add) and report what fit. Play starts the
 * first added track. */
static void browse_apply_gather(const ZZPlayBrowseReply *reply)
{
  uint32_t added = 0U;
  uint32_t i;
  int32_t first = -1;

  gather_active = 0;
  browse_model.busy = 0;

  if (!reply->success) {
    update_status_text(reply->error[0] ? reply->error
                                       : "Could not collect the tracks");
    update_buttons();
    update_busy_pointer();
    return;
  }

  if (!gather_add) {
    zzplay_playlist_clear(gui_playlist);
  }
  for (i = 0U; i < reply->item_count; i++) {
    const ZZPlayBrowseItem *item = &reply->items[i];
    int32_t idx;

    if (item->kind == ZZPLAY_ITEM_FOLDER || item->playable == 0U) {
      continue;
    }
    (void)snprintf(poll_uri, sizeof(poll_uri), "%s://%s",
                   gather_scheme, item->id);
    idx = zzplay_playlist_add(gui_playlist, poll_uri);
    if (idx < 0) {
      break;
    }
    zzplay_playlist_set_title(gui_playlist, (uint32_t)idx, item->title);
    if (first < 0) {
      first = idx;
    }
    added++;
  }
  gui_ctl->dirty |= ZZPLAY_DIRTY_PLAYLIST;

  if (added == reply->item_count && !reply->truncated) {
    (void)snprintf(status_buf, sizeof(status_buf), "Added %lu tracks",
                   (unsigned long)added);
  } else if (added < reply->item_count) {
    (void)snprintf(status_buf, sizeof(status_buf),
                   "Added %lu tracks; %lu did not fit",
                   (unsigned long)added,
                   (unsigned long)(reply->item_count - added));
  } else {
    (void)snprintf(status_buf, sizeof(status_buf),
                   "Added %lu tracks; more did not fit",
                   (unsigned long)added);
  }
  update_status_text(status_buf);

  if (!gather_add && first >= 0) {
    (void)zzplay_playlist_set_current(gui_playlist, first);
    (void)zzplay_controller_request(gui_ctl, ZZPLAY_REQUEST_PLAY);
  }
  update_buttons();
  update_busy_pointer();
}

void zzplay_browse_poll(void)
{
  struct IntuiMessage *imsg;
  const ZZPlayBrowseServiceEntry *svc;
  const ZZPlayProvider *p;

  poll_settings();
  svc = zzplay_browse_model_current_service(&browse_model);
  p = svc ? svc->provider : NULL;


  /* 1. Worker replies (rows arrive in groups; drain what queued) */
  while (zzplay_browse_worker_take_reply(&poll_worker_reply)) {
    if (poll_worker_reply.kind == ZZPLAY_BROWSE_REQ_GATHER) {
      if (gather_active &&
          poll_worker_reply.generation == gather_generation) {
        if (poll_worker_reply.is_partial) {
          gather_seen += poll_worker_reply.item_count;
        } else {
          browse_apply_gather(&poll_worker_reply);
        }
      }
      /* Replies of a superseded or stopped gather are discarded. */
      zzplay_browse_worker_release_reply(&poll_worker_reply);
    } else if (poll_worker_reply.kind == ZZPLAY_BROWSE_REQ_FAVOURITE) {
      if (!poll_worker_reply.success) {
        update_status_text(poll_worker_reply.error[0] ? poll_worker_reply.error
                                                      : "Failed to update favourite");
        browse_model.busy = 0;
        update_buttons();
      } else {
        reload_note = poll_worker_reply.fav_add ? "Added to favourites"
                                                : "Removed from favourites";
        update_status_text(reload_note);
        /* Reload the list (the Favourites folder changes), unless the user
         * has already asked for something else. */
        if (!have_pending) {
          browse_model.saved_selection = browse_model.selection;
          browse_model.generation++;
          browse_model.busy = 1;
          if (browse_model.is_search) {
            poll_req.kind = ZZPLAY_BROWSE_REQ_SEARCH;
            poll_req.generation = browse_model.generation;
            copy_string(poll_req.scheme, sizeof(poll_req.scheme), svc ? svc->scheme : "");
            copy_string(poll_req.query, sizeof(poll_req.query), browse_model.search_query);
          } else {
            poll_req.kind = ZZPLAY_BROWSE_REQ_LIST;
            poll_req.generation = browse_model.generation;
            copy_string(poll_req.scheme, sizeof(poll_req.scheme), svc ? svc->scheme : "");
            copy_string(poll_req.node_id, sizeof(poll_req.node_id), browse_model.current_node);
            poll_req.start = 0U;
            poll_req.count = ZZPLAY_BROWSE_PAGE_SIZE;
          }
          browse_send(&poll_req, p);
        }
        update_buttons();
      }
      zzplay_browse_worker_release_reply(&poll_worker_reply);
    } else {
      int ok = zzplay_browse_model_apply_reply(&browse_model, &poll_worker_reply);
      int partial = poll_worker_reply.is_partial;
      zzplay_browse_worker_release_reply(&poll_worker_reply);

      if (ok) {
        rebuild_list_nodes();
        if (partial) {
          (void)update_stage_status();
        } else if (browse_model.status_message[0]) {
          update_status_text(browse_model.status_message);
        } else if (reload_note) {
          update_status_text(reload_note);
          reload_note = NULL;
        } else if (browse_model.item_count > 0U) {
          if (browse_model.has_total &&
              browse_model.total > browse_model.item_count) {
            (void)snprintf(status_buf, sizeof(status_buf),
                           "%lu of %lu items",
                           (unsigned long)browse_model.item_count,
                           (unsigned long)browse_model.total);
          } else {
            (void)snprintf(status_buf, sizeof(status_buf), "%lu items",
                           (unsigned long)browse_model.item_count);
          }
          update_status_text(status_buf);
        } else {
          update_status_text("");
        }
        if (!partial) {
          reload_note = NULL;
        }
        update_buttons();
      }
    }
    if (have_pending && !zzplay_browse_worker_active()) {
      have_pending = 0;
      (void)zzplay_browse_worker_send(&pending_req, pending_provider);
    }
  }

  /* Stage text while a request runs and no rows have arrived yet */
  (void)update_stage_status();
  update_busy_pointer();


  if (!browse_win || !browse_win->UserPort) {
    return;
  }


  /* 2. Window IDCMP */
  while ((imsg = GT_GetIMsg(browse_win->UserPort)) != NULL) {
    ULONG class = imsg->Class;
    UWORD code = imsg->Code;
    struct Gadget *gad = (struct Gadget *)imsg->IAddress;
    ULONG imsg_s = imsg->Seconds;
    ULONG imsg_m = imsg->Micros;

    GT_ReplyIMsg(imsg);

    if (class == IDCMP_CLOSEWINDOW) {
      zzplay_browse_close();
      return;
    }
    if (class == IDCMP_REFRESHWINDOW) {
      GT_BeginRefresh(browse_win);
      GT_EndRefresh(browse_win, TRUE);
      continue;
    }
    if (class == IDCMP_NEWSIZE) {
      browse_handle_resize();
      continue;
    }
    if (class == IDCMP_GADGETUP && gad) {
      UWORD gid = gad->GadgetID;

      if (gid == BGID_SERVICE) {
        if ((uint32_t)code < browse_model.service_count) {
          if (zzplay_browse_model_select_service(&browse_model, (uint32_t)code,
                                                 &poll_req)) {
            svc = zzplay_browse_model_current_service(&browse_model);
            p = svc ? svc->provider : NULL;
            if (poll_req.kind == ZZPLAY_BROWSE_REQ_NONE) {
              /* Remembered from earlier in this session */
              rebuild_list_nodes();
              update_status_text("");
            } else {
              browse_send(&poll_req, p);
              rebuild_list_nodes();
              update_status_text("Loading service...");
            }
            update_buttons();
            update_busy_pointer();
          }
        }
      } else if (gid == BGID_SEARCH) {
        const char *query = "";
        if (search_gad && search_gad->SpecialInfo) {
          query = (const char *)((struct StringInfo *)search_gad->SpecialInfo)->Buffer;
        }
        if (query && query[0] != '\0') {
          if (zzplay_browse_model_search(&browse_model, query, &poll_req)) {
            if (poll_req.kind == ZZPLAY_BROWSE_REQ_NONE) {
              /* This search is remembered from earlier */
              rebuild_list_nodes();
              update_status_text("");
            } else {
              browse_send(&poll_req, p);
              rebuild_list_nodes();
              update_status_text("Searching...");
            }
            update_buttons();
            update_busy_pointer();
          }
        }
      } else if (gid == BGID_LIST) {
        int32_t clicked = (int32_t)code;

        if (clicked == (int32_t)browse_model.item_count && browse_model.has_more) {
          /* Clicked More row */
          if (zzplay_browse_model_request_more(&browse_model, &poll_req)) {
            browse_send(&poll_req, p);
            update_status_text("Loading more items...");
            update_buttons();
          }
        } else if ((clicked >= 0 &&
                    (uint32_t)clicked < browse_model.item_count &&
                    browse_model.items[clicked].kind == ZZPLAY_ITEM_FOLDER) ||
                   (clicked == last_clicked_idx &&
                    DoubleClick(last_click_s, last_click_m, imsg_s, imsg_m))) {
          /* A folder opens on one click, like a drawer in the ASL file
           * requester; a track or station plays on a double-click. */
          ZZPlayBrowseAction act = zzplay_browse_model_activate(&browse_model, clicked, &poll_req);
          /* A third click starts a new pair, as in the playlist. */
          last_clicked_idx = -1;

          if (act == ZZPLAY_BROWSE_ACTION_ENTER_FOLDER) {
            if (poll_req.kind == ZZPLAY_BROWSE_REQ_NONE) {
              /* Folder remembered from earlier in this session */
              rebuild_list_nodes();
              update_status_text("");
            } else {
              browse_send(&poll_req, p);
              rebuild_list_nodes();
              update_status_text("Opening folder...");
            }
            update_buttons();
            update_busy_pointer();
          } else if (act == ZZPLAY_BROWSE_ACTION_PLAY) {
            if (zzplay_browse_model_format_uri(&browse_model, clicked, poll_uri, sizeof(poll_uri),
                                               poll_title, sizeof(poll_title))) {
              int32_t pl_idx = zzplay_playlist_add(gui_playlist, poll_uri);
              if (pl_idx >= 0) {
                zzplay_playlist_set_title(gui_playlist, (uint32_t)pl_idx, poll_title);
                (void)zzplay_playlist_set_current(gui_playlist, pl_idx);
                gui_ctl->dirty |= ZZPLAY_DIRTY_PLAYLIST;
                (void)zzplay_controller_request(gui_ctl, ZZPLAY_REQUEST_PLAY);
              }
            }
          }
        } else {
          last_clicked_idx = clicked;
          last_click_s = imsg_s;
          last_click_m = imsg_m;
          zzplay_browse_model_set_selection(&browse_model, clicked);
          update_buttons();
        }
      } else if (gid == BGID_BACK) {
        if (zzplay_browse_model_back(&browse_model, &poll_req)) {
          rebuild_list_nodes();
          update_status_text("");
          update_buttons();
        }
      } else if (gid == BGID_RELOAD) {
        if (zzplay_browse_model_reload(&browse_model, &poll_req)) {
          browse_send(&poll_req, p);
          update_status_text("Reloading...");
          update_buttons();
        }
      } else if (gid == BGID_PLAY) {
        ZZPlayBrowseTarget target = zzplay_browse_model_play_target(&browse_model);
        if (target == ZZPLAY_BROWSE_TARGET_ITEM) {
          if (zzplay_browse_model_format_uri(&browse_model, browse_model.selection,
                                             poll_uri, sizeof(poll_uri), poll_title, sizeof(poll_title))) {
            int32_t pl_idx = zzplay_playlist_add(gui_playlist, poll_uri);
            if (pl_idx >= 0) {
              zzplay_playlist_set_title(gui_playlist, (uint32_t)pl_idx, poll_title);
              (void)zzplay_playlist_set_current(gui_playlist, pl_idx);
              gui_ctl->dirty |= ZZPLAY_DIRTY_PLAYLIST;
              (void)zzplay_controller_request(gui_ctl, ZZPLAY_REQUEST_PLAY);
            }
          }
        } else if (target == ZZPLAY_BROWSE_TARGET_COLLECTION) {
          browse_start_gather(0, p);
        }
      } else if (gid == BGID_ADD) {
        ZZPlayBrowseTarget target = zzplay_browse_model_play_target(&browse_model);
        if (target == ZZPLAY_BROWSE_TARGET_ITEM) {
          if (zzplay_browse_model_format_uri(&browse_model, browse_model.selection,
                                             poll_uri, sizeof(poll_uri), poll_title, sizeof(poll_title))) {
            int32_t pl_idx = zzplay_playlist_add(gui_playlist, poll_uri);
            if (pl_idx >= 0) {
              zzplay_playlist_set_title(gui_playlist, (uint32_t)pl_idx, poll_title);
              gui_ctl->dirty |= ZZPLAY_DIRTY_PLAYLIST;
            }
          }
        } else if (target == ZZPLAY_BROWSE_TARGET_COLLECTION) {
          browse_start_gather(1, p);
        }
      } else if (gid == BGID_FAVOURITE) {
        if (zzplay_browse_model_can_favourite(&browse_model)) {
          if (zzplay_browse_model_favourite(&browse_model, &poll_req)) {
            last_fav_add = poll_req.fav_add;
            browse_send(&poll_req, p);
            update_status_text(last_fav_add ? "Adding to favourites..."
                                            : "Removing from favourites...");
            update_buttons();
          }
        }
      } else if (gid == BGID_STOP) {
        have_pending = 0;
        gather_active = 0;
        zzplay_browse_worker_cancel();
        zzplay_browse_model_stop(&browse_model);
        update_status_text("Stopped");
        update_buttons();
        update_busy_pointer();
      } else if (gid == BGID_SETTINGS) {
        open_settings_window();
      }
    }
  }
}

#else /* !defined(__amigaos__) && !defined(AMIGA) */

int zzplay_browse_open(struct Screen *screen, ZZPlayController *ctl,
                       ZZPlayPlaylist *playlist, ZZPlayPrefs *prefs)
{
  (void)screen; (void)ctl; (void)playlist; (void)prefs;
  return 0;
}
void zzplay_browse_close(void) {}
int zzplay_browse_is_open(void) { return 0; }
uint32_t zzplay_browse_signal_mask(void) { return 0U; }
void zzplay_browse_poll(void) {}

#endif
