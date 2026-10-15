/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-gui.h"

#include <dos/dos.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <graphics/gfxbase.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/wb.h>

#include <stdio.h>
#include <string.h>
#include "zzplay-sys.h"

#include "zzplay-controls.h"
#include "zzplay-devices.h"
#include "zzplay-files.h"
#include "zzplay-requester.h"

/* Program-wide library base ownership per plan. */
struct GfxBase *GfxBase;
struct Library *GadToolsBase;
struct Library *WorkbenchBase;

extern struct IntuitionBase *IntuitionBase;

static int owns_gfx;
static int owns_gadtools;
static int owns_workbench;

/* The context is copied at open: callers build it on their stack, and a
 * borrowed pointer would read dead stack on every later poll (the
 * controller/playlist pointers turn into garbage). The structs it points
 * to must still outlive the GUI. */
static ZZPlayGuiContext gui_context_copy;
static const ZZPlayGuiContext *gui_context;

/* --- Layout metrics and constants --- */
#define ZZPLAY_GUI_MARGIN 6
#define ZZPLAY_GUI_SPACING 4
#define ZZPLAY_GUI_MIN_WIDTH 420
#define ZZPLAY_GUI_LINES 5
/* GadTools' fixed checkbox imagery (gadtools.h CHECKBOXWIDTH/HEIGHT in
 * newer NDKs); the gadget is always drawn at this size. */
#ifndef CHECKBOXWIDTH
#define CHECKBOXWIDTH 26
#endif
#ifndef CHECKBOXHEIGHT
#define CHECKBOXHEIGHT 11
#endif

/* Gadget IDs for Player window */
enum {
  GID_POS_SLIDER = 1,
  GID_PREV,
  GID_PLAY,
  GID_PAUSE,
  GID_STOP,
  GID_NEXT,
  GID_VOL_SLIDER,
  GID_REPEAT_CYCLE,
  GID_SHUFFLE_CHECK,
  GID_PLAYLIST_LIST,
  GID_PL_ADD,
  GID_PL_REMOVE,
  GID_PL_CLEAR,
  GID_PL_LOAD,
  GID_PL_SAVE
};

/* Menu IDs */
enum {
  MID_PROJECT = 1,
  MID_OPEN,
  MID_ADD_FILES,
  MID_ADD_DRAWER,
  MID_LOAD_PLAYLIST,
  MID_SAVE_PLAYLIST,
  MID_SETTINGS,
  MID_ABOUT,
  MID_QUIT,

  MID_CONTROL,
  MID_PLAY_PAUSE,
  MID_STOP,
  MID_NEXT,
  MID_PREVIOUS,
  MID_SEEK_FWD,
  MID_SEEK_BACK,
  MID_REPEAT_OFF,
  MID_REPEAT_TRACK,
  MID_REPEAT_ALL,
  MID_SHUFFLE,
  MID_FULLSCREEN
};

/* Gadget IDs for Settings window */
enum {
  SGID_MP3_OUTPUT = 101,
  SGID_VIDEO_SOUND,
  SGID_PRELOAD,
  SGID_AHI_UNIT,
  SGID_MHI_LIST,
  SGID_NOTE_TEXT,
  SGID_SAVE,
  SGID_USE,
  SGID_CANCEL
};

/* --- Player Window State --- */
static struct Screen *player_screen;
static struct DrawInfo *player_draw_info;
static APTR player_vi;
static struct Window *player_win;
static struct Gadget *player_gadgets;
static struct Menu *player_menus;

static struct Gadget *pos_slider_gad;
static struct Gadget *vol_slider_gad;
static struct Gadget *repeat_gad;
static struct Gadget *shuffle_gad;
static struct Gadget *pl_listview_gad;

static int font_height = 8;
static int font_baseline = 6;
static int line_height = 10;
static int inner_width = ZZPLAY_GUI_MIN_WIDTH;
static int info_top = ZZPLAY_GUI_MARGIN;
static int info_height = 54;
/* GadTools gadget coordinates are relative to the window's outer corner,
 * not its inner area, so every gadget and every piece of drawn text is
 * offset by the borders the window will get: left border and title bar
 * (WBorTop + screen font height + 1, the standard Intuition title bar). */
static int win_off_x;
static int win_off_y;
static struct TextAttr *gui_text_attr;

static struct List playlist_exec_list;
static int32_t playlist_selected_row = -1;
/* Files dropped on the window wait here for the modal gate: expanding a
 * drawer or a long playlist blocks this task, which also feeds playback, so
 * an active item is paused first. Paths are copied because the AppMessage
 * (and its locks) is replied at once. */
static char **drop_paths;
static uint32_t drop_count;
static uint32_t drop_capacity;
static char drop_path_buf[ZZPLAY_PLAYLIST_PATH_MAX];


static char *gui_strdup(const char *text)
{
  char *copy;
  size_t length;

  if (!text) {
    return 0;
  }
  length = strlen(text) + 1U;
  copy = (char *)zzplay_alloc(length);
  if (copy) {
    memcpy(copy, text, length);
  }
  return copy;
}
static void drop_queue_push(const char *path)
{
  char *copy;

  if (drop_count == drop_capacity) {
    uint32_t capacity = drop_capacity ? drop_capacity * 2U : 8U;
    char **grown = (char **)zzplay_alloc(capacity * sizeof(char *));
    if (grown && drop_paths) {
      memcpy(grown, drop_paths, drop_count * sizeof(char *));
    }

    if (!grown) {
      return;
    }
    zzplay_free(drop_paths);
    drop_paths = grown;
    drop_capacity = capacity;
  }
  copy = gui_strdup(path);
  if (copy) {
    drop_paths[drop_count++] = copy;
  }
}

static void drop_queue_free(void)
{
  uint32_t i;

  for (i = 0U; i < drop_count; i++) {
    zzplay_free(drop_paths[i]);
  }
  zzplay_free(drop_paths);
  drop_paths = 0;
  drop_count = 0U;
  drop_capacity = 0U;
}

/* The rendered marker lets item changes touch only two ListView labels. */
static int32_t playlist_rendered_current = -1;
static ULONG last_click_s = 0UL;
static ULONG last_click_m = 0UL;
static int32_t last_click_row = -1;
static int pos_slider_dragging = 0;
/* -1 means the position gadget was (re)created and has no applied state. */
static int pos_slider_last_disabled = -1;
static ULONG pos_slider_last_level;
/* The time row as last drawn: the position changes on every pass but its
 * text once a second, and only a change is worth drawing. */
static char shown_position[128];
/* An IDCMP_REFRESHWINDOW that waits for the screen's layers to be free. */
static int refresh_pending = 0;
static int settings_refresh_pending = 0;

/* AppWindow */
static struct MsgPort *app_port;
static struct AppWindow *app_window;

/* Cycle labels */
static const char *repeat_cycle_labels[] = {
  "Off",
  "Track",
  "All",
  0
};

/* --- Settings Window State --- */
static struct Window *settings_win;
static APTR settings_vi;
static struct Gadget *settings_gadgets;
static ZZPlayPrefs settings_copy;

static char ahi_unit_labels[ZZPLAY_PREFS_AHI_UNITS][ZZPLAY_DEVICES_NAME_MAX];
static const char *ahi_cycle_ptrs[ZZPLAY_PREFS_AHI_UNITS + 1];

static char mhi_clean_names[ZZPLAY_DEVICES_MAX_MHI + 1][ZZPLAY_PREFS_DRIVER_MAX];
static struct List mhi_exec_list;
static uint32_t mhi_count = 0U;

static const char *mp3_backend_labels[] = {
  "Auto",
  "MHI",
  "AHI",
  0
};

static const char *video_backend_labels[] = {
  "Auto",
  "AHI",
  "ZZ9000AX",
  "None",
  0
};


static int gui_text_width(struct RastPort *rp, const char *text);
/* Tag lists are passed as real arrays. Taking the address of the first
 * variadic argument (the old `Tag tag1, ...` + `&tag1` idiom) is not a tag
 * list once GCC inlines the helper: only the first tag was stored and
 * GadTools read the rest from unrelated locals, so sliders kept their
 * default 0..15 range and the position bar hit the end within seconds. */
#define GUI_TAGS(...) ((const struct TagItem *)(const ULONG[]){ __VA_ARGS__ })
static struct Gadget *gui_gadget(ULONG kind, struct Gadget *prev,
                                 struct NewGadget *ng, int x, int y,
                                 int w, int h, const char *label,
                                 UWORD id, ULONG flags,
                                 const struct TagItem *tags);

/* --- Helpers for List management --- */
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
static void rebuild_playlist_nodes(void)
{
  uint32_t i;
  ZZPlayPlaylist *pl;

  if (!gui_context || !gui_context->playlist) {
    return;
  }
  pl = gui_context->playlist;

  if (pl_listview_gad && player_win) {
    GT_SetGadgetAttrs(pl_listview_gad, player_win, 0,
                      GTLV_Labels, (ULONG)~0UL,
                      TAG_DONE);
  }

  free_exec_list_nodes(&playlist_exec_list);
  init_exec_list(&playlist_exec_list);

  for (i = 0U; i < pl->count; i++) {
    const char *disp = zzplay_playlist_display_name(pl, i);
    size_t len = strlen(disp);
    char *name_buf = (char *)zzplay_alloc(len + 8U);
    struct Node *node = (struct Node *)zzplay_alloc(sizeof(struct Node));

    if (node && name_buf) {
      memset(node, 0, sizeof(*node));
      if ((int32_t)i == pl->current) {
        sprintf(name_buf, "\xBB %s", disp);
      } else {
        sprintf(name_buf, "   %s", disp);
      }
      node->ln_Name = name_buf;
      AddTail(&playlist_exec_list, node);
    } else {
      /* Row N must stay entry N for the click and Remove handlers, so
       * list only the entries before the first one that could not be
       * allocated rather than skipping it. */
      if (name_buf) zzplay_free(name_buf);
      if (node) zzplay_free(node);
      if (gui_context->controller) {
        zzplay_controller_set_message(gui_context->controller,
                                      "Not enough memory to list the "
                                      "whole playlist");
      }
      break;
    }
  }

  if (playlist_selected_row >= (int32_t)pl->count) {
    playlist_selected_row = (pl->count > 0U) ? (int32_t)(pl->count - 1U) : -1;
  }
  if (playlist_selected_row < 0 && pl->current >= 0) {
    playlist_selected_row = pl->current;
  }

  playlist_rendered_current = pl->current;
  if (pl_listview_gad && player_win) {
    GT_SetGadgetAttrs(pl_listview_gad, player_win, 0,
                      GTLV_Labels, (ULONG)&playlist_exec_list,
                      GTLV_Selected, (ULONG)(playlist_selected_row >= 0
                                                 ? playlist_selected_row
                                                 : ~0UL),
                      GTLV_MakeVisible, (ULONG)(playlist_selected_row >= 0
                                                    ? playlist_selected_row
                                                    : 0),
                      TAG_DONE);
  }
}
/* The list structure and names are stable across item changes. Move the
 * marker in place so advancing a track cannot allocate or relayout every row. */
