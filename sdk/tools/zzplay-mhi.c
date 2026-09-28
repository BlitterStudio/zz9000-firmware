/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-mhi.h"

#include <exec/libraries.h>
#include <exec/tasks.h>
#include <libraries/mhi.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/mhi.h>

#include <string.h>

#define ZZPLAY_MHI_BUFFER_BYTES (16UL * 1024UL)
#define ZZPLAY_MHI_LIBRARY_PATH "MHI/mhizz9000.library"

/* A strong definition prevents the Amiga linker from satisfying this base
 * with libstubs.a(mhi.o), whose pre-main hook opens the unrelated
 * MHI.library before zzplay can select the runtime driver. */
struct Library *MHIBase = 0;

ZZPlayMHIStatus zzplay_mhi_acquire(ZZPlayMHISink *sink)
{
  if (!sink) {
    return ZZPLAY_MHI_IO_ERROR;
  }
  memset(sink, 0, sizeof(*sink));
  sink->signal_bit = -1;
  MHIBase = OpenLibrary((CONST_STRPTR)ZZPLAY_MHI_LIBRARY_PATH, 0U);
  if (!MHIBase) {
    return ZZPLAY_MHI_MISSING;
  }
  sink->library = MHIBase;
  if (MHIQuery(MHIQ_LAYER3) != MHIF_SUPPORTED) {
    zzplay_mhi_release(sink);
    return ZZPLAY_MHI_UNSUPPORTED;
  }
  sink->signal_bit = AllocSignal(-1L);
  if (sink->signal_bit < 0) {
    zzplay_mhi_release(sink);
    return ZZPLAY_MHI_NO_MEMORY;
  }
  sink->signal_mask = 1UL << (unsigned)sink->signal_bit;
  sink->decoder = MHIAllocDecoder(FindTask(0), sink->signal_mask);
  if (!sink->decoder) {
    zzplay_mhi_release(sink);
    return ZZPLAY_MHI_BUSY;
  }
  return ZZPLAY_MHI_OK;
}

static int zzplay_mhi_buffer_index(void *const *buffers, APTR empty)
{
  unsigned i;

  for (i = 0U; i < ZZPLAY_MHI_BUFFER_COUNT; i++) {
    if (buffers[i] == empty) {
      return (int)i;
    }
  }
  return -1;
}

static int zzplay_mhi_ensure_buffers(ZZPlayMHISink *sink)
{
  unsigned i;

  for (i = 0U; i < ZZPLAY_MHI_BUFFER_COUNT; i++) {
    if (!sink->buffers[i]) {
      sink->buffers[i] = AllocVec(ZZPLAY_MHI_BUFFER_BYTES, MEMF_PUBLIC);
      if (!sink->buffers[i]) {
        unsigned j;

        for (j = 0U; j < ZZPLAY_MHI_BUFFER_COUNT; j++) {
          if (sink->buffers[j]) {
            FreeVec(sink->buffers[j]);
            sink->buffers[j] = 0;
          }
        }
        return 0;
      }
    }
  }
  return 1;
}

