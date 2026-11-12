/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-mpega.h"

#include "zzplay-ahi.h"
#include "zzplay-mp3-transport.h"
#include "zzplay-launch.h"
#include "zzplay-sys.h"

#include <SDI_compiler.h>
#include <exec/libraries.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <libraries/mpega.h>
#include <proto/mpega.h>

#include <string.h>

/* A strong definition prevents libstubs from auto-opening mpega.library
 * before main. It is optional and is opened only for a card-less MP3 item. */
struct Library *MPEGABase = 0;

/* mpega.library writes planar decoded PCM into caller-owned buffers. These
 * must outlive each decode slice and cannot live on the small task stack. */
static WORD zzplay_mpega_pcm[MPEGA_MAX_CHANNELS][MPEGA_PCM_SIZE];

typedef struct ZZPlayMPEGAInput {
  ZZPlayFile *file;
  uint64_t file_size;
  int seekable;
} ZZPlayMPEGAInput;

static ULONG zzplay_mpega_bs_access(REG(a0, struct Hook *hook),
                                    REG(a2, APTR object),
                                    REG(a1, MPEGA_ACCESS *access))
{
  ZZPlayMPEGAInput *input;
  size_t bytes;

  (void)object;
  if (!hook || !access) {
    return 0U;
  }
  input = (ZZPlayMPEGAInput *)hook->h_Data;
  if (!input) {
    return 0U;
  }
  switch (access->func) {
  case MPEGA_BSFUNC_OPEN:
    if (input->file || !access->data.open.stream_name) {
      return 0U;
    }
    input->file = zzplay_file_open(access->data.open.stream_name, 0);
    if (!input->file) {
      return 0U;
    }
    if (input->file_size != 0U && input->file_size <= 0x7fffffffUL) {
      access->data.open.stream_size = (LONG)input->file_size;
    }
    return (ULONG)input;
  case MPEGA_BSFUNC_CLOSE:
    if (input->file) {
      zzplay_file_close(input->file);
      input->file = 0;
    }
    return 0U;
  case MPEGA_BSFUNC_READ:
    if (!input->file || !access->data.read.buffer ||
        access->data.read.num_bytes <= 0) {
      return 0U;
    }
    bytes = zzplay_file_read(input->file, access->data.read.buffer,
                             (size_t)access->data.read.num_bytes);
    return (ULONG)bytes;
  case MPEGA_BSFUNC_SEEK:
    if (!input->file || !input->seekable ||
        access->data.seek.abs_byte_seek_pos < 0) {
      return 1U;
    }
    return zzplay_file_seek(input->file, access->data.seek.abs_byte_seek_pos,
                            ZZPLAY_FILE_BEGIN) ? 0U : 1U;
  default:
    return 0U;
  }
}

static int zzplay_mpega_controls(const ZZPlayEngineRun *run, ZZPlayAHISink *ahi)
{
  uint32_t volume;
  int was_playing;
  if (run->pump) {
    run->pump(run->pump_user);
  }
  if (zzplay_controller_take_volume(run->ctl, &volume)) {
    zzplay_ahi_set_volume(ahi, volume);
  }
  if (run->ctl->paused) {
    was_playing = ahi->clock.state == ZZPLAY_AUDIO_CLOCK_PLAYING;

    if (was_playing && !zzplay_ahi_pause(ahi)) {
      return 0;
    }
    zzplay_controller_set_engine_paused(run->ctl, 1);
    while (run->ctl->paused &&
           !zzplay_controller_item_should_end(run->ctl)) {
      if (run->pump) {
        run->pump(run->pump_user);
      }
      Delay(1);
    }
    if (zzplay_controller_item_should_end(run->ctl) ||
        (was_playing && !zzplay_ahi_resume(ahi))) {
      return 0;
    }
    zzplay_controller_set_engine_paused(run->ctl, 0);
  }
  return !zzplay_controller_item_should_end(run->ctl);
}

static int zzplay_mpega_submit(ZZPlayAHISink *ahi, WORD *pcm[2],
                               uint32_t channels, uint32_t frames,
                               uint32_t *frame_offset)
{
  uint8_t *out;
  size_t capacity;
  uint32_t output_frames;
  uint32_t i;

  out = (uint8_t *)zzplay_ahi_acquire_buffer(ahi, &capacity);
  if (!out || channels == 0U || frames <= *frame_offset) {
    return 0;
  }
  output_frames = (uint32_t)(capacity / ((size_t)channels * 2U));
  if (output_frames > frames - *frame_offset) {
    output_frames = frames - *frame_offset;
  }
  for (i = 0U; i < output_frames; i++) {
    unsigned channel;

    for (channel = 0U; channel < channels; channel++) {
      WORD sample = pcm[channel][*frame_offset + i];
      size_t offset = ((size_t)i * channels + channel) * 2U;

      out[offset] = ((uint8_t *)&sample)[0];
      out[offset + 1U] = ((uint8_t *)&sample)[1];
    }
  }
  *frame_offset += output_frames;
  return zzplay_ahi_submit_buffer(ahi,
                                  (size_t)output_frames * channels * 2U);
}