static struct Node *playlist_node_at(int32_t row)
{
  struct Node *node;
  int32_t i;

  if (row < 0) {
    return 0;
  }
  node = playlist_exec_list.lh_Head;
  for (i = 0; node && node->ln_Succ && i < row; i++) {
    node = node->ln_Succ;
  }
  return node && node->ln_Succ && i == row ? node : 0;
}

static void update_current_playlist_node(void)
{
  ZZPlayPlaylist *pl;
  struct Node *node;
  char *name;
  size_t length;
  int32_t current;

  if (!gui_context || !gui_context->playlist) {
    return;
  }
  pl = gui_context->playlist;
  current = pl->current;
  if (current == playlist_rendered_current) {
    return;
  }
  node = playlist_node_at(playlist_rendered_current);
  if (node && node->ln_Name) {
    name = node->ln_Name;
    length = strlen(name);
    if (length >= 2U) {
      memmove(name + 3, name + 2, length - 1U);
      name[0] = ' ';
      name[1] = ' ';
      name[2] = ' ';
    }
  }
  node = playlist_node_at(current);
  if (node && node->ln_Name) {
    name = node->ln_Name;
    length = strlen(name);
    if (length >= 3U) {
      memmove(name + 2, name + 3, length - 2U);
      name[0] = '\xBB';
      name[1] = ' ';
    }
  }
  playlist_rendered_current = current;
  /* The listview highlights the new current row, so Remove must act on
   * that row too. */
  playlist_selected_row = current;
  if (pl_listview_gad && player_win) {
    GT_SetGadgetAttrs(pl_listview_gad, player_win, 0,
                      GTLV_Labels, (ULONG)&playlist_exec_list,
                      GTLV_Selected, (ULONG)(current >= 0 ? current : ~0UL),
                      GTLV_MakeVisible, (ULONG)(current >= 0 ? current : 0),
                      TAG_DONE);
  }
}

/* --- Menu checkmark synchronisation --- */
static void update_menu_checks(void)
{
  struct Menu *menu;
  struct MenuItem *item;

  if (!player_win || !player_menus || !gui_context ||
      !gui_context->controller) {
    return;
  }

  ClearMenuStrip(player_win);

  for (menu = player_menus; menu; menu = menu->NextMenu) {
    for (item = menu->FirstItem; item; item = item->NextItem) {
      uint32_t id = (uint32_t)GTMENUITEM_USERDATA(item);
      if (id == MID_SHUFFLE) {
        if (gui_context->controller->shuffle) {
          item->Flags |= CHECKED;
        } else {
          item->Flags &= ~CHECKED;
        }
      } else if (id == MID_FULLSCREEN) {
        if (gui_context->controller->fullscreen) {
          item->Flags |= CHECKED;
        } else {
          item->Flags &= ~CHECKED;
        }
      }
      if (item->SubItem) {
        struct MenuItem *sub;
        for (sub = item->SubItem; sub; sub = sub->NextItem) {
          uint32_t sub_id = (uint32_t)GTMENUITEM_USERDATA(sub);
          if (sub_id == MID_REPEAT_OFF) {
            if (gui_context->controller->repeat == ZZPLAY_REPEAT_OFF) {
              sub->Flags |= CHECKED;
            } else {
              sub->Flags &= ~CHECKED;
            }
          } else if (sub_id == MID_REPEAT_TRACK) {
            if (gui_context->controller->repeat == ZZPLAY_REPEAT_ONE) {
              sub->Flags |= CHECKED;
            } else {
              sub->Flags &= ~CHECKED;
            }
          } else if (sub_id == MID_REPEAT_ALL) {
            if (gui_context->controller->repeat == ZZPLAY_REPEAT_ALL) {
              sub->Flags |= CHECKED;
            } else {
              sub->Flags &= ~CHECKED;
            }
          }
        }
      }
    }
  }

  ResetMenuStrip(player_win, player_menus);
}

/* Take the window's screen LayerInfo lock and then its layer lock, the
 * layers library order, without waiting. Intuition holds the LayerInfo
 * lock (with every layer) while a menu is open or a window is dragged. */
static int try_lock_window(struct Window *window)
{
  struct SignalSemaphore *info = &window->WScreen->LayerInfo.Lock;

  if (!AttemptSemaphore(info)) {
    return 0;
  }
  if (!AttemptLockLayerRom(window->WLayer)) {
    ReleaseSemaphore(info);
    return 0;
  }
  return 1;
}

static void unlock_window(struct Window *window)
{
  UnlockLayerRom(window->WLayer);
  ReleaseSemaphore(&window->WScreen->LayerInfo.Lock);
}

int zzplay_gui_screen_free(struct Window *window)
{
  if (!window || !window->WScreen) {
    return 1;
  }
  if (!try_lock_window(window)) {
    return 0;
  }
  unlock_window(window);
  return 1;
}

/* Pure graphics may run holding these locks: graphics takes the same layer
 * lock again (it nests), and holding them keeps a menu from opening between
 * two drawing calls. Intuition and GadTools gadget calls must not run under
 * them: some hand work to Intuition's input task and wait for it. */
static int lock_screen_for_drawing(void)
{
  return try_lock_window(player_win);
}

static void unlock_screen_for_drawing(void)
{
  unlock_window(player_win);
}

/* Draw one info line, clipped to the panel so a long title never runs over
 * the bevel or into the window border. */
static void draw_info_line(struct RastPort *rp, int left, int top, int width,
                           int index, const char *text)
{
  struct TextExtent extent;
  ULONG fit;

  if (!text || text[0] == '\0' || width <= 0) {
    return;
  }
  fit = TextFit(rp, (CONST_STRPTR)text, (ULONG)strlen(text), &extent, 0, 1,
                (WORD)width, (WORD)(font_height + 1));
  if (fit == 0UL) {
    return;
  }
  Move(rp, (WORD)left, (WORD)(top + index * line_height + font_baseline));
  Text(rp, (CONST_STRPTR)text, fit);
}

/* Time row text ("~1:23 / 4:56   Playing"), shared by the full panel draw
 * and the position-only repaint so both always show the same thing. */
static void format_position_line(const ZZPlayController *ctl, char *line)
{
  char elapsed[16];
  char total[16];
  uint32_t elapsed_s = ctl->now.elapsed_ms / 1000U;

  sprintf(elapsed, "%lu:%02lu", (unsigned long)(elapsed_s / 60U),
          (unsigned long)(elapsed_s % 60U));
  if (ctl->now.total_ms > 0U) {
    uint32_t total_s = ctl->now.total_ms / 1000U;

    sprintf(total, "%lu:%02lu", (unsigned long)(total_s / 60U),
            (unsigned long)(total_s % 60U));
    sprintf(line, "%s%s / %s   %s", ctl->now.position_exact ? "" : "~",
            elapsed, total, ctl->paused ? "Paused" : "Playing");
  } else {
    sprintf(line, "%s%s   %s", ctl->now.position_exact ? "" : "~",
            elapsed, ctl->paused ? "Paused" : "Playing");
  }
}

/* Position-only changes clear one text row, preserving the panel bevel.
 * Returns 0 when the screen was locked and nothing was drawn. */
static int draw_position_info(void)
{
  struct RastPort *rp;
  ZZPlayController *ctl;
  char line[128];
  int left;
  int top;
  int width;
  int text_left;
  int text_top;
  int text_width;

  if (!player_win || !gui_context || !gui_context->controller) {
    return 1;
  }
  ctl = gui_context->controller;
  if (!ctl->item_active) {
    return 1;
  }
  format_position_line(ctl, line);
  if (strcmp(line, shown_position) == 0) {
    return 1;
  }
  if (!lock_screen_for_drawing()) {
    return 0;
  }
  rp = player_win->RPort;
  left = win_off_x + ZZPLAY_GUI_MARGIN;
  top = win_off_y + info_top;
  width = inner_width - 2 * ZZPLAY_GUI_MARGIN;
  text_left = left + 6;
  text_top = top + 3;
  text_width = width - 12;
  SetAPen(rp, player_draw_info ? player_draw_info->dri_Pens[BACKGROUNDPEN] : 0U);
  RectFill(rp, (WORD)(left + 2), (WORD)(text_top + 3 * line_height),
           (WORD)(left + width - 3),
           (WORD)(text_top + 4 * line_height - 1));
  SetAPen(rp, player_draw_info ? player_draw_info->dri_Pens[TEXTPEN] : 1U);
  SetBPen(rp, player_draw_info ? player_draw_info->dri_Pens[BACKGROUNDPEN] : 0U);
  SetDrMd(rp, JAM2);
  draw_info_line(rp, text_left, text_top, text_width, 3, line);
  unlock_screen_for_drawing();
  strcpy(shown_position, line);
  return 1;
}

