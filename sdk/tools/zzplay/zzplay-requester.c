/* Asynchronous ASL and About requesters for ZZPlay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-requester.h"

#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/dos.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <graphics/gfxbase.h>
#include <libraries/asl.h>
#include <workbench/startup.h>

#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>

#include <string.h>

#include "zzplay-formats.h"
#include "zzplay-files.h"

extern struct Library *AslBase;

enum ZZPlayRequesterState {
  ZZPLAY_REQUESTER_IDLE = 0,
  ZZPLAY_REQUESTER_RUNNING,
  ZZPLAY_REQUESTER_DONE
};

typedef struct ZZPlayRequesterStateData {
  volatile int state;
  struct Task *parent;
  ULONG signal_mask;
  struct Screen *screen;
  char initial_drawer[ZZPLAY_PLAYLIST_PATH_MAX];
  char version[96];
  ZZPlayRequesterResult result;
  /* Save writes this parent-side snapshot in the requester task. */
  ZZPlayPlaylist save_snapshot;
} ZZPlayRequesterStateData;

static ZZPlayRequesterStateData requester;
static BYTE requester_signal = -1;

static void requester_copy(char *dst, size_t size, const char *src)
{
  if (!src) {
    src = "";
  }
  strncpy(dst, src, size - 1U);
  dst[size - 1U] = '\0';
}

static int requester_add_path(const char *path)
{
  char *copy;
  uint32_t index = requester.result.path_count;

  if (!path || path[0] == '\0' || !requester.result.paths) {
    return 0;
  }
  copy = (char *)AllocVec((ULONG)strlen(path) + 1UL, MEMF_PUBLIC);
  if (!copy) {
    return 0;
  }
  strcpy(copy, path);
  requester.result.paths[index] = copy;
  requester.result.path_count = index + 1U;
  return 1;
}

static int requester_collect_files(struct FileRequester *fr)
{
  char path[ZZPLAY_PLAYLIST_PATH_MAX];
  int i;

  if (fr->fr_NumArgs > 0) {
    requester.result.paths = (char **)AllocVec(
        (ULONG)fr->fr_NumArgs * (ULONG)sizeof(char *), MEMF_PUBLIC | MEMF_CLEAR);
    if (!requester.result.paths) {
      return 0;
    }
    for (i = 0; i < fr->fr_NumArgs; i++) {
      struct WBArg *arg = &fr->fr_ArgList[i];

      path[0] = '\0';
      if (arg->wa_Lock) {
        NameFromLock(arg->wa_Lock, (STRPTR)path, sizeof(path));
      }
      if (path[0] == '\0') {
        requester_copy(path, sizeof(path), (const char *)fr->fr_Drawer);
      }
      if (arg->wa_Name && arg->wa_Name[0] != '\0') {
        size_t len = strlen(path);
        if (len != 0U && path[len - 1U] != ':' && path[len - 1U] != '/') {
          strncat(path, "/", sizeof(path) - len - 1U);
        }
        strncat(path, (const char *)arg->wa_Name,
                sizeof(path) - strlen(path) - 1U);
      }
      if (!requester_add_path(path)) {
        return 0;
      }
    }
  } else if (fr->fr_File && fr->fr_File[0] != '\0') {
    requester_copy(path, sizeof(path), (const char *)fr->fr_Drawer);
    if (path[0] != '\0' && path[strlen(path) - 1U] != ':' &&
        path[strlen(path) - 1U] != '/') {
      strncat(path, "/", sizeof(path) - strlen(path) - 1U);
    }
    strncat(path, (const char *)fr->fr_File, sizeof(path) - strlen(path) - 1U);
    requester.result.paths = (char **)AllocVec(sizeof(char *),
                                                 MEMF_PUBLIC | MEMF_CLEAR);
    if (!requester.result.paths || !requester_add_path(path)) {
      return 0;
    }
  }
  return 1;
}

static void requester_paths_free(void)
{
  uint32_t i;

  for (i = 0U; i < requester.result.path_count; i++) {
    FreeVec(requester.result.paths[i]);
  }
  if (requester.result.paths) {
    FreeVec(requester.result.paths);
  }
  requester.result.paths = 0;
  requester.result.path_count = 0U;
}
static void requester_fail(const char *message)
{
  requester.result.accepted = 0;
  requester_copy(requester.result.error, sizeof(requester.result.error),
                 message);
  requester_paths_free();
  zzplay_playlist_free(&requester.result.prepared);
}

static int requester_copy_playlist(ZZPlayPlaylist *dst,
                                   const ZZPlayPlaylist *src)
{
  uint32_t i;

  zzplay_playlist_init(dst);
  if (!src) {
    return 1;
  }
  for (i = 0U; i < src->count; i++) {
    const ZZPlayPlaylistEntry *entry = &src->entries[i];
    int32_t index = zzplay_playlist_add(dst, entry->path);

    if (index < 0) {
      zzplay_playlist_free(dst);
      return 0;
    }
    zzplay_playlist_set_title(dst, (uint32_t)index, entry->title);
    dst->entries[index].duration_ms = entry->duration_ms;
  }
  return 1;
}

