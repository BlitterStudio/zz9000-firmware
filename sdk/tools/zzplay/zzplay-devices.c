/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-devices.h"

#include <devices/ahi.h>
#include <dos/dos.h>
#include <exec/types.h>
#include <proto/ahi.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zzplay-ahiprefs.h"

/* Program-wide owner of AHIBase per plan. Kept non-zero only while querying
 * audio mode names, then restored to 0. */
struct Library *AHIBase;

#define ZZPLAY_AHI_PREFS_BUF_MAX 4096U

/* Static buffer to keep large reads off small Amiga stacks. */
static uint8_t ahi_prefs_data[ZZPLAY_AHI_PREFS_BUF_MAX];

void zzplay_devices_ahi_units(
    char labels[ZZPLAY_PREFS_AHI_UNITS][ZZPLAY_DEVICES_NAME_MAX])
{
  uint32_t u;
  BPTR file;
  LONG read_bytes = 0;
  ZZPlayAHIUnitPref parsed_units[ZZPLAY_AHIPREFS_UNITS];
  int parsed_count = -1;
  struct MsgPort *port = 0;
  struct AHIRequest *req = 0;
  int device_open = 0;

  if (!labels) {
    return;
  }

  /* Default label when AHI or its prefs are unavailable: "Unit 0", etc. */
  for (u = 0U; u < ZZPLAY_PREFS_AHI_UNITS; u++) {
    sprintf(labels[u], "Unit %lu", (unsigned long)u);
  }

  file = Open((CONST_STRPTR)ZZPLAY_AHIPREFS_PATH, MODE_OLDFILE);
  if (file) {
    read_bytes = Read(file, ahi_prefs_data, sizeof(ahi_prefs_data));
    Close(file);
  }

  if (read_bytes > 0) {
    parsed_count = zzplay_ahiprefs_parse(ahi_prefs_data, (size_t)read_bytes,
                                         parsed_units);
  }

  if (parsed_count < 0) {
    /* Prefs unavailable or malformed. */
    return;
  }

  port = CreateMsgPort();
  if (port) {
    req = (struct AHIRequest *)CreateIORequest(port, sizeof(struct AHIRequest));
    if (req) {
      req->ahir_Version = 4U;
      if (OpenDevice((CONST_STRPTR)AHINAME, AHI_NO_UNIT,
                     (struct IORequest *)req, 0U) == 0) {
        device_open = 1;
        AHIBase = (struct Library *)req->ahir_Std.io_Device;
      }
    }
  }

  if (!device_open) {
    /* AHI unavailable: leave labels as "Unit %lu". */
    if (req) {
      DeleteIORequest((struct IORequest *)req);
    }
    if (port) {
      DeleteMsgPort(port);
    }
    return;
  }

  /* Both prefs and AHI are available: name each unit. */
  for (u = 0U; u < ZZPLAY_PREFS_AHI_UNITS; u++) {
    if (!parsed_units[u].present) {
      sprintf(labels[u], "Unit %lu: (not configured)", (unsigned long)u);
    } else {
      /* "Unit N: " takes 8 of the label's bytes; ask AHI for no more name
       * than fits after it. */
      char mode_name[ZZPLAY_DEVICES_NAME_MAX - 8U];
      mode_name[0] = '\0';

      /* BufferLen first: AHI applies it to the string tags that follow
       * it, and a name requested before it is copied with length 0. */
      if (AHI_GetAudioAttrs(parsed_units[u].audio_mode, 0,
                            AHIDB_BufferLen, (ULONG)sizeof(mode_name),
                            AHIDB_Name, (ULONG)mode_name,
                            TAG_DONE) && mode_name[0] != '\0') {
        sprintf(labels[u], "Unit %lu: %s", (unsigned long)u, mode_name);
      } else {
        sprintf(labels[u], "Unit %lu: (unknown mode)", (unsigned long)u);
      }
    }
  }

  CloseDevice((struct IORequest *)req);
  AHIBase = 0;

  DeleteIORequest((struct IORequest *)req);
  DeleteMsgPort(port);
}

static int compare_driver_names(const void *a, const void *b)
{
  return strcasecmp((const char *)a, (const char *)b);
}

uint32_t zzplay_devices_mhi_drivers(
    char names[][ZZPLAY_PREFS_DRIVER_MAX], uint32_t max)
{
  BPTR lock;
  struct FileInfoBlock *fib;
  uint32_t count = 0U;

  if (!names || max == 0U) {
    return 0U;
  }

  lock = Lock((CONST_STRPTR)"LIBS:MHI", ACCESS_READ);
  if (!lock) {
    return 0U;
  }

  fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, 0);
  if (!fib) {
    UnLock(lock);
    return 0U;
  }

  if (Examine(lock, fib)) {
    while (ExNext(lock, fib)) {
      if (fib->fib_DirEntryType < 0) {
        const char *name = (const char *)fib->fib_FileName;
        size_t len = strlen(name);
        if (len >= 8U &&
            strcasecmp(name + len - 8U, ".library") == 0) {
          if (len < ZZPLAY_PREFS_DRIVER_MAX && count < max) {
            strncpy(names[count], name,
                    ZZPLAY_PREFS_DRIVER_MAX - 1U);
            names[count][ZZPLAY_PREFS_DRIVER_MAX - 1U] = '\0';
            count++;
          }
        }
      }
    }
  }

  FreeDosObject(DOS_FIB, fib);
  UnLock(lock);

  if (count > 1U) {
    qsort(names, (size_t)count, ZZPLAY_PREFS_DRIVER_MAX, compare_driver_names);
  }

  return count;
}