/* --- Redraw Info Text --- */
/* Returns 0 when the screen was locked and nothing was drawn. */
static int draw_info(void)
{
  static char shown_title[48];
  struct RastPort *rp;
  ZZPlayController *ctl;
  char line[128];
  int left;
  int top;
  int width;
  int text_left;
  int text_top;
  int text_width;

  if (!player_win || !gui_context || !gui_context->controller) {
    return 1;
  }
  if (!lock_screen_for_drawing()) {
    return 0;
  }

  ctl = gui_context->controller;
  rp = player_win->RPort;
  left = win_off_x + ZZPLAY_GUI_MARGIN;
  top = win_off_y + info_top;
  width = inner_width - 2 * ZZPLAY_GUI_MARGIN;
  text_left = left + 6;
  text_top = top + 3;
  text_width = width - 12;

  /* Clear inside the bevel only; the bevel itself is drawn once below. */
  SetAPen(rp, player_draw_info ? player_draw_info->dri_Pens[BACKGROUNDPEN] : 0U);
  RectFill(rp, (WORD)(left + 2), (WORD)(top + 1), (WORD)(left + width - 3),
           (WORD)(top + info_height - 2));
  DrawBevelBox(rp, (WORD)left, (WORD)top, (WORD)width, (WORD)info_height,
               GT_VisualInfo, (ULONG)player_vi,
               GTBB_Recessed, TRUE,
               TAG_DONE);

  SetAPen(rp, player_draw_info ? player_draw_info->dri_Pens[TEXTPEN] : 1U);
  SetBPen(rp, player_draw_info ? player_draw_info->dri_Pens[BACKGROUNDPEN] : 0U);
  SetDrMd(rp, JAM2);

  if (ctl->item_active) {
    draw_info_line(rp, text_left, text_top, text_width, 0,
                   ctl->now.title[0] != '\0' ? ctl->now.title
                                             : "Unknown title");
    draw_info_line(rp, text_left, text_top, text_width, 1, ctl->now.format);
    sprintf(line, "Output: %s%s",
            ctl->now.output[0] != '\0' ? ctl->now.output : "selecting...",
            ctl->now.volume_supported ? "" : " (fixed volume)");
    draw_info_line(rp, text_left, text_top, text_width, 2, line);

    format_position_line(ctl, line);
    draw_info_line(rp, text_left, text_top, text_width, 3, line);
    strcpy(shown_position, line);
  } else {
    /* Idle: say what to do rather than leaving an unexplained label. */
    draw_info_line(rp, text_left, text_top, text_width, 0,
                   gui_context->playlist && gui_context->playlist->count != 0U
                       ? "Stopped. Press Play to start."
                       : "Drop files here, or click Add...");
    shown_position[0] = '\0';
  }
  draw_info_line(rp, text_left, text_top, text_width, 4, ctl->now.message);
  unlock_screen_for_drawing();

  /* Title bar: only touched when it changes, so playback progress does not
   * make Intuition redraw the title every second. */
  if (ctl->item_active) {
    sprintf(line, "ZZPlay - %s", ctl->paused ? "Paused" : "Playing");
  } else {
    strcpy(line, "ZZPlay");
  }
  if (strcmp(line, shown_title) != 0) {
    strncpy(shown_title, line, sizeof(shown_title) - 1U);
    shown_title[sizeof(shown_title) - 1U] = '\0';
    SetWindowTitles(player_win, (CONST_STRPTR)shown_title,
                    (CONST_STRPTR)~0UL);
  }
  return 1;
}

/* --- Settings Window Backend Mappings --- */
static ZZPlayAudioBackend index_to_mp3_backend(UWORD idx)
{
  switch (idx) {
    case 1:
      return ZZPLAY_AUDIO_MHI;
    case 2:
      return ZZPLAY_AUDIO_AHI;
    case 0:
    default:
      return ZZPLAY_AUDIO_AUTO;
  }
}

static UWORD mp3_backend_to_index(ZZPlayAudioBackend b)
{
  switch (b) {
    case ZZPLAY_AUDIO_MHI:
      return 1U;
    case ZZPLAY_AUDIO_AHI:
      return 2U;
    case ZZPLAY_AUDIO_AUTO:
    default:
      return 0U;
  }
}

static ZZPlayAudioBackend index_to_video_backend(UWORD idx)
{
  switch (idx) {
    case 1:
      return ZZPLAY_AUDIO_AHI;
    case 2:
      return ZZPLAY_AUDIO_AX;
    case 3:
      return ZZPLAY_AUDIO_NONE;
    case 0:
    default:
      return ZZPLAY_AUDIO_AUTO;
  }
}

static UWORD video_backend_to_index(ZZPlayAudioBackend b)
{
  switch (b) {
    case ZZPLAY_AUDIO_AHI:
      return 1U;
    case ZZPLAY_AUDIO_AX:
      return 2U;
    case ZZPLAY_AUDIO_NONE:
      return 3U;
    case ZZPLAY_AUDIO_AUTO:
    default:
      return 0U;
  }
}

static void apply_settings_device_fields(ZZPlayPrefs *dest,
                                         const ZZPlayPrefs *src)
{
  dest->mp3_output = src->mp3_output;
  dest->video_audio = src->video_audio;
  dest->ahi_unit = src->ahi_unit;
  strncpy(dest->mhi_driver, src->mhi_driver, sizeof(dest->mhi_driver) - 1U);
  dest->preload = src->preload;
  dest->mhi_driver[sizeof(dest->mhi_driver) - 1U] = '\0';
}

static void close_settings_window(void)
{
  if (settings_win) {
    CloseWindow(settings_win);
    settings_win = 0;
    settings_refresh_pending = 0;
  }
  if (settings_gadgets) {
    FreeGadgets(settings_gadgets);
    settings_gadgets = 0;
  }
  if (settings_vi) {
    FreeVisualInfo(settings_vi);
    settings_vi = 0;
  }
  free_exec_list_nodes(&mhi_exec_list);
  mhi_count = 0U;
}

static void open_settings_window(void)
{
  struct NewGadget ng;
  struct Gadget *gad;
  /* Static, not on the stack: this runs inside the GUI event handler,
   * possibly nested in a playback loop on a small shell stack. */
  static char discovered[ZZPLAY_DEVICES_MAX_MHI][ZZPLAY_PREFS_DRIVER_MAX];
  static const char *const setting_labels[] = {
    "MP3 output", "Video sound", "Preload media", "AHI unit"
  };
  struct RastPort *srp;
  int label_w;
  int value_w;
  int list_h;
  uint32_t disc_count;
  uint32_t i;
  int found_current = 0;
  int32_t selected_mhi = 0;
  int btn_h = font_height + 6;
  int s_width;
  int cur_y = ZZPLAY_GUI_MARGIN;
  int left;
  int top;
  int btn_w;

  if (settings_win) {
    WindowToFront(settings_win);
    ActivateWindow(settings_win);
    return;
  }

  if (!player_screen || !gui_context || !gui_context->prefs) {
    return;
  }

  settings_vi = GetVisualInfo(player_screen, TAG_DONE);
  if (!settings_vi) {
    return;
  }

  settings_copy = *gui_context->prefs;

  /* Prepare AHI unit labels */
  zzplay_devices_ahi_units(ahi_unit_labels);
  for (i = 0U; i < ZZPLAY_PREFS_AHI_UNITS; i++) {
    ahi_cycle_ptrs[i] = ahi_unit_labels[i];
  }
  ahi_cycle_ptrs[ZZPLAY_PREFS_AHI_UNITS] = 0;

  /* Prepare MHI driver list */
  free_exec_list_nodes(&mhi_exec_list);
  init_exec_list(&mhi_exec_list);
  mhi_count = 0U;

  disc_count = zzplay_devices_mhi_drivers(discovered, ZZPLAY_DEVICES_MAX_MHI);
  for (i = 0U; i < disc_count; i++) {
    if (strcasecmp(discovered[i], settings_copy.mhi_driver) == 0) {
      found_current = 1;
      selected_mhi = (int32_t)i;
    }
  }

  if (!found_current && settings_copy.mhi_driver[0] != '\0') {
    /* Current driver not found in LIBS:MHI, add as (missing) at the head */
    char name_buf[ZZPLAY_PREFS_DRIVER_MAX + 16];
    struct Node *node = (struct Node *)zzplay_alloc(sizeof(struct Node));
    char *dup_name;

    sprintf(name_buf, "%s (missing)", settings_copy.mhi_driver);
    dup_name = gui_strdup(name_buf);
    if (node && dup_name) {
      memset(node, 0, sizeof(*node));
      node->ln_Name = dup_name;
      AddTail(&mhi_exec_list, node);
      strncpy(mhi_clean_names[0], settings_copy.mhi_driver,
              ZZPLAY_PREFS_DRIVER_MAX - 1U);
      mhi_clean_names[0][ZZPLAY_PREFS_DRIVER_MAX - 1U] = '\0';
      mhi_count++;
      selected_mhi = 0;
    } else {
      if (dup_name) zzplay_free(dup_name);
      if (node) zzplay_free(node);
    }
  }

  for (i = 0U; i < disc_count; i++) {
    struct Node *node = (struct Node *)zzplay_alloc(sizeof(struct Node));
    char *dup_name = gui_strdup(discovered[i]);

    if (node && dup_name) {
      memset(node, 0, sizeof(*node));
      node->ln_Name = dup_name;
      AddTail(&mhi_exec_list, node);
      strncpy(mhi_clean_names[mhi_count], discovered[i],
              ZZPLAY_PREFS_DRIVER_MAX - 1U);
      mhi_clean_names[mhi_count][ZZPLAY_PREFS_DRIVER_MAX - 1U] = '\0';
      if (found_current &&
          strcasecmp(discovered[i], settings_copy.mhi_driver) == 0) {
        selected_mhi = (int32_t)mhi_count;
      }
      mhi_count++;
    } else {
      if (dup_name) zzplay_free(dup_name);
      if (node) zzplay_free(node);
    }
  }

  /* Gadgets: a label column sized to the longest label, a value column
   * sized to the longest choice (AHI mode names can be long), all in the
   * screen font and offset by the window borders like the player window. */
  srp = &player_screen->RastPort;
  label_w = 0;
  for (i = 0U; i < 4U; i++) {
    int w = gui_text_width(srp, setting_labels[i]);

    if (w > label_w) {
      label_w = w;
    }
  }
  label_w += 12;
  value_w = gui_text_width(srp, "ZZ9000AX") + 32;
  {
    int w = gui_text_width(srp, "Preload whole file") + 32;

    if (w > value_w) {
      value_w = w;
    }
  }
  for (i = 0U; i < ZZPLAY_PREFS_AHI_UNITS; i++) {
    int w = gui_text_width(srp, ahi_unit_labels[i]) + 32;

    if (w > value_w) {
      value_w = w;
    }
  }
  s_width = 2 * ZZPLAY_GUI_MARGIN + label_w + value_w;
  if (s_width < 300) {
    s_width = 300;
    value_w = s_width - 2 * ZZPLAY_GUI_MARGIN - label_w;
  }
  if (s_width > player_screen->Width - player_screen->WBorLeft -
                    player_screen->WBorRight) {
    s_width = player_screen->Width - player_screen->WBorLeft -
              player_screen->WBorRight;
    value_w = s_width - 2 * ZZPLAY_GUI_MARGIN - label_w;
  }
  list_h = 4 * font_height + 4;
  btn_w = (s_width - 2 * ZZPLAY_GUI_MARGIN - 2 * ZZPLAY_GUI_SPACING) / 3;

  settings_gadgets = 0;
  gad = CreateContext(&settings_gadgets);
  if (!gad) {
    close_settings_window();
    return;
  }

  memset(&ng, 0, sizeof(ng));
  ng.ng_VisualInfo = settings_vi;
  ng.ng_TextAttr = gui_text_attr;

  gad = gui_gadget(CYCLE_KIND, gad, &ng, ZZPLAY_GUI_MARGIN + label_w, cur_y,
                   value_w, btn_h, "MP3 output", SGID_MP3_OUTPUT,
                   PLACETEXT_LEFT,
                   GUI_TAGS(GTCY_Labels, (ULONG)mp3_backend_labels,
                   GTCY_Active,
                   (ULONG)mp3_backend_to_index(settings_copy.mp3_output),
                   TAG_DONE));
  cur_y += btn_h + ZZPLAY_GUI_SPACING;

  gad = gui_gadget(CYCLE_KIND, gad, &ng, ZZPLAY_GUI_MARGIN + label_w, cur_y,
                   value_w, btn_h, "Video sound", SGID_VIDEO_SOUND,
                   PLACETEXT_LEFT,
                   GUI_TAGS(GTCY_Labels, (ULONG)video_backend_labels,
                   GTCY_Active,
                   (ULONG)video_backend_to_index(settings_copy.video_audio),
                   TAG_DONE));
  cur_y += btn_h + ZZPLAY_GUI_SPACING;
  gad = gui_gadget(CHECKBOX_KIND, gad, &ng,
                   ZZPLAY_GUI_MARGIN + label_w, cur_y,
                   CHECKBOXWIDTH, CHECKBOXHEIGHT, "Preload whole file",
                   SGID_PRELOAD, PLACETEXT_RIGHT,
                   GUI_TAGS(GTCB_Checked,
                            settings_copy.preload ? TRUE : FALSE,
                            TAG_DONE));
  cur_y += btn_h + ZZPLAY_GUI_SPACING;

  gad = gui_gadget(CYCLE_KIND, gad, &ng, ZZPLAY_GUI_MARGIN + label_w, cur_y,
                   value_w, btn_h, "AHI unit", SGID_AHI_UNIT,
                   PLACETEXT_LEFT,
                   GUI_TAGS(GTCY_Labels, (ULONG)ahi_cycle_ptrs,
                   GTCY_Active,
                   (ULONG)(settings_copy.ahi_unit < ZZPLAY_PREFS_AHI_UNITS
                               ? settings_copy.ahi_unit
                               : 0U),
                   TAG_DONE));
  /* Room for the listview's label, which GadTools draws above it. */
  cur_y += btn_h + ZZPLAY_GUI_SPACING + font_height + 4;

  gad = gui_gadget(
      LISTVIEW_KIND, gad, &ng, ZZPLAY_GUI_MARGIN, cur_y,
      s_width - 2 * ZZPLAY_GUI_MARGIN, list_h, "MHI driver",
      SGID_MHI_LIST, PLACETEXT_ABOVE,
      GUI_TAGS(GTLV_Labels, (ULONG)&mhi_exec_list,
               GTLV_Selected, (ULONG)selected_mhi,
               GTLV_MakeVisible, (ULONG)selected_mhi,
               GTLV_ShowSelected, 0,
               GTLV_ScrollWidth, 18,
               TAG_DONE));
  cur_y += list_h + ZZPLAY_GUI_SPACING;

  gad = gui_gadget(TEXT_KIND, gad, &ng, ZZPLAY_GUI_MARGIN, cur_y,
                   s_width - 2 * ZZPLAY_GUI_MARGIN, font_height + 2, 0,
                   SGID_NOTE_TEXT, 0,
                   GUI_TAGS(GTTX_Text, (ULONG)"Changes apply from the next item.",
                   GTTX_Border, FALSE,
                   TAG_DONE));
  cur_y += font_height + 2 + ZZPLAY_GUI_SPACING;

  gad = gui_gadget(BUTTON_KIND, gad, &ng, ZZPLAY_GUI_MARGIN, cur_y, btn_w,
                   btn_h, "Save", SGID_SAVE, PLACETEXT_IN, GUI_TAGS(TAG_DONE));
  gad = gui_gadget(BUTTON_KIND, gad, &ng,
                   ZZPLAY_GUI_MARGIN + btn_w + ZZPLAY_GUI_SPACING, cur_y,
                   btn_w, btn_h, "Use", SGID_USE, PLACETEXT_IN, GUI_TAGS(TAG_DONE));
  gad = gui_gadget(BUTTON_KIND, gad, &ng,
                   s_width - ZZPLAY_GUI_MARGIN - btn_w, cur_y, btn_w, btn_h,
                   "Cancel", SGID_CANCEL, PLACETEXT_IN, GUI_TAGS(TAG_DONE));
  cur_y += btn_h + ZZPLAY_GUI_MARGIN;

  if (!gad) {
    close_settings_window();
    zzplay_controller_set_message(gui_context->controller,
                                  "Not enough memory for the settings window");
    return;
  }

  left = player_win ? (player_win->LeftEdge + 30) : 40;
  top = player_win ? (player_win->TopEdge + 30) : 40;

  settings_win = OpenWindowTags(
      0,
      WA_Title, (ULONG)"ZZPlay Settings",
      WA_Left, (ULONG)left,
      WA_Top, (ULONG)top,
      WA_InnerWidth, (ULONG)s_width,
      WA_InnerHeight, (ULONG)cur_y,
      WA_AutoAdjust, TRUE,
      WA_PubScreen, (ULONG)player_screen,
      WA_Gadgets, (ULONG)settings_gadgets,
      WA_DragBar, TRUE,
      WA_CloseGadget, TRUE,
      WA_DepthGadget, TRUE,
      WA_Activate, TRUE,
      WA_SmartRefresh, TRUE,
      WA_IDCMP,
      IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
      BUTTONIDCMP | CYCLEIDCMP | LISTVIEWIDCMP,
      TAG_DONE);

  if (!settings_win) {
    close_settings_window();
    return;
  }

  GT_RefreshWindow(settings_win, 0);
}