static int requester_prepare_selection(void)
{
  uint32_t i;

  zzplay_playlist_init(&requester.result.prepared);
  if (requester.result.op == ZZPLAY_MODAL_ADD_DRAWER) {
    zzplay_files_add(&requester.result.prepared, requester.result.drawer);
  } else {
    for (i = 0U; i < requester.result.path_count; i++) {
      if (requester.result.op == ZZPLAY_MODAL_LOAD_PLAYLIST) {
        /* Loading explicitly selected playlists must not depend on their
         * suffix: parse every selected file as M3U. */
        zzplay_playlist_load_m3u(&requester.result.prepared,
                                 requester.result.paths[i]);
      } else {
        zzplay_files_add(&requester.result.prepared, requester.result.paths[i]);
      }
      if (requester.result.prepared.allocation_failed) {
        break;
      }
    }
  }
  requester_paths_free();
  if (requester.result.prepared.allocation_failed) {
    zzplay_playlist_free(&requester.result.prepared);
    return 0;
  }
  return 1;
}

static void requester_save_snapshot(void)
{
  char path[ZZPLAY_PLAYLIST_PATH_MAX];

  if (requester.result.path_count == 0U) {
    return;
  }
  requester_copy(path, sizeof(path), requester.result.paths[0]);
  if (!zzplay_playlist_is_playlist_path(path)) {
    strncat(path, ".m3u", sizeof(path) - strlen(path) - 1U);
  }
  requester.result.saved = zzplay_playlist_save_m3u(&requester.save_snapshot,
                                                     path);
  requester_paths_free();
}
static void requester_show_about(void)
{
  struct EasyStruct es;
  char text[512];
  size_t count;
  size_t i;

  requester_copy(text, sizeof(text),
                 requester.version[0] ? requester.version : "ZZPlay");
  strncat(text, "\n\nRegistered formats:\n",
          sizeof(text) - strlen(text) - 1U);
  count = zzplay_format_count();
  for (i = 0U; i < count; i++) {
    const ZZPlayFormat *fmt = zzplay_format_at(i);
    if (fmt && fmt->name) {
      strncat(text, "  ", sizeof(text) - strlen(text) - 1U);
      strncat(text, fmt->name, sizeof(text) - strlen(text) - 1U);
      strncat(text, "\n", sizeof(text) - strlen(text) - 1U);
    }
  }
  memset(&es, 0, sizeof(es));
  es.es_StructSize = sizeof(es);
  es.es_Title = (CONST_STRPTR)"About ZZPlay";
  es.es_TextFormat = (CONST_STRPTR)text;
  es.es_GadgetFormat = (CONST_STRPTR)"OK";
  EasyRequestArgs(0, &es, 0, 0);
}

static struct FileRequester *requester_alloc_file_request(void)
{
  switch (requester.result.op) {
  case ZZPLAY_MODAL_OPEN_FILES:
  case ZZPLAY_MODAL_ADD_FILES:
    {
      char pattern[128];
      zzplay_formats_pattern(pattern, sizeof(pattern), 1);
      return (struct FileRequester *)AllocAslRequestTags(
          ASL_FileRequest,
          ASLFR_Screen, (ULONG)requester.screen,
          ASLFR_TitleText,
          (ULONG)(requester.result.op == ZZPLAY_MODAL_OPEN_FILES ? "Open File(s)"
                                                                  : "Add File(s)"),
          ASLFR_InitialDrawer, (ULONG)requester.initial_drawer,
          ASLFR_InitialPattern, (ULONG)pattern,
          ASLFR_DoPatterns, TRUE,
          ASLFR_DoMultiSelect, TRUE,
          TAG_DONE);
    }
  case ZZPLAY_MODAL_ADD_DRAWER:
    return (struct FileRequester *)AllocAslRequestTags(
        ASL_FileRequest, ASLFR_Screen, (ULONG)requester.screen,
        ASLFR_TitleText, (ULONG)"Select Drawer",
        ASLFR_InitialDrawer, (ULONG)requester.initial_drawer,
        ASLFR_DrawersOnly, TRUE, TAG_DONE);
  case ZZPLAY_MODAL_LOAD_PLAYLIST:
    return (struct FileRequester *)AllocAslRequestTags(
        ASL_FileRequest, ASLFR_Screen, (ULONG)requester.screen,
        ASLFR_TitleText, (ULONG)"Load Playlist",
        ASLFR_InitialDrawer, (ULONG)requester.initial_drawer,
        ASLFR_InitialPattern, (ULONG)"#?.(m3u|m3u8)",
        ASLFR_DoPatterns, TRUE, TAG_DONE);
  case ZZPLAY_MODAL_SAVE_PLAYLIST:
    return (struct FileRequester *)AllocAslRequestTags(
        ASL_FileRequest, ASLFR_Screen, (ULONG)requester.screen,
        ASLFR_TitleText, (ULONG)"Save Playlist",
        ASLFR_InitialDrawer, (ULONG)requester.initial_drawer,
        ASLFR_DoSaveMode, TRUE, TAG_DONE);
  default:
    return 0;
  }
}

