/*
 * Descriptor recognition hook for the ZZ9000-MP3 DataTypes descriptor.
 *
 * Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The descriptor carries an empty comparison mask and this hook in its DTCD
 * chunk. datatypes.library loads the chunk with InternalLoadSeg and calls the
 * first instruction of the first hunk with the DTHookContext in A0
 * (zz9k_mp3_dthook_entry.S). There is no startup code: library bases come
 * from the context, nothing uses the C library, and the shared recognizer
 * avoids hardware division so the hook runs on 68000 systems.
 *
 * The hook claims a file only when a bounded ID3v2 tag (or none) is followed
 * immediately by two consecutive matching Layer III frame headers. Bare ID3
 * data, Layer I/II, and streams whose first frame does not follow the tag are
 * not claimed. zz9k-sound.datatype re-validates every routed file.
 */

#include "zz9k_sound_mp3.h"

#include <datatypes/datatypes.h>
#include <dos/dos.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

long zz9k_mp3_dthook(struct DTHookContext *dthc)
{
  const uint8_t *buffer;
  uint32_t length;
  uint32_t start;
  uint8_t *probe;
  LONG position;
  LONG got;
  long recognized = 0;

  if (!dthc || !dthc->dthc_Buffer || !dthc->dthc_FileHandle) {
    return 0;
  }
  buffer = (const uint8_t *)dthc->dthc_Buffer;
  length = dthc->dthc_BufferLength;
  if (!zz9k_sound_audio_start(buffer, length, &start)) {
    return 0;
  }
  /* Cheap reject before any I/O: an untagged file must open with a Layer
   * III header; a tagged file must contain its audio start. */
  if (start == 0U ? !zz9k_sound_mp3_header(buffer, length, 0) :
      (dthc->dthc_FIB && (uint32_t)dthc->dthc_FIB->fib_Size <= start)) {
    return 0;
  }

  SysBase = (struct ExecBase *)dthc->dthc_SysBase;
  DOSBase = (struct DosLibrary *)dthc->dthc_DOSBase;
  probe = (uint8_t *)AllocVec(ZZ9K_SOUND_MP3_PAIR_PROBE_BYTES, MEMF_ANY);
  if (!probe) {
    return 0;
  }
  position = Seek(dthc->dthc_FileHandle, 0, OFFSET_CURRENT);
  if (position >= 0) {
    if (Seek(dthc->dthc_FileHandle, (LONG)start, OFFSET_BEGINNING) >= 0) {
      got = Read(dthc->dthc_FileHandle, probe,
                 (LONG)ZZ9K_SOUND_MP3_PAIR_PROBE_BYTES);
      recognized = got > 0 &&
                   zz9k_sound_mp3_pair_at(probe, (uint32_t)got, 0);
    }
    Seek(dthc->dthc_FileHandle, position, OFFSET_BEGINNING);
  }
  FreeVec(probe);
  return recognized;
}