/* Drops can expand drawers and playlists synchronously, so retain the modal
 * gate for them. ASL and About requesters use zzplay-requester.c instead. */
static void execute_modal_op(ZZPlayModal op)
{
  ZZPlayController *ctl;
  ZZPlayPlaylist *pl;
  int32_t first_added = -1;
  uint32_t count_before;
  uint32_t i;

  if (!gui_context || op != ZZPLAY_MODAL_ADD_DROPPED) {
    return;
  }
  ctl = gui_context->controller;
  pl = gui_context->playlist;
  count_before = pl->count;
  for (i = 0U; i < drop_count; i++) {
    uint32_t added = zzplay_files_add(pl, drop_paths[i]);

    if (added > 0U && first_added < 0) {
      first_added = (int32_t)count_before;
    }
    count_before += added;
  }
  drop_queue_free();
  if (!ctl->item_active && first_added >= 0) {
    zzplay_controller_jump(ctl, first_added);
  }
  ctl->dirty |= ZZPLAY_DIRTY_PLAYLIST;
}

static int playlist_append_entries(ZZPlayPlaylist *dst,
                                   const ZZPlayPlaylist *src,
                                   int32_t *first_added, uint32_t *added,
                                   int *truncated)
{
  uint32_t i;

  for (i = 0U; i < src->count; i++) {
    const ZZPlayPlaylistEntry *entry = &src->entries[i];
    int32_t index = zzplay_playlist_add(dst, entry->path);

    if (index < 0) {
      if (dst->count == ZZPLAY_PLAYLIST_MAX_ENTRIES) {
        if (truncated) {
          *truncated = 1;
        }
        break;
      }
      return 0;
    }
    zzplay_playlist_set_title(dst, (uint32_t)index, entry->title);
    dst->entries[index].duration_ms = entry->duration_ms;
    dst->entries[index].failed = entry->failed;
    if (first_added && *first_added < 0) {
      *first_added = index;
    }
    if (added) {
      (*added)++;
    }
  }
  return 1;
}

static int playlist_clone(ZZPlayPlaylist *dst, const ZZPlayPlaylist *src)
{
  zzplay_playlist_init(dst);
  dst->repeat = src->repeat;
  dst->shuffle = 0;
  dst->rng = src->rng;
  if (!playlist_append_entries(dst, src, 0, 0, 0)) {
    zzplay_playlist_free(dst);
    return 0;
  }
  if (src->count != 0U) {
    memcpy(dst->order, src->order, src->count * sizeof(*dst->order));
  }
  dst->current = src->current;
  dst->resume_pos = src->resume_pos;
  dst->repeat = src->repeat;
  dst->shuffle = src->shuffle;
  dst->rng = src->rng;
  return 1;
}

static void apply_requester_result(ZZPlayController *ctl, ZZPlayPlaylist *pl,
                                   ZZPlayPrefs *prefs,
                                   ZZPlayRequesterResult *result)
{
  int32_t first_added = -1;
  uint32_t added = 0U;
  int truncated = 0;
  ZZPlayPlaylist next;

  if (result->error[0] != '\0') {
    zzplay_controller_set_message(ctl, result->error);
    return;
  }
  if (!result->accepted) {
    return;
  }
  if (result->prepared.allocation_failed) {
    zzplay_controller_set_message(ctl, "Unable to update playlist");
    return;
  }
  if (result->drawer[0] != '\0' && prefs) {
    strncpy(prefs->last_drawer, result->drawer, sizeof(prefs->last_drawer) - 1U);
    prefs->last_drawer[sizeof(prefs->last_drawer) - 1U] = '\0';
  }
  switch (result->op) {
  case ZZPLAY_MODAL_OPEN_FILES:
    zzplay_playlist_init(&next);
    next.repeat = pl->repeat;
    next.shuffle = pl->shuffle;
    next.rng = pl->rng;
    if (!playlist_append_entries(&next, &result->prepared, &first_added, 0,
                                 0)) {
      zzplay_playlist_free(&next);
      zzplay_controller_set_message(ctl, "Unable to update playlist");
      return;
    }
    zzplay_playlist_free(pl);
    *pl = next;
    if (first_added >= 0) {
      /* Replacing a playing list ends that item only after the user accepts. */
      zzplay_controller_jump(ctl, first_added);
    }
    ctl->dirty |= ZZPLAY_DIRTY_PLAYLIST;
    break;

  case ZZPLAY_MODAL_ADD_FILES:
  case ZZPLAY_MODAL_ADD_DRAWER:
  case ZZPLAY_MODAL_LOAD_PLAYLIST:
    if (!playlist_clone(&next, pl) ||
        !playlist_append_entries(&next, &result->prepared, 0, &added,
                                 &truncated)) {
      zzplay_playlist_free(&next);
      zzplay_controller_set_message(ctl, "Unable to update playlist");
      return;
    }
    zzplay_playlist_free(pl);
    *pl = next;
    if (truncated) {
      char message[64];

      sprintf(message, "Playlist full, %lu added", (unsigned long)added);
      zzplay_controller_set_message(ctl, message);
    }
    ctl->dirty |= ZZPLAY_DIRTY_PLAYLIST;
    break;

  case ZZPLAY_MODAL_SAVE_PLAYLIST:
    if (!result->saved) {
      zzplay_controller_set_message(ctl, "Failed to save playlist");
    }
    break;

  default:
    break;
  }
}