ZZPlayEngineResult zzplay_mpega_run(const ZZPlayEngineRun *run)
{
  ZZPlayMPEGAInput input;
  struct Hook hook;
  MPEGA_CTRL control;
  MPEGA_STREAM *stream = 0;
  ZZPlayAHISink ahi;
  WORD *pcm[MPEGA_MAX_CHANNELS];
  LONG decoded;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t period;
  uint32_t frame_offset = 0U;
  uint32_t pending_frames = 0U;
  int playing = 0;
  ZZPlayEngineResult result = ZZPLAY_ENGINE_FAILED;

  if (!run || !run->ctl || !run->path || !run->prefs) {
    return ZZPLAY_ENGINE_FAILED;
  }
  memset(&input, 0, sizeof(input));
  memset(&hook, 0, sizeof(hook));
  memset(&control, 0, sizeof(control));
  memset(&ahi, 0, sizeof(ahi));
  ahi.fill_slot = -1;
  input.file_size = run->file_size;
  input.seekable = run->source_seekable;
  hook.h_Entry = (ULONG (*)())zzplay_mpega_bs_access;
  hook.h_Data = &input;
  control.bs_access = &hook;
  control.stream_buffer_size = 16384L;
  pcm[0] = zzplay_mpega_pcm[0];
  pcm[1] = zzplay_mpega_pcm[1];

  MPEGABase = OpenLibrary((CONST_STRPTR)"mpega.library", MPEGA_VERSION);
  if (!MPEGABase) {
    zzplay_launch_reportf(run->ctl, run->options,
                          "MP3 needs a software mpega.library for AHI output");
    return ZZPLAY_ENGINE_FAILED;
  }
  stream = MPEGA_open((char *)run->path, &control);
  if (!stream) {
    zzplay_launch_reportf(run->ctl, run->options,
                          "mpega.library cannot decode this MP3 without a ZZ9000");
    goto done;
  }
  sample_rate = stream->dec_frequency > 0 ? (uint32_t)stream->dec_frequency
                                          : (uint32_t)stream->frequency;
  channels = stream->dec_channels > 0 ? (uint32_t)stream->dec_channels
                                      : (uint32_t)stream->channels;
  if (sample_rate == 0U || (channels != 1U && channels != 2U)) {
    zzplay_launch_reportf(run->ctl, run->options,
                          "mpega.library returned an unsupported MP3 output format");
    goto done;
  }
  period = zzplay_mp3_ahi_period_frames(sample_rate, ZZPLAY_AHI_BUFFER_COUNT);
  if (!zzplay_ahi_prepare(&ahi, run->prefs->ahi_unit, sample_rate, channels,
                          period)) {
    zzplay_launch_reportf(run->ctl, run->options,
                          "AHI output unavailable (device error %d)", ahi.last_error);
    goto done;
  }
  zzplay_ahi_set_volume(&ahi, run->ctl->volume);
  zzplay_controller_set_capabilities(run->ctl, run->source_seekable, 1, 0);
  zzplay_controller_set_output(run->ctl, "AHI mpega.library");
  if (run->seek_ms != 0U && run->source_seekable &&
      MPEGA_seek(stream, (ULONG)run->seek_ms) != MPEGA_ERR_NONE) {
    zzplay_launch_reportf(run->ctl, run->options, "cannot seek in %s", run->path);
    goto done;
  }
  for (;;) {
    if (!zzplay_mpega_controls(run, &ahi) || !zzplay_ahi_poll(&ahi)) {
      result = zzplay_controller_item_should_end(run->ctl)
                   ? ZZPLAY_ENGINE_STOPPED : ZZPLAY_ENGINE_FAILED;
      break;
    }
    if (playing && sample_rate != 0U) {
      zzplay_controller_set_position(
          run->ctl,
          zzplay_mp3_presented_ms(zzplay_ahi_played_frames(&ahi),
                                  sample_rate, run->seek_ms),
          1);
    }
    if (pending_frames != 0U) {
      if (!zzplay_mpega_submit(&ahi, pcm, channels, pending_frames,
                               &frame_offset)) {
        /* Every AHI buffer is queued: give the CPU to the rest of the
         * system (and the ARexx and GUI handlers) until one returns,
         * as the card-decode paths do. */
        Delay(1);
        continue;
      }
      if (!playing) {
        playing = zzplay_ahi_play(&ahi);
        if (!playing) {
          result = ZZPLAY_ENGINE_FAILED;
          break;
        }
      }
      if (frame_offset < pending_frames) {
        continue;
      }
      pending_frames = 0U;
      frame_offset = 0U;
      continue;
    }
    decoded = MPEGA_decode_frame(stream, pcm);
    if (decoded == MPEGA_ERR_EOF) {
      zzplay_ahi_mark_end_of_stream(&ahi);
      if (!playing || zzplay_ahi_drained(&ahi)) {
        result = ZZPLAY_ENGINE_EOF;
        break;
      }
      continue;
    }
    if (decoded <= 0 || (uint32_t)decoded > MPEGA_PCM_SIZE) {
      zzplay_launch_reportf(run->ctl, run->options,
                            "mpega.library failed while decoding MP3");
      break;
    }
    pending_frames = (uint32_t)decoded;
  }

done:
  zzplay_controller_set_engine_paused(run->ctl, 0);
  zzplay_ahi_close(&ahi);
  if (stream) {
    MPEGA_close(stream);
  }
  if (input.file) {
    zzplay_file_close(input.file);
  }
  if (MPEGABase) {
    CloseLibrary(MPEGABase);
    MPEGABase = 0;
  }
  return result;
}
