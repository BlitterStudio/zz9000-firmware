/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-launch.h"

#include "zzplay-controller.h"
#include "zzplay-gui.h"

#include <dos/dos.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <intuition/intuition.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/icon.h>
#include <proto/intuition.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct Library *IconBase;
struct Library *AslBase;
/* proto/intuition.h resolves EasyRequestArgs() through this exact global, so
 * it must be the one we open — a private handle would not be used by the
 * call. A -noixemul startup does not provide it. The player window may
 * open asl.library into AslBase later; launch_end closes whatever is
 * there. */
struct IntuitionBase *IntuitionBase;
static int zzplay_launch_owns_intuition;

/* The player calls Intuition directly (window title, resize, screen size) on
 * every launch path, not only the Workbench error path, so open it once up
 * front and let every caller rely on it. */
static void zzplay_launch_open_intuition(void)
{
  if (IntuitionBase) {
    return;
  }
  IntuitionBase = (struct IntuitionBase *)OpenLibrary(
      (CONST_STRPTR)"intuition.library", 37U);
  zzplay_launch_owns_intuition = IntuitionBase ? 1 : 0;
}

/* CurrentDir() must be moved to reach an icon by name, but the original must
 * be restored exactly once no matter how many locks we visit. */
static void zzplay_launch_set_dir(ZZPlayLaunch *launch, BPTR lock)
{
  BPTR previous;

  if (!launch || !lock) {
    return;
  }
  previous = CurrentDir(lock);
  if (launch->old_directory == -1) {
    launch->old_directory = (long)previous;
  }
}

/* Resolve one Workbench argument into a full path in storage owned by the
 * launch. The 68k build has no snprintf guarantees worth relying on, and
 * NameFromLock is the documented way to turn a WBArg lock into text. */
static char *zzplay_launch_path(BPTR lock, const char *name)
{
  char *path;

  if (!name || !*name) {
    return 0;
  }
  path = (char *)AllocVec((ULONG)ZZPLAY_LAUNCH_PATH_MAX,
                          MEMF_PUBLIC | MEMF_CLEAR);
  if (!path) {
    return 0;
  }
  if (lock) {
    if (!NameFromLock(lock, (STRPTR)path,
                      (LONG)ZZPLAY_LAUNCH_PATH_MAX) ||
        !AddPart((STRPTR)path, (CONST_STRPTR)name,
                 (ULONG)ZZPLAY_LAUNCH_PATH_MAX)) {
      FreeVec(path);
      return 0;
    }
    return path;
  }
  if (strlen(name) >= ZZPLAY_LAUNCH_PATH_MAX) {
    FreeVec(path);
    return 0;
  }
  strcpy(path, name);
  return path;
}

static void zzplay_launch_apply_tooltypes(ZZPlayOptions *options,
                                          struct DiskObject *icon,
                                          int *bad_tooltype)
{
  STRPTR *tooltypes;
  unsigned i;

  if (!icon || !icon->do_ToolTypes) {
    return;
  }
  tooltypes = (STRPTR *)icon->do_ToolTypes;
  for (i = 0U; tooltypes[i]; i++) {
    if (!zzplay_options_apply_tooltype(options, (const char *)tooltypes[i])) {
      if (bad_tooltype) {
        *bad_tooltype = 1;
      }
    }
  }
}

void zzplay_launch_report(const ZZPlayOptions *options, const char *message)
{
  if (!message) {
    return;
  }
  if (options && options->launch == ZZPLAY_LAUNCH_WORKBENCH) {
    struct EasyStruct easy;

    zzplay_launch_open_intuition();
    if (!IntuitionBase) {
      return;
    }
    memset(&easy, 0, sizeof(easy));
    easy.es_StructSize = sizeof(easy);
    easy.es_Title = (STRPTR)"ZZPlay";
    easy.es_TextFormat = (STRPTR)"%s";
    easy.es_GadgetFormat = (STRPTR)"Continue";
    (void)EasyRequestArgs(0, &easy, 0, (APTR)&message);
    return;
  }
  fprintf(stderr, "zzplay: %s\n", message);
}

void zzplay_launch_report_surface(const ZZPlayOptions *options,
                                  const char *message)
{
  if (!message) {
    return;
  }
  /* The player window's message line is the desktop player's error
   * surface; only a headless run needs the console/requester routing. */
  if (zzplay_options_wants_player(options) && zzplay_gui_is_open() &&
      zzplay_gui_report(message)) {
    return;
  }
  zzplay_launch_report(options, message);
}