static void start_requester(ZZPlayModal op)
{
  if (zzplay_requester_active()) {
    zzplay_controller_set_message(gui_context->controller,
                                  "A file requester is already open");
    return;
  }
  if (!zzplay_requester_start(op,
                               gui_context->prefs ? gui_context->prefs->last_drawer : "",
                               gui_context->version, gui_context->playlist,
                               player_screen)) {
    zzplay_controller_set_message(gui_context->controller,
                                  "Unable to open requester");
  }
}

static void request_quit(ZZPlayController *ctl)
{
  if (zzplay_requester_active()) {
    zzplay_controller_set_message(ctl,
                                  "Close the file requester before quitting");
    return;
  }
  zzplay_controller_request(ctl, ZZPLAY_REQUEST_QUIT);
}

/* Text width in the screen font, which is what GadTools renders labels in
 * (ng_TextAttr is the screen font). */
static int gui_text_width(struct RastPort *rp, const char *text)
{
  return (int)TextLength(rp, (CONST_STRPTR)text, (ULONG)strlen(text));
}

/* Create one player-window gadget at inner-area coordinates; the window
 * border offset is applied here so no call site can forget it. */
static struct Gadget *gui_gadget(ULONG kind, struct Gadget *prev,
                                 struct NewGadget *ng, int x, int y,
                                 int w, int h, const char *label,
                                 UWORD id, ULONG flags,
                                 const struct TagItem *tags)
{
  if (!prev) {
    return 0;
  }
  ng->ng_LeftEdge = (WORD)(win_off_x + x);
  ng->ng_TopEdge = (WORD)(win_off_y + y);
  ng->ng_Width = (WORD)w;
  ng->ng_Height = (WORD)h;
  ng->ng_GadgetText = (CONST_STRPTR)label;
  ng->ng_GadgetID = id;
  ng->ng_Flags = flags;
  return CreateGadgetA(kind, prev, ng, (struct TagItem *)tags);
}

/* --- Public API --- */

int zzplay_gui_open(const ZZPlayGuiContext *context)
{
  struct TextFont *font;
  struct RastPort *srp;
  struct NewGadget ng;
  struct Gadget *gad;
  static const char *const button_labels[] = {
    "|<", "Play", "Pause", "Stop", ">|",
    "Add...", "Remove", "Clear", "Load...", "Save..."
  };
  int min_btn_w;
  int btn_h;
  int slider_h;
  int btn_w;
  int cur_y;
  int content_w;
  int label_vol;
  int label_rep;
  int label_shuf;
  int cycle_w;
  int vol_w;
  int required_w;
  int listview_h;
  int x;
  int i;
  int left;
  int top;

  static struct NewMenu new_menu[] = {
    { NM_TITLE, (CONST_STRPTR)"Project",       0, 0, 0, (APTR)MID_PROJECT },
    { NM_ITEM,  (CONST_STRPTR)"Open...",       (CONST_STRPTR)"O", 0, 0, (APTR)MID_OPEN },
    { NM_ITEM,  (CONST_STRPTR)"Add files...",  (CONST_STRPTR)"A", 0, 0, (APTR)MID_ADD_FILES },
    { NM_ITEM,  (CONST_STRPTR)"Add drawer...", 0, 0, 0, (APTR)MID_ADD_DRAWER },
    { NM_ITEM,  (CONST_STRPTR)"Load playlist...", (CONST_STRPTR)"L", 0, 0, (APTR)MID_LOAD_PLAYLIST },
    { NM_ITEM,  (CONST_STRPTR)"Save playlist...", (CONST_STRPTR)"S", 0, 0, (APTR)MID_SAVE_PLAYLIST },
    { NM_ITEM,  (CONST_STRPTR)"Settings...",   (CONST_STRPTR)"P", 0, 0, (APTR)MID_SETTINGS },
    { NM_ITEM,  (CONST_STRPTR)"About...",      0, 0, 0, (APTR)MID_ABOUT },
    { NM_ITEM,  (CONST_STRPTR)"Quit",          (CONST_STRPTR)"Q", 0, 0, (APTR)MID_QUIT },

    { NM_TITLE, (CONST_STRPTR)"Control",       0, 0, 0, (APTR)MID_CONTROL },
    { NM_ITEM,  (CONST_STRPTR)"Play/Pause",    0, 0, 0, (APTR)MID_PLAY_PAUSE },
    { NM_ITEM,  (CONST_STRPTR)"Stop",          0, 0, 0, (APTR)MID_STOP },
    { NM_ITEM,  (CONST_STRPTR)"Next",          (CONST_STRPTR)"N", 0, 0, (APTR)MID_NEXT },
    { NM_ITEM,  (CONST_STRPTR)"Previous",      (CONST_STRPTR)"B", 0, 0, (APTR)MID_PREVIOUS },
    { NM_ITEM,  (CONST_STRPTR)"Seek forward",  0, 0, 0, (APTR)MID_SEEK_FWD },
    { NM_ITEM,  (CONST_STRPTR)"Seek back",     0, 0, 0, (APTR)MID_SEEK_BACK },
    { NM_ITEM,  (CONST_STRPTR)"Repeat",        0, 0, 0, 0 },
    { NM_SUB,   (CONST_STRPTR)"Off",           0, CHECKIT, 6, (APTR)MID_REPEAT_OFF },
    { NM_SUB,   (CONST_STRPTR)"Track",         0, CHECKIT, 5, (APTR)MID_REPEAT_TRACK },
    { NM_SUB,   (CONST_STRPTR)"All",           0, CHECKIT, 3, (APTR)MID_REPEAT_ALL },
    { NM_ITEM,  (CONST_STRPTR)"Shuffle",       0, CHECKIT | MENUTOGGLE, 0, (APTR)MID_SHUFFLE },
    { NM_ITEM,  (CONST_STRPTR)"Fullscreen video", (CONST_STRPTR)"F", CHECKIT | MENUTOGGLE, 0, (APTR)MID_FULLSCREEN },

    { NM_END, 0, 0, 0, 0, 0 }
  };

  if (!context || !context->controller || !context->playlist) {
    return 0;
  }
  gui_context_copy = *context;
  gui_context = &gui_context_copy;

  if (!GfxBase) {
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library",
                                           37U);
    owns_gfx = GfxBase ? 1 : 0;
  }
  if (!GadToolsBase) {
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 37U);
    owns_gadtools = GadToolsBase ? 1 : 0;
  }
  if (!WorkbenchBase) {
    WorkbenchBase = OpenLibrary((CONST_STRPTR)"workbench.library", 36U);
    owns_workbench = WorkbenchBase ? 1 : 0;
  }

  if (!GfxBase || !GadToolsBase) {
    return 0;
  }

  player_screen = LockPubScreen(0);
  if (!player_screen) {
    return 0;
  }

  player_draw_info = GetScreenDrawInfo(player_screen);
  player_vi = GetVisualInfo(player_screen, TAG_DONE);
  if (!player_vi) {
    zzplay_gui_close();
    return 0;
  }

  /* Everything is measured in the screen font, which is also the font the
   * gadgets are created with, so a larger Workbench font grows the window
   * instead of clipping labels. */
  font = player_screen->RastPort.Font;
  srp = &player_screen->RastPort;
  gui_text_attr = player_screen->Font;
  font_height = (font && font->tf_YSize > 0) ? (int)font->tf_YSize : 8;
  font_baseline = (font && font->tf_Baseline > 0) ? (int)font->tf_Baseline : 6;
  line_height = font_height + 1;
  info_height = ZZPLAY_GUI_LINES * line_height + 6;
  btn_h = font_height + 6;
  slider_h = font_height + 2;
  win_off_x = player_screen->WBorLeft;
  win_off_y = player_screen->WBorTop + player_screen->Font->ta_YSize + 1;

  min_btn_w = 0;
  for (i = 0; i < (int)(sizeof(button_labels) / sizeof(button_labels[0]));
       i++) {
    int w = gui_text_width(srp, button_labels[i]) + 16;

    if (w > min_btn_w) {
      min_btn_w = w;
    }
  }

  /* Controls row, left to right: "Volume" [slider] "Repeat" [cycle]
   * [x] "Shuffle". Labels get their own room so they never sit on top of
   * the neighbouring gadget. */
  label_vol = gui_text_width(srp, "Volume") + 8;
  label_rep = gui_text_width(srp, "Repeat") + 8;
  label_shuf = gui_text_width(srp, "Shuffle") + 6;
  cycle_w = gui_text_width(srp, "Track");
  if (gui_text_width(srp, "All") > cycle_w) {
    cycle_w = gui_text_width(srp, "All");
  }
  cycle_w += 32; /* the cycle glyph and its separator */
  required_w = 2 * ZZPLAY_GUI_MARGIN + label_vol + 80 +
               2 * ZZPLAY_GUI_SPACING + label_rep + cycle_w +
               2 * ZZPLAY_GUI_SPACING + CHECKBOXWIDTH + label_shuf;

  inner_width = 2 * ZZPLAY_GUI_MARGIN + 5 * min_btn_w + 4 * ZZPLAY_GUI_SPACING;
  if (inner_width < required_w) {
    inner_width = required_w;
  }
  if (inner_width < ZZPLAY_GUI_MIN_WIDTH) {
    inner_width = ZZPLAY_GUI_MIN_WIDTH;
  }
  if (inner_width > player_screen->Width - player_screen->WBorLeft -
                        player_screen->WBorRight) {
    inner_width = player_screen->Width - player_screen->WBorLeft -
                  player_screen->WBorRight;
  }
  content_w = inner_width - 2 * ZZPLAY_GUI_MARGIN;
  btn_w = (content_w - 4 * ZZPLAY_GUI_SPACING) / 5;
  vol_w = content_w - label_vol - 2 * ZZPLAY_GUI_SPACING - label_rep -
          cycle_w - 2 * ZZPLAY_GUI_SPACING - CHECKBOXWIDTH - label_shuf;
  /* GadTools sizes a listview to whole rows plus its frame. */
  listview_h = 7 * font_height + 4;

  player_gadgets = 0;
  gad = CreateContext(&player_gadgets);
  if (!gad) {
    zzplay_gui_close();
    return 0;
  }
  memset(&ng, 0, sizeof(ng));
  ng.ng_VisualInfo = player_vi;
  ng.ng_TextAttr = gui_text_attr;

  /* Now-playing panel (drawn by draw_info), then the position slider. */
  cur_y = ZZPLAY_GUI_MARGIN;
  info_top = cur_y;
  cur_y += info_height + ZZPLAY_GUI_SPACING;

  pos_slider_gad = gad = gui_gadget(
      SLIDER_KIND, gad, &ng, ZZPLAY_GUI_MARGIN, cur_y, content_w, slider_h,
      0, GID_POS_SLIDER, 0,
      GUI_TAGS(GTSL_Min, 0, GTSL_Max, 1000, GTSL_Level, 0,
      PGA_Freedom, LORIENT_HORIZ, GA_RelVerify, TRUE, GA_Immediate, TRUE,
      GA_Disabled, TRUE, TAG_DONE));
  pos_slider_last_disabled = -1;
  cur_y += slider_h + ZZPLAY_GUI_SPACING;

  /* Transport: |<  Play  Pause  Stop  >| */
  for (i = 0; i < 5; i++) {
    gad = gui_gadget(BUTTON_KIND, gad, &ng,
                     ZZPLAY_GUI_MARGIN + i * (btn_w + ZZPLAY_GUI_SPACING),
                     cur_y, btn_w, btn_h, button_labels[i],
                     (UWORD)(GID_PREV + i), PLACETEXT_IN, GUI_TAGS(TAG_DONE));
  }
  cur_y += btn_h + ZZPLAY_GUI_SPACING;

  /* Controls row. */
  x = ZZPLAY_GUI_MARGIN + label_vol;
  vol_slider_gad = gad = gui_gadget(
      SLIDER_KIND, gad, &ng, x, cur_y + (btn_h - slider_h) / 2, vol_w,
      slider_h, "Volume", GID_VOL_SLIDER, PLACETEXT_LEFT,
      GUI_TAGS(GTSL_Min, 0, GTSL_Max, (ULONG)ZZPLAY_VOLUME_MAX,
      GTSL_Level, (ULONG)gui_context->controller->volume,
      PGA_Freedom, LORIENT_HORIZ, GA_RelVerify, TRUE, TAG_DONE));
  x += vol_w + 2 * ZZPLAY_GUI_SPACING + label_rep;
  repeat_gad = gad = gui_gadget(
      CYCLE_KIND, gad, &ng, x, cur_y, cycle_w, btn_h, "Repeat",
      GID_REPEAT_CYCLE, PLACETEXT_LEFT,
      GUI_TAGS(GTCY_Labels, (ULONG)repeat_cycle_labels,
      GTCY_Active, (ULONG)gui_context->controller->repeat, TAG_DONE));
  x += cycle_w + 2 * ZZPLAY_GUI_SPACING;
  shuffle_gad = gad = gui_gadget(
      CHECKBOX_KIND, gad, &ng, x, cur_y + (btn_h - CHECKBOXHEIGHT) / 2,
      CHECKBOXWIDTH, CHECKBOXHEIGHT, "Shuffle", GID_SHUFFLE_CHECK,
      PLACETEXT_RIGHT,
      GUI_TAGS(GTCB_Checked, gui_context->controller->shuffle ? TRUE : FALSE,
      TAG_DONE));
  cur_y += btn_h + ZZPLAY_GUI_SPACING;

  /* Playlist. */
  init_exec_list(&playlist_exec_list);
  pl_listview_gad = gad = gui_gadget(
      LISTVIEW_KIND, gad, &ng, ZZPLAY_GUI_MARGIN, cur_y, content_w,
      listview_h, 0, GID_PLAYLIST_LIST, 0,
      GUI_TAGS(GTLV_Labels, (ULONG)&playlist_exec_list,
      GTLV_ShowSelected, 0, GTLV_ScrollWidth, 18, TAG_DONE));
  cur_y += listview_h + ZZPLAY_GUI_SPACING;

  /* Playlist editing: Add...  Remove  Clear  Load...  Save... */
  for (i = 0; i < 5; i++) {
    gad = gui_gadget(BUTTON_KIND, gad, &ng,
                     ZZPLAY_GUI_MARGIN + i * (btn_w + ZZPLAY_GUI_SPACING),
                     cur_y, btn_w, btn_h, button_labels[5 + i],
                     (UWORD)(GID_PL_ADD + i), PLACETEXT_IN, GUI_TAGS(TAG_DONE));
  }
  cur_y += btn_h + ZZPLAY_GUI_MARGIN;

  /* Any failed allocation leaves a NULL that every later gui_gadget call
   * propagates, so one check covers the whole chain. */
  if (!gad || !pos_slider_gad || !vol_slider_gad || !repeat_gad ||
      !shuffle_gad || !pl_listview_gad) {
    zzplay_gui_close();
    return 0;
  }

  /* Menus */
  player_menus = CreateMenus(new_menu, TAG_DONE);
  if (player_menus &&
      !LayoutMenus(player_menus, player_vi, GTMN_NewLookMenus, TRUE,
                   TAG_DONE)) {
    FreeMenus(player_menus);
    player_menus = 0;
  }

  /* Open Window */
  left = (gui_context->prefs && gui_context->prefs->window_left >= 0)
             ? (int)gui_context->prefs->window_left
             : 20;
  top = (gui_context->prefs && gui_context->prefs->window_top >= 0)
            ? (int)gui_context->prefs->window_top
            : (player_screen->BarHeight + 4);

  player_win = OpenWindowTags(
      0,
      WA_Title, (ULONG)"ZZPlay",
      WA_Left, (ULONG)left,
      WA_Top, (ULONG)top,
      WA_InnerWidth, (ULONG)inner_width,
      WA_InnerHeight, (ULONG)cur_y,
      /* A remembered position from a larger screen must not push the
       * window off this one. */
      WA_AutoAdjust, TRUE,
      WA_PubScreen, (ULONG)player_screen,
      WA_Gadgets, (ULONG)player_gadgets,
      WA_DragBar, TRUE,
      WA_CloseGadget, TRUE,
      WA_DepthGadget, TRUE,
      WA_Activate, TRUE,
      /* Smart refresh preserves the player while its independent requester
       * process is active. */
      WA_SmartRefresh, TRUE,
      /* Without this Intuition renders the GadTools new-look menu layout
       * with the old-look pens: black text on a black strip on many
       * screens. GTMN_NewLookMenus only covers the layout half. */
      WA_NewLookMenus, TRUE,
      WA_IDCMP,
      IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_RAWKEY |
      IDCMP_MENUPICK | IDCMP_REFRESHWINDOW |
      BUTTONIDCMP | SLIDERIDCMP | CYCLEIDCMP | CHECKBOXIDCMP |
      LISTVIEWIDCMP,
      TAG_DONE);

  if (!player_win) {
    zzplay_gui_close();
    return 0;
  }

  if (player_menus) {
    SetMenuStrip(player_win, player_menus);
  }

  GT_RefreshWindow(player_win, 0);

  /* AppWindow */
  if (WorkbenchBase) {
    app_port = CreateMsgPort();
    if (app_port) {
      app_window = AddAppWindowA(1U, 0U, player_win, app_port, 0);
    }
  }

  rebuild_playlist_nodes();
  context->controller->dirty &=
      ~(ZZPLAY_DIRTY_PLAYLIST | ZZPLAY_DIRTY_CURRENT);
  update_menu_checks();
  if (!draw_info()) {
    context->controller->dirty |= ZZPLAY_DIRTY_INFO;
  }

  return 1;
}