ZZPlayMHIStatus zzplay_mhi_play_file(
    ZZPlayMHISink *sink,
    FILE *file,
    ZZPlayMHIStopRequested stop_requested,
    ZZPlayMHIPaused paused,
    void *user)
{
  uint32_t queued = 0U;
  ZZPlayMHIStatus result = ZZPLAY_MHI_IO_ERROR;
  unsigned i;
  int eof = 0;
  int eof_announced = 0;

  if (!sink || !sink->decoder || !file) {
    return ZZPLAY_MHI_IO_ERROR;
  }
  sink->input_bytes = 0U;
  if (!zzplay_mhi_ensure_buffers(sink)) {
    return ZZPLAY_MHI_NO_MEMORY;
  }
  for (i = 0U; i < ZZPLAY_MHI_BUFFER_COUNT; i++) {
    size_t got = fread(sink->buffers[i], 1U,
                       ZZPLAY_MHI_BUFFER_BYTES, file);

    if (got == 0U) {
      if (ferror(file)) {
        goto done;
      }
      eof = 1;
      break;
    }
    if (!MHIQueueBuffer(sink->decoder, sink->buffers[i], (ULONG)got)) {
      goto done;
    }
    sink->input_bytes += got;
    queued++;
  }
  if (queued == 0U) {
    goto done;
  }
  SetSignal(0L, sink->signal_mask);
  MHIPlay(sink->decoder);
  sink->playing = 1U;
  sink->paused = 0U;
  for (;;) {
    UBYTE status;
    APTR empty;

    if (stop_requested && stop_requested(user)) {
      result = ZZPLAY_MHI_STOPPED;
      goto done;
    }
    if (paused) {
      int want_pause = paused(user) != 0;

      if (want_pause && !sink->paused) {
        (void)zzplay_mhi_pause(sink);
      } else if (!want_pause && sink->paused) {
        (void)zzplay_mhi_resume(sink);
      }
      if (sink->paused) {
        /* Hold input as well: refilling while the decoder is paused races
         * the driver's buffer accounting for no benefit. */
        Delay(1U);
        continue;
      }
    }
    while ((empty = MHIGetEmpty(sink->decoder)) != 0) {
      int index = zzplay_mhi_buffer_index(sink->buffers, empty);

      if (index < 0 || queued == 0U) {
        goto done;
      }
      queued--;
      if (!eof) {
        size_t got = fread(sink->buffers[index], 1U,
                           ZZPLAY_MHI_BUFFER_BYTES, file);

        if (got == 0U) {
          if (ferror(file)) {
            goto done;
          }
          eof = 1;
        } else {
          if (!MHIQueueBuffer(sink->decoder, sink->buffers[index],
                              (ULONG)got)) {
            goto done;
          }
          sink->input_bytes += got;
          queued++;
        }
      }
    }
    if (eof && queued == 0U && !eof_announced) {
      /* mhizz9000 extends the public queue call with a self-detecting
       * zero-length EOF marker. Firmware needs the corresponding stream EOF
       * flag to decode the last short input and drain a sub-period PCM tail. */
      if (!MHIQueueBuffer(sink->decoder, sink->buffers[0], 0U)) {
        goto done;
      }
      eof_announced = 1;
    }
    status = MHIGetStatus(sink->decoder);
    if (eof && queued == 0U && eof_announced &&
        status == MHIF_OUT_OF_DATA) {
      result = ZZPLAY_MHI_OK;
      goto done;
    }
    if (status == MHIF_STOPPED) {
      goto done;
    }
    /* Poll as well as accepting the MHI completion signal: public MHI does
     * not define an error signal, so an indefinitely blocking Wait would
     * hide a driver-side stop and delay Ctrl-C cleanup. */
    Delay(1U);
  }

done:
  if (sink && sink->decoder && (sink->playing || queued != 0U)) {
    zzplay_mhi_stop(sink);
  }
  return result;
}

int zzplay_mhi_pause(ZZPlayMHISink *sink)
{
  if (!sink || !sink->decoder || !sink->playing || sink->paused) {
    return 0;
  }
  MHIPause(sink->decoder);
  if (MHIGetStatus(sink->decoder) != MHIF_PAUSED) {
    return 0;
  }
  sink->paused = 1U;
  return 1;
}

int zzplay_mhi_resume(ZZPlayMHISink *sink)
{
  if (!sink || !sink->decoder || !sink->playing || !sink->paused) {
    return 0;
  }
  MHIPlay(sink->decoder);
  if (MHIGetStatus(sink->decoder) != MHIF_PLAYING) {
    return 0;
  }
  sink->paused = 0U;
  return 1;
}

void zzplay_mhi_stop(ZZPlayMHISink *sink)
{
  if (!sink || !sink->decoder) {
    return;
  }
  MHIStop(sink->decoder);
  sink->playing = 0U;
  sink->paused = 0U;
  while (MHIGetEmpty(sink->decoder)) {
  }
}

void zzplay_mhi_release(ZZPlayMHISink *sink)
{
  unsigned i;

  if (!sink) {
    return;
  }
  if (sink->decoder) {
    zzplay_mhi_stop(sink);
    MHIFreeDecoder(sink->decoder);
  }
  for (i = 0U; i < ZZPLAY_MHI_BUFFER_COUNT; i++) {
    if (sink->buffers[i]) {
      FreeVec(sink->buffers[i]);
    }
  }
  if (sink->signal_bit >= 0) {
    FreeSignal(sink->signal_bit);
  }
  if (sink->library) {
    CloseLibrary((struct Library *)sink->library);
  }
  memset(sink, 0, sizeof(*sink));
  sink->signal_bit = -1;
  MHIBase = 0;
}

const char *zzplay_mhi_status_name(ZZPlayMHIStatus status)
{
  switch (status) {
    case ZZPLAY_MHI_OK: return "ok";
    case ZZPLAY_MHI_MISSING: return "missing-library";
    case ZZPLAY_MHI_UNSUPPORTED: return "unsupported-layer";
    case ZZPLAY_MHI_BUSY: return "busy";
    case ZZPLAY_MHI_NO_MEMORY: return "no-memory";
    case ZZPLAY_MHI_IO_ERROR: return "io-error";
    case ZZPLAY_MHI_STOPPED: return "stopped";
    default: return "unknown";
  }
}
