/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-launch.h"

#include <dos/dos.h>
#include <exec/types.h>
#include <intuition/intuition.h>
#include <libraries/asl.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>

#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/icon.h>
#include <proto/intuition.h>

#include <stdio.h>
#include <string.h>

struct Library *IconBase;
struct Library *AslBase;
/* proto/intuition.h resolves EasyRequestArgs() through this exact global, so
 * it must be the one we open — a private handle would not be used by the
 * call. A -noixemul startup does not provide it. */
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

/* Join a lock and a name into a full path. The 68k build has no snprintf
 * guarantees worth relying on, and NameFromLock is the documented way to
 * turn a WBArg lock into text. */
static int zzplay_launch_join(ZZPlayLaunch *launch, BPTR lock,
                              const char *name)
{
  if (!launch || !name || !*name) {
    return 0;
  }
  launch->path[0] = '\0';
  if (lock) {
    if (!NameFromLock(lock, (STRPTR)launch->path,
                      (LONG)sizeof(launch->path))) {
      return 0;
    }
    if (!AddPart((STRPTR)launch->path, (CONST_STRPTR)name,
                 (ULONG)sizeof(launch->path))) {
      return 0;
    }
  } else {
    if (strlen(name) >= sizeof(launch->path)) {
      return 0;
    }
    strcpy(launch->path, name);
  }
  launch->have_path = 1;
  return 1;
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

int zzplay_launch_request_file(ZZPlayLaunch *launch)
{
  struct FileRequester *requester;
  int chosen = 0;

  if (!launch) {
    return 0;
  }
  if (!AslBase) {
    AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 37U);
  }
  if (!AslBase) {
    return 0;
  }
  requester = (struct FileRequester *)AllocAslRequestTags(
      ASL_FileRequest,
      ASLFR_TitleText, (ULONG) "Select an MPEG-1 Program Stream or MP3",
      ASLFR_DoPatterns, TRUE,
      ASLFR_InitialPattern, (ULONG) "#?.(mpg|mpeg|mp3)",
      TAG_DONE);
  if (!requester) {
    return 0;
  }
  if (AslRequestTags(requester, TAG_DONE)) {
    BPTR lock = Lock((CONST_STRPTR)requester->fr_Drawer, ACCESS_READ);

    if (lock) {
      chosen = zzplay_launch_join(launch, lock, requester->fr_File);
      UnLock(lock);
    } else {
      chosen = zzplay_launch_join(launch, 0, requester->fr_File);
    }
  }
  FreeAslRequest(requester);
  return chosen;
}

ZZPlayOptionsResult zzplay_launch_begin(int argc, char **argv,
                                        ZZPlayOptions *options,
                                        ZZPlayLaunch *launch)
{
  struct WBStartup *startup;
  struct WBArg *args;
  struct DiskObject *icon;
  int bad_tooltype = 0;

  if (!options || !launch) {
    return ZZPLAY_OPTIONS_ERROR;
  }
  memset(launch, 0, sizeof(*launch));
  launch->old_directory = -1;
  zzplay_launch_open_intuition();

  /* A CLI launch keeps the existing behaviour exactly. */
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
   * defaults. A dropped project (argument 1) may then override them, which
   * is the conventional Workbench precedence. */
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
    /* The path is built from the lock itself, so it stays correct
     * regardless of where CurrentDir currently points. */
    if (!zzplay_launch_join(launch, args[1].wa_Lock, args[1].wa_Name)) {
      return ZZPLAY_OPTIONS_ERROR;
    }
  }

  if (bad_tooltype) {
    return ZZPLAY_OPTIONS_ERROR;
  }
  /* Started from its own icon with nothing dropped on it: ask. */
  if (!launch->have_path && !zzplay_launch_request_file(launch)) {
    return ZZPLAY_OPTIONS_ERROR;
  }
  options->path = launch->path;
  return zzplay_options_finish(options);
}

void zzplay_launch_end(ZZPlayLaunch *launch)
{
  if (!launch) {
    return;
  }
  if (launch->disk_object) {
    FreeDiskObject((struct DiskObject *)launch->disk_object);
    launch->disk_object = 0;
  }
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
  launch->have_path = 0;
}