void zzplay_gui_close(void)
{
  if (player_win && gui_context && gui_context->prefs) {
    gui_context->prefs->window_left = (int16_t)player_win->LeftEdge;
    gui_context->prefs->window_top = (int16_t)player_win->TopEdge;
  }

  if (zzplay_requester_active() && gui_context && gui_context->controller) {
    zzplay_controller_set_message(gui_context->controller,
                                  "Close the file requester before quitting");
  }
  /* An ASL requester cannot be closed from another task. Wait for its child
   * before tearing down the screen or letting this task exit. */
  zzplay_requester_close();
  close_settings_window();

  if (app_window) {
    RemoveAppWindow(app_window);
    app_window = 0;
  }
  if (app_port) {
    struct Message *msg;
    while ((msg = GetMsg(app_port)) != 0) {
      ReplyMsg(msg);
    }
    DeleteMsgPort(app_port);
    app_port = 0;
  }

  free_exec_list_nodes(&playlist_exec_list);
  drop_queue_free();

  if (player_win && player_menus) {
    ClearMenuStrip(player_win);
  }
  if (player_menus) {
    FreeMenus(player_menus);
    player_menus = 0;
  }

  if (player_win) {
    CloseWindow(player_win);
    player_win = 0;
  }

  if (player_gadgets) {
    FreeGadgets(player_gadgets);
    player_gadgets = 0;
  }

  if (player_vi) {
    FreeVisualInfo(player_vi);
    player_vi = 0;
  }

  if (player_draw_info) {
    FreeScreenDrawInfo(player_screen, player_draw_info);
    player_draw_info = 0;
  }

  if (player_screen) {
    UnlockPubScreen(0, player_screen);
    player_screen = 0;
  }

  if (owns_workbench && WorkbenchBase) {
    CloseLibrary(WorkbenchBase);
    WorkbenchBase = 0;
    owns_workbench = 0;
  }

  if (owns_gadtools && GadToolsBase) {
    CloseLibrary(GadToolsBase);
    GadToolsBase = 0;
    owns_gadtools = 0;
  }

  if (owns_gfx && GfxBase) {
    CloseLibrary((struct Library *)GfxBase);
    GfxBase = 0;
    owns_gfx = 0;
  }

  gui_context = 0;
}

int zzplay_gui_is_open(void)
{
  return (player_win != 0) ? 1 : 0;
}

uint32_t zzplay_gui_signal_mask(void)
{
  uint32_t mask = 0U;

  if (player_win && player_win->UserPort) {
    mask |= (1UL << player_win->UserPort->mp_SigBit);
  }
  if (settings_win && settings_win->UserPort) {
    mask |= (1UL << settings_win->UserPort->mp_SigBit);
  }
  if (app_port) {
    mask |= (1UL << app_port->mp_SigBit);
  }
  mask |= zzplay_requester_signal_mask();
  return mask;
}

