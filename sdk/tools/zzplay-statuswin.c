/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-statuswin.h"

#include <exec/types.h>
#include <intuition/intuition.h>

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <stdio.h>
#include <string.h>

/* Text() and RectFill() resolve through this global, so open it here rather
 * than assume a -noixemul startup provided one. */
struct GfxBase *GfxBase;
static int zzplay_statuswin_owns_gfx;

#define ZZPLAY_STATUSWIN_WIDTH 384
/* Five text lines plus the position bar. The pixel height is computed from
 * the screen font at open time, never assumed. */
#define ZZPLAY_STATUSWIN_LINES 6
#define ZZPLAY_STATUSWIN_BAR 4

/* Keep only the final path component so the display stays readable. */
static const char *zzplay_statuswin_basename(const char *path)
{
  const char *last = path;
  const char *scan;

  if (!path) {
    return "";
  }
  for (scan = path; *scan; scan++) {
    if (*scan == '/' || *scan == ':') {
      last = scan + 1;
    }
  }
  return last;
}

static void zzplay_statuswin_time(char *out, uint32_t ms)
{
  uint32_t seconds = ms / 1000U;

  sprintf(out, "%lu:%02lu", (unsigned long)(seconds / 60U),
          (unsigned long)(seconds % 60U));
}

/* Baseline of text line `index`, measured from the top of the inner area. */
static int zzplay_statuswin_line_y(const ZZPlayStatusWindow *status,
                                   int index)
{
  return index * status->line_height + status->baseline;
}

static void zzplay_statuswin_line(ZZPlayStatusWindow *status, int index,
                                  const char *text)
{
  struct Window *window = (struct Window *)status->window;
  struct RastPort *rp = window->RPort;
  int x = window->BorderLeft + status->text_left;
  int y = window->BorderTop + 2 + zzplay_statuswin_line_y(status, index);

  SetAPen(rp, 1U);
  SetBPen(rp, 0U);
  SetDrMd(rp, JAM2);
  Move(rp, (WORD)x, (WORD)y);
  Text(rp, (CONST_STRPTR)text, (ULONG)strlen(text));
}

static void zzplay_statuswin_draw(ZZPlayStatusWindow *status)
{
  struct Window *window = (struct Window *)status->window;
  struct RastPort *rp;
  char line[96];
  char elapsed[16];
  char total[16];
  int left;
  int right;
  int top;

  if (!window || !GfxBase) {
    return;
  }
  rp = window->RPort;
  left = window->BorderLeft;
  top = window->BorderTop;
  right = window->Width - window->BorderRight;

  /* Clear the interior rather than the whole window, so the borders and
   * gadgets Intuition drew are left alone. */
  SetAPen(rp, 0U);
  RectFill(rp, (WORD)left, (WORD)top,
           (WORD)(right - 1),
           (WORD)(window->Height - window->BorderBottom - 1));

  sprintf(line, "%.60s", status->name);
  zzplay_statuswin_line(status, 0, line);

  if (status->sample_rate != 0U) {
    sprintf(line, "%lu Hz  %s  %lu kbps",
            (unsigned long)status->sample_rate,
            status->channels == 1U ? "mono" : "stereo",
            (unsigned long)status->bitrate_kbps);
  } else {
    strcpy(line, "");
  }
  zzplay_statuswin_line(status, 1, line);

  sprintf(line, "Output: %s", status->backend[0] ? status->backend
                                                 : "selecting...");
  zzplay_statuswin_line(status, 2, line);

  zzplay_statuswin_time(elapsed, status->elapsed_ms);
  if (status->total_ms != 0U) {
    zzplay_statuswin_time(total, status->total_ms);
    sprintf(line, "%s%s / %s   %s%s", status->position_exact ? "" : "~",
            elapsed, total, status->paused ? "PAUSED" : "playing",
            status->loop ? "  loop" : "");
  } else {
    sprintf(line, "%s%s   %s%s", status->position_exact ? "" : "~",
            elapsed, status->paused ? "PAUSED" : "playing",
            status->loop ? "  loop" : "");
  }
  zzplay_statuswin_line(status, 3, line);

  /* Position bar. Only meaningful when the duration is known. */
  if (status->total_ms != 0U) {
    int bar_left = left + status->text_left;
    int bar_right = right - status->text_left - 1;
    int bar_y = top + 2 + 4 * status->line_height +
                (status->line_height - ZZPLAY_STATUSWIN_BAR) / 2;
    int span = bar_right - bar_left;
    uint32_t done = status->elapsed_ms;

    if (span > 0) {
      int filled;

      if (done > status->total_ms) {
        done = status->total_ms;
      }
      filled = (int)(((uint32_t)span * done) / status->total_ms);
      SetAPen(rp, 2U);
      RectFill(rp, (WORD)bar_left, (WORD)bar_y,
               (WORD)(bar_right),
               (WORD)(bar_y + ZZPLAY_STATUSWIN_BAR - 1));
      if (filled > 0) {
        SetAPen(rp, 3U);
        RectFill(rp, (WORD)bar_left, (WORD)bar_y,
                 (WORD)(bar_left + filled),
                 (WORD)(bar_y + ZZPLAY_STATUSWIN_BAR - 1));
      }
    }
  }

  zzplay_statuswin_line(status, 5, "Space pause   Q stop   L loop");
  status->dirty = 0;
}