void zzplay_launch_reportf(ZZPlayController *controller,
                           const ZZPlayOptions *options,
                           const char *format, ...)
{
  static char message[512];
  va_list args;

  va_start(args, format);
  /* Messages carry user paths of any length: truncate, never overrun. */
  (void)vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (controller) {
    zzplay_controller_set_message(controller, message);
  }
  zzplay_launch_report_surface(options, message);
}

ZZPlayOptionsResult zzplay_launch_begin(int argc, char **argv,
                                        ZZPlayOptions *options,
                                        ZZPlayLaunch *launch)
{
  struct WBStartup *startup;
  struct WBArg *args;
  struct DiskObject *icon;
  int bad_tooltype = 0;
  unsigned i;

  if (!options || !launch) {
    return ZZPLAY_OPTIONS_ERROR;
  }
  memset(launch, 0, sizeof(*launch));
  launch->old_directory = -1;
  zzplay_launch_open_intuition();

  /* A CLI launch keeps the existing behaviour: options and file arguments
   * may be interleaved, and the parsed paths borrow argv storage. */
  if (argc != 0) {
    return zzplay_options_parse_cli(argc, argv, options);
  }

  zzplay_options_init(options, ZZPLAY_LAUNCH_WORKBENCH);
  startup = (struct WBStartup *)argv;
  launch->startup = startup;
  if (!startup || startup->sm_NumArgs < 1) {
    return ZZPLAY_OPTIONS_ERROR;
  }
  if (!IconBase) {
    IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 37U);
  }
  if (!IconBase) {
    return ZZPLAY_OPTIONS_ERROR;
  }
  args = startup->sm_ArgList;

  /* sm_ArgList[0] is always the tool itself; its ToolTypes are the user's
   * defaults. The first dropped project may then override them, which is
   * the conventional Workbench precedence. */
  zzplay_launch_set_dir(launch, args[0].wa_Lock);
  icon = GetDiskObject((STRPTR)args[0].wa_Name);
  if (icon) {
    zzplay_launch_apply_tooltypes(options, icon, &bad_tooltype);
    FreeDiskObject(icon);
  }

  if (startup->sm_NumArgs > 1) {
    zzplay_launch_set_dir(launch, args[1].wa_Lock);
    icon = GetDiskObject((STRPTR)args[1].wa_Name);
    if (icon) {
      zzplay_launch_apply_tooltypes(options, icon, &bad_tooltype);
      FreeDiskObject(icon);
    }
  }

  /* Every dropped argument becomes a playlist entry; the paths are built
   * from the locks themselves, so they stay correct regardless of where
   * CurrentDir currently points. An argument that cannot be resolved is
   * skipped: one unreadable icon must not hide the other drops. */
  for (i = 1U; (long)i < startup->sm_NumArgs; i++) {
    char *path = zzplay_launch_path(args[i].wa_Lock,
                                    (const char *)args[i].wa_Name);

    if (!path) {
      continue;
    }
    if (!zzplay_options_add_path(options, path)) {
      FreeVec(path);
      break;
    }
    launch->paths[launch->path_count++] = path;
  }

  if (bad_tooltype) {
    return ZZPLAY_OPTIONS_ERROR;
  }
  return zzplay_options_finish(options);
}

void zzplay_launch_end(ZZPlayLaunch *launch)
{
  unsigned i;

  if (!launch) {
    return;
  }
  for (i = 0U; i < launch->path_count; i++) {
    FreeVec(launch->paths[i]);
    launch->paths[i] = 0;
  }
  launch->path_count = 0U;
  if (launch->old_directory != -1) {
    (void)CurrentDir((BPTR)launch->old_directory);
    launch->old_directory = -1;
  }
  if (AslBase) {
    CloseLibrary(AslBase);
    AslBase = 0;
  }
  if (IconBase) {
    CloseLibrary(IconBase);
    IconBase = 0;
  }
  if (zzplay_launch_owns_intuition && IntuitionBase) {
    CloseLibrary((struct Library *)IntuitionBase);
    IntuitionBase = 0;
    zzplay_launch_owns_intuition = 0;
  }
  launch->startup = 0;
}