int zzplay_gui_redraw_deferred(void)
{
  if (!player_win || !gui_context) {
    return 0;
  }
  return (refresh_pending || settings_refresh_pending ||
          (gui_context->controller &&
           gui_context->controller->dirty != 0U))
             ? 1
             : 0;
}

int zzplay_gui_report(const char *message)
{
  if (!player_win || !gui_context || !gui_context->controller) {
    return 0;
  }
  zzplay_controller_set_message(gui_context->controller, message);
  return 1;
}

/* True when the port has a queued message. Reads the list directly: the
 * port's signal bit is not a reliable indicator, because the caller's
 * Wait() has already cleared it by the time we look. */
static int gui_port_pending(const struct MsgPort *port)
{
  return port && port->mp_MsgList.lh_Head->ln_Succ != 0;
}

void zzplay_gui_poll(void)
{
  ZZPlayController *ctl;
  ZZPlayPlaylist *pl;
  ZZPlayPrefs *prefs;
  ZZPlayModal modal_op;
  ZZPlayRequesterResult requester_result;
  struct IntuiMessage *imsg;
  struct AppMessage *amsg;
  ULONG last_event_s = 0UL;
  ULONG last_event_m = 0UL;

  if (!player_win || !gui_context) {
    return;
  }

  ctl = gui_context->controller;
  pl = gui_context->playlist;
  prefs = gui_context->prefs;
  if (zzplay_requester_take(&requester_result)) {
    apply_requester_result(ctl, pl, prefs, &requester_result);
    zzplay_requester_result_release(&requester_result);
  }
  /* Cheap when idle: this runs on every pass of the playback loops. It
   * must look at the message queues themselves, not the signal bits - the
   * idle loop's Wait() clears those, and testing them made every message
   * after the first sit unreplied forever (no quit, Intuition piling up
   * input events). */
  if (!gui_port_pending(player_win->UserPort) &&
      !(settings_win && gui_port_pending(settings_win->UserPort)) &&
      !gui_port_pending(app_port) && ctl->dirty == 0U && !refresh_pending &&
      !settings_refresh_pending &&
      zzplay_controller_modal_ready(ctl) == ZZPLAY_MODAL_NONE) {
    return;
  }

  /* 1. AppWindow drag-and-drop messages */
  if (app_port) {
    while ((amsg = (struct AppMessage *)GetMsg(app_port)) != 0) {
      int i;

      for (i = 0; i < amsg->am_NumArgs; i++) {
        struct WBArg *arg = &amsg->am_ArgList[i];

        if (zzplay_files_lock_path((long)arg->wa_Lock,
                                   (const char *)arg->wa_Name,
                                   drop_path_buf, sizeof(drop_path_buf))) {
          drop_queue_push(drop_path_buf);
        }
      }
      ReplyMsg((struct Message *)amsg);
    }
    if (drop_count != 0U && ctl->modal == ZZPLAY_MODAL_NONE) {
      (void)zzplay_controller_begin_modal(ctl, ZZPLAY_MODAL_ADD_DROPPED);
    }
  }

  /* 2. Settings window IDCMP. The loop body can close the settings window
   * (close gadget, Save, Use, Cancel), so re-check it before every
   * GT_GetIMsg. */
  if (settings_win && settings_win->UserPort) {
    while (settings_win &&
           (imsg = GT_GetIMsg(settings_win->UserPort)) != 0) {
      ULONG m_class = imsg->Class;
      UWORD m_code = imsg->Code;
      struct Gadget *gad = (struct Gadget *)imsg->IAddress;
      GT_ReplyIMsg(imsg);

      switch (m_class) {
        case IDCMP_CLOSEWINDOW:
          close_settings_window();
          break;

        case IDCMP_REFRESHWINDOW:
          /* Refreshed below once the screen's layers are free. */
          settings_refresh_pending = 1;
          break;

        case IDCMP_GADGETUP:
          if (gad) {
            switch (gad->GadgetID) {
              case SGID_MP3_OUTPUT:
                settings_copy.mp3_output = index_to_mp3_backend(m_code);
                break;
              case SGID_VIDEO_SOUND:
                settings_copy.video_audio = index_to_video_backend(m_code);
                break;
              case SGID_PRELOAD:
                settings_copy.preload = m_code ? 1U : 0U;
                break;
              case SGID_AHI_UNIT:
                settings_copy.ahi_unit = (uint32_t)m_code;
                break;
              case SGID_MHI_LIST:
                if ((uint32_t)m_code < mhi_count) {
                  strncpy(settings_copy.mhi_driver, mhi_clean_names[m_code],
                          ZZPLAY_PREFS_DRIVER_MAX - 1U);
                  settings_copy.mhi_driver[ZZPLAY_PREFS_DRIVER_MAX - 1U] = '\0';
                }
                break;
              case SGID_SAVE:
                apply_settings_device_fields(prefs, &settings_copy);
                {
                  int ok1 = zzplay_prefs_save(prefs, ZZPLAY_PREFS_ENVARC_PATH);
                  int ok2 = zzplay_prefs_save(prefs, ZZPLAY_PREFS_ENV_PATH);
                  if (!ok1 || !ok2) {
                    zzplay_controller_set_message(ctl, "Failed to save settings");
                  }
                }
                ctl->settings_changed = 1;
                close_settings_window();
                break;
              case SGID_USE:
                apply_settings_device_fields(prefs, &settings_copy);
                if (!zzplay_prefs_save(prefs, ZZPLAY_PREFS_ENV_PATH)) {
                  zzplay_controller_set_message(ctl, "Failed to save settings");
                }
                ctl->settings_changed = 1;
                close_settings_window();
                break;
              case SGID_CANCEL:
                close_settings_window();
                break;
              default:
                break;
            }
          }
          break;

        default:
          break;
      }
    }
  }
  /* A refresh deferred while the screen was locked, or one received just
   * above, is drawn once the screen's layers are free. */
  if (settings_win && settings_refresh_pending &&
      zzplay_gui_screen_free(settings_win)) {
    settings_refresh_pending = 0;
    GT_BeginRefresh(settings_win);
    GT_EndRefresh(settings_win, TRUE);
  }

  /* 3. Player window IDCMP */
  if (player_win && player_win->UserPort) {
    while ((imsg = GT_GetIMsg(player_win->UserPort)) != 0) {
      ULONG m_class = imsg->Class;
      UWORD m_code = imsg->Code;
      struct Gadget *gad = (struct Gadget *)imsg->IAddress;
      last_event_s = imsg->Seconds;
      last_event_m = imsg->Micros;

      switch (m_class) {
        case IDCMP_CLOSEWINDOW:
          GT_ReplyIMsg(imsg);
          request_quit(ctl);
          break;

        case IDCMP_VANILLAKEY:
          {
            ZZPlayControlAction act = zzplay_control_action_from_key((unsigned)m_code);
            GT_ReplyIMsg(imsg);
            if (act != ZZPLAY_CONTROL_NONE) {
              zzplay_controller_apply(ctl, act);
            }
          }
          break;

        case IDCMP_RAWKEY:
          {
            unsigned code = (unsigned)m_code;
            GT_ReplyIMsg(imsg);
            if ((code & 0x80U) == 0U) { /* Ignore key-up */
              ZZPlayControlAction act = zzplay_control_action_from_rawkey(code);
              if (act != ZZPLAY_CONTROL_NONE) {
                zzplay_controller_apply(ctl, act);
              }
            }
          }
          break;

        case IDCMP_GADGETDOWN:
          if (gad && gad->GadgetID == GID_POS_SLIDER) {
            pos_slider_dragging = 1;
          }
          GT_ReplyIMsg(imsg);
          break;

        case IDCMP_MOUSEMOVE:
          if (gad && gad->GadgetID == GID_POS_SLIDER) {
            pos_slider_dragging = 1;
          }
          GT_ReplyIMsg(imsg);
          break;

        case IDCMP_GADGETUP:
          GT_ReplyIMsg(imsg);
          if (gad) {
            switch (gad->GadgetID) {
              case GID_POS_SLIDER:
                pos_slider_dragging = 0;
                if (ctl->now.seekable && ctl->now.total_ms > 0U) {
                  uint32_t target_ms = (uint32_t)(((uint64_t)m_code * ctl->now.total_ms) / 1000ULL);
                  zzplay_controller_seek_to(ctl, target_ms);
                }
                break;

              case GID_PREV:
                zzplay_controller_apply(ctl, ZZPLAY_CONTROL_PREVIOUS);
                break;

              case GID_PLAY:
                if (ctl->item_active && ctl->paused) {
                  zzplay_controller_apply(ctl, ZZPLAY_CONTROL_TOGGLE_PAUSE);
                } else if (!ctl->item_active) {
                  zzplay_controller_request(ctl, ZZPLAY_REQUEST_PLAY);
                }
                break;

              case GID_PAUSE:
                if (ctl->item_active) {
                  zzplay_controller_apply(ctl, ZZPLAY_CONTROL_TOGGLE_PAUSE);
                }
                break;

              case GID_STOP:
                zzplay_controller_request(ctl, ZZPLAY_REQUEST_STOP);
                break;

              case GID_NEXT:
                zzplay_controller_apply(ctl, ZZPLAY_CONTROL_NEXT);
                break;

              case GID_VOL_SLIDER:
                {
                  uint32_t v = (uint32_t)m_code;
                  if (v > ZZPLAY_VOLUME_MAX) v = ZZPLAY_VOLUME_MAX;
                  zzplay_controller_set_volume(ctl, v);
                  if (prefs) prefs->volume = v;
                }
                break;

              case GID_REPEAT_CYCLE:
                {
                  ZZPlayRepeat rep = (ZZPlayRepeat)m_code;
                  zzplay_controller_set_repeat(ctl, rep);
                  zzplay_playlist_set_repeat(pl, rep);
                  if (prefs) prefs->repeat = rep;
                  update_menu_checks();
                }
                break;

              case GID_SHUFFLE_CHECK:
                {
                  int shuf = m_code ? 1 : 0;
                  uint32_t seed = (uint32_t)(last_event_s ^ (last_event_m << 10) ^ (uint32_t)VBeamPos());
                  if (seed == 0U) seed = 1U;
                  zzplay_controller_set_shuffle(ctl, shuf);
                  zzplay_playlist_set_shuffle(pl, shuf, seed);
                  if (prefs) prefs->shuffle = shuf;
                  update_menu_checks();
                }
                break;

              case GID_PLAYLIST_LIST:
                {
                  int32_t clicked = (int32_t)m_code;
                  if (clicked == last_click_row &&
                      DoubleClick(last_click_s, last_click_m, last_event_s, last_event_m)) {
                    zzplay_controller_jump(ctl, clicked);
                    last_click_row = -1;
                  } else {
                    last_click_row = clicked;
                    last_click_s = last_event_s;
                    last_click_m = last_event_m;
                    playlist_selected_row = clicked;
                  }
                }
                break;

              case GID_PL_ADD:
                start_requester(ZZPLAY_MODAL_ADD_FILES);
                break;

              case GID_PL_REMOVE:
                if (playlist_selected_row >= 0 &&
                    (uint32_t)playlist_selected_row < pl->count) {
                  zzplay_playlist_remove(pl, (uint32_t)playlist_selected_row);
                  if (playlist_selected_row >= (int32_t)pl->count) {
                    playlist_selected_row = (pl->count > 0U)
                                                ? (int32_t)(pl->count - 1U)
                                                : -1;
                  }
                  ctl->dirty |= ZZPLAY_DIRTY_PLAYLIST;
                }
                break;

              case GID_PL_CLEAR:
                zzplay_playlist_clear(pl);
                playlist_selected_row = -1;
                ctl->dirty |= ZZPLAY_DIRTY_PLAYLIST;
                break;

              case GID_PL_LOAD:
                start_requester(ZZPLAY_MODAL_LOAD_PLAYLIST);
                break;

              case GID_PL_SAVE:
                start_requester(ZZPLAY_MODAL_SAVE_PLAYLIST);
                break;

              default:
                break;
            }
          }
          break;

        case IDCMP_MENUPICK:
          {
            UWORD next_code = m_code;
            GT_ReplyIMsg(imsg);
            while (next_code != MENUNULL) {
              struct MenuItem *item = ItemAddress(player_menus, next_code);
              uint32_t mid = (uint32_t)GTMENUITEM_USERDATA(item);

              switch (mid) {
                case MID_OPEN:
                  start_requester(ZZPLAY_MODAL_OPEN_FILES);
                  break;
                case MID_ADD_FILES:
                  start_requester(ZZPLAY_MODAL_ADD_FILES);
                  break;
                case MID_ADD_DRAWER:
                  start_requester(ZZPLAY_MODAL_ADD_DRAWER);
                  break;
                case MID_LOAD_PLAYLIST:
                  start_requester(ZZPLAY_MODAL_LOAD_PLAYLIST);
                  break;
                case MID_SAVE_PLAYLIST:
                  start_requester(ZZPLAY_MODAL_SAVE_PLAYLIST);
                  break;
                case MID_SETTINGS:
                  open_settings_window();
                  break;
                case MID_ABOUT:
                  start_requester(ZZPLAY_MODAL_ABOUT);
                  break;
                case MID_QUIT:
                  request_quit(ctl);
                  break;

                case MID_PLAY_PAUSE:
                  zzplay_controller_apply(ctl, ZZPLAY_CONTROL_TOGGLE_PAUSE);
                  break;
                case MID_STOP:
                  zzplay_controller_request(ctl, ZZPLAY_REQUEST_STOP);
                  break;
                case MID_NEXT:
                  zzplay_controller_apply(ctl, ZZPLAY_CONTROL_NEXT);
                  break;
                case MID_PREVIOUS:
                  zzplay_controller_apply(ctl, ZZPLAY_CONTROL_PREVIOUS);
                  break;
                case MID_SEEK_FWD:
                  zzplay_controller_apply(ctl, ZZPLAY_CONTROL_SEEK_FORWARD);
                  break;
                case MID_SEEK_BACK:
                  zzplay_controller_apply(ctl, ZZPLAY_CONTROL_SEEK_BACK);
                  break;

                case MID_REPEAT_OFF:
                  zzplay_controller_set_repeat(ctl, ZZPLAY_REPEAT_OFF);
                  zzplay_playlist_set_repeat(pl, ZZPLAY_REPEAT_OFF);
                  if (prefs) prefs->repeat = ZZPLAY_REPEAT_OFF;
                  ctl->dirty |= ZZPLAY_DIRTY_STATE;
                  break;
                case MID_REPEAT_TRACK:
                  zzplay_controller_set_repeat(ctl, ZZPLAY_REPEAT_ONE);
                  zzplay_playlist_set_repeat(pl, ZZPLAY_REPEAT_ONE);
                  if (prefs) prefs->repeat = ZZPLAY_REPEAT_ONE;
                  ctl->dirty |= ZZPLAY_DIRTY_STATE;
                  break;
                case MID_REPEAT_ALL:
                  zzplay_controller_set_repeat(ctl, ZZPLAY_REPEAT_ALL);
                  zzplay_playlist_set_repeat(pl, ZZPLAY_REPEAT_ALL);
                  if (prefs) prefs->repeat = ZZPLAY_REPEAT_ALL;
                  ctl->dirty |= ZZPLAY_DIRTY_STATE;
                  break;

                case MID_SHUFFLE:
                  {
                    int shuf = (item->Flags & CHECKED) ? 1 : 0;
                    uint32_t seed = (uint32_t)(last_event_s ^ (last_event_m << 10) ^ (uint32_t)VBeamPos());
                    if (seed == 0U) seed = 1U;
                    zzplay_controller_set_shuffle(ctl, shuf);
                    zzplay_playlist_set_shuffle(pl, shuf, seed);
                    if (prefs) prefs->shuffle = shuf;
                    ctl->dirty |= ZZPLAY_DIRTY_STATE;
                  }
                  break;

                case MID_FULLSCREEN:
                  {
                    int full = (item->Flags & CHECKED) ? 1 : 0;
                    zzplay_controller_set_fullscreen(ctl, full);
                    if (prefs) prefs->fullscreen = full;
                  }
                  break;

                default:
                  break;
              }
              next_code = item->NextSelect;
            }
          }
          break;

        case IDCMP_REFRESHWINDOW:
          /* Refreshed below once the screen's layers are free. */
          refresh_pending = 1;
          GT_ReplyIMsg(imsg);
          break;

        default:
          GT_ReplyIMsg(imsg);
          break;
      }
    }
  }

  /* 4. Modal Gate execution */
  modal_op = zzplay_controller_modal_ready(ctl);
  if (modal_op != ZZPLAY_MODAL_NONE) {
    execute_modal_op(modal_op);
    zzplay_controller_end_modal(ctl);
  }
  /* Drops that arrived while another operation held the gate. */
  if (drop_count != 0U && ctl->modal == ZZPLAY_MODAL_NONE) {
    (void)zzplay_controller_begin_modal(ctl, ZZPLAY_MODAL_ADD_DROPPED);
  }

  /* 5. Redraw what changed, only while the screen's layers are free: a
   * menu or window drag holds them, and rendering would block this loop
   * and the sound with it. Whatever is not drawn stays dirty for a later
   * pass. */
  if ((ctl->dirty != 0U || refresh_pending) &&
      zzplay_gui_screen_free(player_win)) {
    uint32_t d = ctl->dirty;
    uint32_t undrawn = 0U;

    if (refresh_pending) {
      refresh_pending = 0;
      GT_BeginRefresh(player_win);
      if (!draw_info()) {
        undrawn |= ZZPLAY_DIRTY_INFO;
      }
      GT_EndRefresh(player_win, TRUE);
    }

    if (d & ZZPLAY_DIRTY_PLAYLIST) {
      rebuild_playlist_nodes();
    } else if (d & ZZPLAY_DIRTY_CURRENT) {
      update_current_playlist_node();
    }

    if (d & ZZPLAY_DIRTY_STATE) {
      /* Synchronize state into playlist and prefs */
      if (pl->repeat != ctl->repeat) {
        zzplay_playlist_set_repeat(pl, ctl->repeat);
      }
      if (pl->shuffle != ctl->shuffle) {
        uint32_t seed = (uint32_t)(last_event_s ^ (last_event_m << 10) ^ (uint32_t)VBeamPos());
        if (seed == 0U) seed = 1U;
        zzplay_playlist_set_shuffle(pl, ctl->shuffle, seed);
      }
      if (prefs) {
        prefs->repeat = ctl->repeat;
        prefs->shuffle = ctl->shuffle;
        prefs->volume = ctl->volume;
      }

      /* Update volume slider */
      if (ctl->item_active && !ctl->now.volume_supported) {
        GT_SetGadgetAttrs(vol_slider_gad, player_win, 0,
                          GA_Disabled, TRUE,
                          TAG_DONE);
      } else {
        GT_SetGadgetAttrs(vol_slider_gad, player_win, 0,
                          GA_Disabled, FALSE,
                          GTSL_Level, (ULONG)ctl->volume,
                          TAG_DONE);
      }

      /* Update cycle and checkbox */
      GT_SetGadgetAttrs(repeat_gad, player_win, 0,
                        GTCY_Active, (ULONG)ctl->repeat,
                        TAG_DONE);
      GT_SetGadgetAttrs(shuffle_gad, player_win, 0,
                        GTCB_Checked, ctl->shuffle ? TRUE : FALSE,
                        TAG_DONE);

      update_menu_checks();
    }

    if ((d & ZZPLAY_DIRTY_POSITION) && !pos_slider_dragging) {
      if (ctl->now.seekable && ctl->now.total_ms > 0U) {
        uint32_t level = (uint32_t)(((uint64_t)ctl->now.elapsed_ms * 1000ULL) /
                                    ctl->now.total_ms);
        if (level > 1000U) level = 1000U;
        if (pos_slider_last_disabled != 0 || pos_slider_last_level != level) {
          GT_SetGadgetAttrs(pos_slider_gad, player_win, 0,
                            GA_Disabled, FALSE, GTSL_Level, (ULONG)level,
                            TAG_DONE);
          pos_slider_last_disabled = 0;
          pos_slider_last_level = level;
        }
      } else if (pos_slider_last_disabled != 1 ||
                 pos_slider_last_level != 0UL) {
        GT_SetGadgetAttrs(pos_slider_gad, player_win, 0,
                          GA_Disabled, TRUE, GTSL_Level, 0UL, TAG_DONE);
        pos_slider_last_disabled = 1;
        pos_slider_last_level = 0UL;
      }
    }

    if (d & (ZZPLAY_DIRTY_INFO | ZZPLAY_DIRTY_STATE)) {
      if (!draw_info()) {
        undrawn |= ZZPLAY_DIRTY_INFO;
      }
    } else if (d & ZZPLAY_DIRTY_POSITION) {
      if (!draw_position_info()) {
        undrawn |= ZZPLAY_DIRTY_POSITION;
      }
    }

    ctl->dirty = undrawn;
  }
}