static void zzplay_statuswin_retitle(ZZPlayStatusWindow *status)
{
  struct Window *window = (struct Window *)status->window;
  char previous[sizeof(status->title)];

  if (!window) {
    return;
  }
  strcpy(previous, status->title);
  sprintf(status->title, "ZZPlay - %s - %s%s",
          status->backend[0] ? status->backend : "...",
          status->paused ? "paused" : "playing",
          status->loop ? " - loop" : "");
  if (strcmp(previous, status->title) != 0) {
    SetWindowTitles(window, (CONST_STRPTR)status->title,
                    (CONST_STRPTR)~0UL);
  }
}

uint32_t zzplay_statuswin_duration_ms(const char *path,
                                      uint32_t bitrate_kbps)
{
  FILE *file;
  long length;

  if (!path || bitrate_kbps == 0U) {
    return 0U;
  }
  file = fopen(path, "rb");
  if (!file) {
    return 0U;
  }
  if (fseek(file, 0L, SEEK_END) != 0 || (length = ftell(file)) <= 0L) {
    fclose(file);
    return 0U;
  }
  fclose(file);
  /* CBR estimate: bytes * 8 / kbps gives milliseconds directly. A VBR file
   * makes this an approximation, which is why the position is shown with a
   * leading '~' whenever the source of the position is itself inexact. */
  return (uint32_t)(((uint64_t)length * 8ULL) / bitrate_kbps);
}

int zzplay_statuswin_open(ZZPlayStatusWindow *status, const char *path,
                          uint32_t sample_rate, uint32_t channels,
                          uint32_t bitrate_kbps, uint32_t total_ms)
{
  struct Window *window;
  int inner_height;

  if (!status) {
    return 0;
  }
  memset(status, 0, sizeof(*status));
  if (!IntuitionBase) {
    return 0;
  }
  if (!GfxBase) {
    GfxBase = (struct GfxBase *)OpenLibrary(
        (CONST_STRPTR)"graphics.library", 37U);
    zzplay_statuswin_owns_gfx = GfxBase ? 1 : 0;
  }
  strncpy(status->name, zzplay_statuswin_basename(path),
          sizeof(status->name) - 1U);
  status->name[sizeof(status->name) - 1U] = '\0';
  status->sample_rate = sample_rate;
  status->channels = channels;
  status->bitrate_kbps = bitrate_kbps;
  status->total_ms = total_ms;
  status->position_exact = 1;
  strcpy(status->title, "ZZPlay");
  /* Size from the screen's actual font. A hardcoded 10-pixel line clipped
   * the descenders of a taller Workbench font by a pixel or two. */
  status->line_height = 10;
  status->baseline = 8;
  status->text_left = 4;
  {
    struct Screen *screen = LockPubScreen(0);

    if (screen) {
      struct TextFont *font = screen->RastPort.Font;

      if (font && font->tf_YSize > 0) {
        status->line_height = (int)font->tf_YSize + 2;
        status->baseline = (int)font->tf_Baseline + 1;
      }
      UnlockPubScreen(0, screen);
    }
  }
  inner_height = ZZPLAY_STATUSWIN_LINES * status->line_height + 4;
  window = OpenWindowTags(
      0,
      WA_Title, (ULONG)status->title,
      WA_InnerWidth, (ULONG)ZZPLAY_STATUSWIN_WIDTH,
      WA_InnerHeight, (ULONG)inner_height,
      WA_PubScreenName, (ULONG)"Workbench",
      WA_Activate, TRUE,
      WA_DragBar, TRUE,
      WA_CloseGadget, TRUE,
      WA_DepthGadget, TRUE,
      WA_SimpleRefresh, TRUE,
      WA_IDCMP,
      IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_REFRESHWINDOW,
      TAG_DONE);
  if (!window) {
    return 0;
  }
  status->window = window;
  status->dirty = 1;
  zzplay_statuswin_draw(status);
  return 1;
}