static void requester_process(void)
{
  struct FileRequester *fr = 0;

  if (requester.result.op == ZZPLAY_MODAL_ABOUT) {
    requester_show_about();
    goto done;
  }

  fr = requester_alloc_file_request();
  if (fr) {
    requester.result.accepted = AslRequest(fr, 0) ? 1 : 0;
    if (requester.result.accepted) {
      int collected = 1;

      requester_copy(requester.result.drawer, sizeof(requester.result.drawer),
                     (const char *)fr->fr_Drawer);
      if (requester.result.op == ZZPLAY_MODAL_OPEN_FILES ||
          requester.result.op == ZZPLAY_MODAL_ADD_FILES ||
          (requester.result.op != ZZPLAY_MODAL_ADD_DRAWER &&
           fr->fr_File && fr->fr_File[0] != '\0')) {
        collected = requester_collect_files(fr);
      }
      if (!collected) {
        requester_fail("Not enough memory to prepare selection");
      } else if (requester.result.op == ZZPLAY_MODAL_SAVE_PLAYLIST) {
        requester_save_snapshot();
      } else if (!requester_prepare_selection()) {
        requester_fail("Not enough memory to prepare selection");
      }
    }
    FreeAslRequest(fr);
  }

done:
  /* Signal as the final application-code action. The child exits through DOS
   * while still forbidden, so the parent cannot unload this segment first. */
  Forbid();
  requester.state = ZZPLAY_REQUESTER_DONE;
  Signal(requester.parent, requester.signal_mask);
}

int zzplay_requester_start(ZZPlayModal op, const char *initial_drawer,
                           const char *version, const ZZPlayPlaylist *playlist,
                           struct Screen *screen)
{
  struct Process *process;

  if (op == ZZPLAY_MODAL_NONE || op == ZZPLAY_MODAL_ADD_DROPPED ||
      requester.state != ZZPLAY_REQUESTER_IDLE) {
    return 0;
  }
  if (op != ZZPLAY_MODAL_ABOUT && !AslBase) {
    AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 37U);
    if (!AslBase) {
      return 0;
    }
  }
  if (requester_signal < 0) {
    requester_signal = AllocSignal(-1L);
    if (requester_signal < 0) {
      return 0;
    }
  }

  memset(&requester.result, 0, sizeof(requester.result));
  zzplay_playlist_init(&requester.save_snapshot);
  if (op == ZZPLAY_MODAL_SAVE_PLAYLIST &&
      !requester_copy_playlist(&requester.save_snapshot, playlist)) {
    return 0;
  }
  requester.result.op = op;
  requester_copy(requester.initial_drawer, sizeof(requester.initial_drawer),
                 initial_drawer);
  requester_copy(requester.version, sizeof(requester.version), version);
  requester.screen = screen;
  requester.parent = FindTask(0);
  requester.signal_mask = 1UL << (ULONG)requester_signal;

  Forbid();
  requester.state = ZZPLAY_REQUESTER_RUNNING;
  process = CreateNewProcTags(NP_Entry, (ULONG)requester_process,
                              NP_Name, (ULONG)"ZZPlay requester",
                              NP_StackSize, 16384UL,
                              TAG_DONE);
  if (!process) {
    requester.state = ZZPLAY_REQUESTER_IDLE;
    zzplay_playlist_free(&requester.save_snapshot);
  }
  Permit();
  return process ? 1 : 0;
}

int zzplay_requester_active(void)
{
  return requester.state == ZZPLAY_REQUESTER_RUNNING;
}

uint32_t zzplay_requester_signal_mask(void)
{
  return requester_signal >= 0 ? requester.signal_mask : 0U;
}

int zzplay_requester_take(ZZPlayRequesterResult *result)
{
  if (!result || requester.state != ZZPLAY_REQUESTER_DONE) return 0;
  Forbid();
  *result = requester.result;
  memset(&requester.result, 0, sizeof(requester.result));
  requester.state = ZZPLAY_REQUESTER_IDLE;
  Permit();
  zzplay_playlist_free(&requester.save_snapshot);
  return 1;
}

void zzplay_requester_result_release(ZZPlayRequesterResult *result)
{
  uint32_t i;

  if (!result) {
    return;
  }
  for (i = 0U; i < result->path_count; i++) {
    FreeVec(result->paths[i]);
  }
  FreeVec(result->paths);
  zzplay_playlist_free(&result->prepared);
  memset(result, 0, sizeof(*result));
}

void zzplay_requester_close(void)
{
  ZZPlayRequesterResult result;

  while (requester.state == ZZPLAY_REQUESTER_RUNNING) {
    Wait(requester.signal_mask);
  }
  if (zzplay_requester_take(&result)) {
    zzplay_requester_result_release(&result);
  }
  if (requester_signal >= 0) {
    FreeSignal(requester_signal);
    requester_signal = -1;
  }
}
