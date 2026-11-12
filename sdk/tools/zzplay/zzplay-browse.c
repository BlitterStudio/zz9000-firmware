/* Service browser window for ZZPlay (GadTools, AmigaOS 3.0+).
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-browse.h"

#include <stdio.h>
#include <string.h>

#include "zzplay-browse-model.h"
#include "zzplay-browse-worker.h"
#include "zzplay-host.h"
#include "zzplay-http.h"
#include "zzplay-net.h"
#include "zzplay-providers.h"
#include "zzplay-sys.h"

#if defined(__amigaos__) || defined(AMIGA)

#include <graphics/gfxbase.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#define BROWSE_MARGIN 6
#define BROWSE_SPACING 4
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

static char *gui_strdup(const char *s)
{
  size_t len;
  char *copy;

  if (!s) {
    return NULL;
  }
  len = strlen(s);
  copy = (char *)zzplay_alloc(len + 1U);
  if (copy) {
    memcpy(copy, s, len + 1U);
  }
  return copy;
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
  while ((node = RemHead(list)) != 0) {
    if (node->ln_Name) {
      zzplay_free(node->ln_Name);
    }
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

/* While a request runs, name what it waits for; once rows arrive, count
 * them. Returns 1 when stage text was drawn. */
static int update_stage_status(void)
{
  int stage;

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
  static char row_text[256];

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
    struct Node *node = (struct Node *)zzplay_alloc(sizeof(struct Node));

    if (!node) {
      break;
    }
    memset(node, 0, sizeof(*node));
    if (it->kind == ZZPLAY_ITEM_FOLDER) {
      (void)snprintf(row_text, sizeof(row_text), "[+] %s", it->title);
    } else if (it->subtitle[0] != '\0') {
      (void)snprintf(row_text, sizeof(row_text), "%s - %s", it->title, it->subtitle);
    } else {
      (void)snprintf(row_text, sizeof(row_text), "%s", it->title);
    }
    node->ln_Name = gui_strdup(row_text);
    if (!node->ln_Name) {
      zzplay_free(node);
      break;
    }
    AddTail(&browse_exec_list, node);
  }

  /* Synthetic More row */
  if (browse_model.has_more) {
    struct Node *node = (struct Node *)zzplay_alloc(sizeof(struct Node));
    if (node) {
      memset(node, 0, sizeof(*node));
      node->ln_Name = gui_strdup("--- More items ---");
      if (node->ln_Name) {
        AddTail(&browse_exec_list, node);
      } else {
        zzplay_free(node);
      }
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
  tags[2].ti_Tag = fresh ? GTLV_Top : TAG_IGNORE;
  tags[2].ti_Data = browse_model.selection >= 0
                        ? (ULONG)browse_model.selection : 0UL;
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

int zzplay_browse_open(struct Screen *screen, ZZPlayController *ctl,
                       ZZPlayPlaylist *playlist)
{
  ZZPlayProviders *providers;
  struct NewGadget ng;
  struct Gadget *gad;
  struct TagItem tags[4];
  int font_h;
  int btn_h;
  int win_w;
  int win_h;
  int cur_y;
  int content_w;
  int btn_w;
  int cycle_w;
  int search_w;
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

  {
    struct TextFont *font = gui_screen->RastPort.Font;
    font_h = (font && font->tf_YSize > 0) ? (int)font->tf_YSize : 8;
  }
  btn_h = font_h + 6;
  /* Six buttons as wide as the widest label; the cycle gadget as wide as
   * the longest service name plus its glyph. */
  btn_w = 0;
  {
    static const char *const labels[] = {
      "Back", "Reload", "Play", "Add", "Favourite", "Stop", "Settings"
    };
    for (i = 0U; i < sizeof(labels) / sizeof(labels[0]); i++) {
      int w = browse_text_w(labels[i]);
      if (w > btn_w) {
        btn_w = w;
      }
    }
  }
  btn_w += 16;
  cycle_w = browse_text_w("(No services)");
  for (i = 0U; i < loaded_count; i++) {
    int w = browse_text_w(service_labels[i]);
    if (w > cycle_w) {
      cycle_w = w;
    }
  }
  cycle_w += 32;
  search_label_w = browse_text_w("Search:") + 8;
  win_w = 7 * btn_w + 6 * BROWSE_SPACING + 2 * BROWSE_MARGIN;
  if (win_w < cycle_w + BROWSE_SPACING + search_label_w + 160 +
                  2 * BROWSE_MARGIN) {
    win_w = cycle_w + BROWSE_SPACING + search_label_w + 160 +
            2 * BROWSE_MARGIN;
  }
  if (win_w < 460) {
    win_w = 460;
  }
  if (win_w > gui_screen->Width - gui_screen->WBorLeft - gui_screen->WBorRight) {
    win_w = gui_screen->Width - gui_screen->WBorLeft - gui_screen->WBorRight;
  }
  content_w = win_w - 2 * BROWSE_MARGIN;
  btn_w = (content_w - 6 * BROWSE_SPACING) / 7;
  /* Service row, list, button row, status line. */
  win_h = BROWSE_MARGIN + btn_h + BROWSE_SPACING + (font_h * 8 + 8) +
          BROWSE_SPACING + btn_h + BROWSE_SPACING + btn_h + BROWSE_MARGIN;

  init_exec_list(&browse_exec_list);

  browse_win = OpenWindowTags(
      NULL,
      WA_Title, (ULONG)"Browse Services",
      WA_Left, 40,
      WA_Top, 40,
      WA_InnerWidth, (ULONG)win_w,
      WA_InnerHeight, (ULONG)win_h,
      WA_DragBar, TRUE,
      WA_DepthGadget, TRUE,
      WA_CloseGadget, TRUE,
      WA_Activate, TRUE,
      WA_NewLookMenus, TRUE,
      WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW,
      WA_CustomScreen, (ULONG)gui_screen,
      TAG_DONE);

  if (!browse_win) {
    zzplay_browse_close();
    return 0;
  }

  browse_glist = NULL;
  gad = CreateContext(&browse_glist);
  if (!gad) {
    zzplay_browse_close();
    return 0;
  }

  ng.ng_TextAttr = browse_text_attr;
  ng.ng_VisualInfo = browse_vi;

  cur_y = BROWSE_MARGIN;

  /* Service cycle + Search string (cycle_w measured above) */
  tags[0].ti_Tag = GTCY_Labels;
  tags[0].ti_Data = (ULONG)service_labels;
  tags[1].ti_Tag = TAG_DONE;

  service_gad = gad = create_gadget(
      browse_win, CYCLE_KIND, gad, &ng,
      BROWSE_MARGIN, cur_y, cycle_w, btn_h,
      NULL, BGID_SERVICE, 0, tags);

  search_w = content_w - cycle_w - BROWSE_SPACING - search_label_w;
  search_buf[0] = '\0';
  tags[0].ti_Tag = GTST_String;
  tags[0].ti_Data = (ULONG)search_buf;
  tags[1].ti_Tag = GTST_MaxChars;
  tags[1].ti_Data = (ULONG)(sizeof(search_buf) - 1U);
  tags[2].ti_Tag = GA_RelVerify;
  tags[2].ti_Data = TRUE;
  tags[3].ti_Tag = TAG_DONE;

  search_gad = gad = create_gadget(
      browse_win, STRING_KIND, gad, &ng,
      BROWSE_MARGIN + cycle_w + BROWSE_SPACING + search_label_w, cur_y,
      search_w, btn_h, "Search:", BGID_SEARCH, PLACETEXT_LEFT, tags);

  cur_y += btn_h + BROWSE_SPACING;

  /* List view */
  tags[0].ti_Tag = GTLV_Labels;
  tags[0].ti_Data = (ULONG)&browse_exec_list;
  tags[1].ti_Tag = GTLV_ShowSelected;
  tags[1].ti_Data = 0;
  tags[2].ti_Tag = TAG_DONE;

  list_gad = gad = create_gadget(
      browse_win, LISTVIEW_KIND, gad, &ng,
      BROWSE_MARGIN, cur_y, content_w, font_h * 8 + 8,
      NULL, BGID_LIST, 0, tags);

  cur_y += font_h * 8 + 8 + BROWSE_SPACING;

  /* Buttons: Back, Reload, Play, Add, Favourite, Stop, Settings */

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

  /* Status text */
  status_buf[0] = '\0';
  tags[0].ti_Tag = GTTX_Text;
  tags[0].ti_Data = (ULONG)status_buf;
  tags[1].ti_Tag = GTTX_CopyText;
  tags[1].ti_Data = TRUE;
  tags[2].ti_Tag = TAG_DONE;

  status_gad = gad = create_gadget(
      browse_win, TEXT_KIND, gad, &ng,
      BROWSE_MARGIN, cur_y, content_w, btn_h,
      NULL, BGID_STATUS, 0, tags);

  if (!gad) {
    zzplay_browse_close();
    return 0;
  }

  AddGList(browse_win, browse_glist, (UWORD)-1, (UWORD)-1, NULL);
  RefreshGList(browse_glist, browse_win, NULL, (UWORD)-1);
  GT_RefreshWindow(browse_win, NULL);

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
  close_settings_window();

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
    if (poll_worker_reply.kind == ZZPLAY_BROWSE_REQ_FAVOURITE) {
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
          (void)snprintf(status_buf, sizeof(status_buf), "%lu items",
                         (unsigned long)browse_model.item_count);
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
        if (zzplay_browse_model_can_play(&browse_model)) {
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
        }
      } else if (gid == BGID_ADD) {
        if (zzplay_browse_model_can_add(&browse_model)) {
          if (zzplay_browse_model_format_uri(&browse_model, browse_model.selection,
                                             poll_uri, sizeof(poll_uri), poll_title, sizeof(poll_title))) {
            int32_t pl_idx = zzplay_playlist_add(gui_playlist, poll_uri);
            if (pl_idx >= 0) {
              zzplay_playlist_set_title(gui_playlist, (uint32_t)pl_idx, poll_title);
              gui_ctl->dirty |= ZZPLAY_DIRTY_PLAYLIST;
            }
          }
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
                       ZZPlayPlaylist *playlist)
{
  (void)screen; (void)ctl; (void)playlist;
  return 0;
}
void zzplay_browse_close(void) {}
int zzplay_browse_is_open(void) { return 0; }
uint32_t zzplay_browse_signal_mask(void) { return 0U; }
void zzplay_browse_poll(void) {}

#endif