void zzplay_statuswin_close(ZZPlayStatusWindow *status)
{
  if (!status || !status->window) {
    return;
  }
  CloseWindow((struct Window *)status->window);
  status->window = 0;
  if (zzplay_statuswin_owns_gfx && GfxBase) {
    CloseLibrary((struct Library *)GfxBase);
    GfxBase = 0;
    zzplay_statuswin_owns_gfx = 0;
  }
}

ZZPlayControlAction zzplay_statuswin_poll(ZZPlayStatusWindow *status)
{
  struct Window *window;
  struct IntuiMessage *message;
  ZZPlayControlInput input;
  ZZPlayControlAction action;
  int refresh = 0;

  if (!status) {
    return ZZPLAY_CONTROL_NONE;
  }
  memset(&input, 0, sizeof(input));
  /* Consume the break here for the same reason the video path does: a
   * pending CTRL-C would otherwise surface inside a later stdio call and
   * bypass the normal teardown. */
  input.ctrl_c = (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0U;
  window = (struct Window *)status->window;
  while (window &&
         (message = (struct IntuiMessage *)GetMsg(window->UserPort))) {
    if (message->Class == IDCMP_CLOSEWINDOW) {
      input.window_close = 1;
    } else if (message->Class == IDCMP_VANILLAKEY) {
      ZZPlayControlAction key_action =
          zzplay_control_action_from_key((unsigned)message->Code);

      if (key_action != ZZPLAY_CONTROL_NONE && input.key == 0U) {
        input.key = (unsigned)message->Code;
      }
    } else if (message->Class == IDCMP_REFRESHWINDOW) {
      refresh = 1;
    }
    ReplyMsg((struct Message *)message);
  }
  /* A SimpleRefresh window must acknowledge the refresh even if it redraws
   * nothing, or Intuition keeps resending it. */
  if (refresh && window) {
    BeginRefresh(window);
    EndRefresh(window, TRUE);
    status->dirty = 1;
  }
  action = zzplay_control_resolve(&input);
  if (zzplay_control_is_stop(action)) {
    status->stop = 1;
  } else if (action == ZZPLAY_CONTROL_TOGGLE_PAUSE) {
    status->paused = !status->paused;
    status->dirty = 1;
  } else if (action == ZZPLAY_CONTROL_TOGGLE_LOOP) {
    status->loop = !status->loop;
    status->dirty = 1;
  }
  /* Fullscreen is meaningless without video; ignore it rather than
   * pretending it did something. */
  if (status->dirty) {
    zzplay_statuswin_retitle(status);
    zzplay_statuswin_draw(status);
  }
  return action;
}

void zzplay_statuswin_set_backend(ZZPlayStatusWindow *status,
                                  const char *backend)
{
  if (!status || !backend) {
    return;
  }
  strncpy(status->backend, backend, sizeof(status->backend) - 1U);
  status->backend[sizeof(status->backend) - 1U] = '\0';
  status->dirty = 1;
  zzplay_statuswin_draw(status);
}

void zzplay_statuswin_set_position(ZZPlayStatusWindow *status,
                                   uint32_t elapsed_ms, int exact)
{
  if (!status) {
    return;
  }
  status->position_exact = exact;
  /* Redraw only when the displayed second actually changes: this is called
   * from the decode loop and repainting every pass would waste the 68k. */
  if (elapsed_ms / 1000U == status->elapsed_ms / 1000U &&
      status->elapsed_ms != 0U) {
    status->elapsed_ms = elapsed_ms;
    return;
  }
  status->elapsed_ms = elapsed_ms;
  status->dirty = 1;
  zzplay_statuswin_draw(status);
}
