/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-mp3.h"

#include "zz9k/audio.h"
#include "zzplay-codec.h"
#include "zzplay-core.h"
#include "zzplay-gui.h"
#include "zzplay-launch.h"
#include "zzplay-mhi.h"
#include "zzplay-mp3-transport.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* zzplay_mhi_play_file() carries one opaque pointer, so bundle everything
 * the callbacks need behind it. */
typedef struct ZZPlayMHIBridge {
  ZZPlayCodecEngine *engine;
  ZZPlayMHISink *sink;
  /* Where the current pass started, in bytes and in time; the reported
   * MHI position is derived from the bytes handed to the decoder since
   * then and therefore runs slightly ahead (shown as approximate). */
  uint64_t offset;
  uint32_t base_ms;
  /* A consumed SEEK ended the pass; restart at the new offset. */
  int seek_restart;
} ZZPlayMHIBridge;

static int zzplay_mp3_mhi_stop_thunk(void *user)
{
  ZZPlayMHIBridge *bridge = (ZZPlayMHIBridge *)user;
  ZZPlayController *ctl;
  uint32_t volume;

  if (!bridge) {
    return 0;
  }
  ctl = bridge->engine->ctl;
  /* Runs once per driver poll: this is where the MHI path pumps the GUI,
   * reports its position and applies live controls. */
  zzplay_codec_pump(bridge->engine);
  if (bridge->engine->info->bitrate_kbps != 0U) {
    zzplay_controller_set_position(
        ctl,
        bridge->base_ms +
            (uint32_t)((bridge->sink->input_bytes * 8ULL) /
                       bridge->engine->info->bitrate_kbps),
        0);
  }
  if (zzplay_controller_take_volume(ctl, &volume)) {
    (void)zzplay_mhi_set_volume(bridge->sink, volume);
    zzplay_codec_refresh_output(bridge->engine);
  }
  if (ctl->request == ZZPLAY_REQUEST_SEEK) {
    uint32_t seek_ms = 0U;

    (void)zzplay_controller_take_request(ctl, 0, &seek_ms);
    bridge->offset = zzplay_mp3_seek_offset(
        bridge->engine->audio_start, bridge->engine->audio_end,
        bridge->engine->total_ms, seek_ms);
    bridge->base_ms = seek_ms;
    bridge->seek_restart = 1;
    return 1;
  }
  return zzplay_controller_item_should_end(ctl);
}

static int zzplay_mp3_mhi_paused_thunk(void *user)
{
  ZZPlayMHIBridge *bridge = (ZZPlayMHIBridge *)user;
  ZZPlayController *ctl;

  if (!bridge) {
    return 0;
  }
  ctl = bridge->engine->ctl;
  /* play_file drives the sink's real MHI pause off this answer. Confirm
   * the hold once the sink reports it, so modal operations may run. */
  if (bridge->sink->paused) {
    zzplay_controller_set_engine_paused(ctl, 1);
  } else if (!ctl->paused) {
    zzplay_controller_set_engine_paused(ctl, 0);
  }
  return ctl->paused != 0;
}

static ZZPlayEngineResult zzplay_mp3_mhi(ZZPlayCodecEngine *engine,
                                         ZZPlayMHIStatus *open_status)
{
  const ZZPlayEngineRun *run = engine->run;
  ZZPlayMHIBridge bridge;
  ZZPlayMHISink sink;
  ZZPlayMHIStatus status;
  uint32_t repeats = run->options->loop_count;
  uint32_t completed = 0U;
  ZZPlayEngineResult result = ZZPLAY_ENGINE_FAILED;

  memset(&bridge, 0, sizeof(bridge));
  memset(&sink, 0, sizeof(sink));
  bridge.engine = engine;
  bridge.sink = &sink;
  bridge.offset = zzplay_mp3_seek_offset(
      engine->audio_start, engine->audio_end, engine->total_ms,
      run->seek_ms);
  bridge.base_ms = run->seek_ms;
  status = zzplay_mhi_acquire(&sink, run->prefs->mhi_driver);
  *open_status = status;
  if (status != ZZPLAY_MHI_OK) {
    return ZZPLAY_ENGINE_FAILED;
  }
  zzplay_controller_set_capabilities(
      run->ctl, 1, sink.volume_supported != 0, 0);
  {
    char output[ZZPLAY_NOW_OUTPUT_MAX];

    sprintf(output, "MHI %s", zzplay_mhi_driver_name(&sink));
    zzplay_codec_set_output(engine, output);
  }
  (void)zzplay_mhi_set_volume(&sink, run->ctl->volume);
  zzplay_info("zzplay: selected audio backend MHI (card-local Layer III)\n");
  for (;;) {
    FILE *file = fopen(run->path, "rb");

    if (!file) {
      *open_status = ZZPLAY_MHI_IO_ERROR;
      goto done;
    }
    if (fseek(file, (long)bridge.offset, SEEK_SET) != 0) {
      fclose(file);
      *open_status = ZZPLAY_MHI_IO_ERROR;
      goto done;
    }
    bridge.seek_restart = 0;
    status = zzplay_mhi_play_file(
        &sink, file, zzplay_mp3_mhi_stop_thunk,
        zzplay_mp3_mhi_paused_thunk, &bridge);
    fclose(file);
    if (bridge.seek_restart) {
      /* The seek was consumed inside the poll; restart the pass at the
       * new offset with the decoder still held. */
      continue;
    }
    if (status == ZZPLAY_MHI_STOPPED) {
      result = ZZPLAY_ENGINE_STOPPED;
      goto done;
    }
    if (status != ZZPLAY_MHI_OK) {
      *open_status = status;
      goto done;
    }
    zzplay_info("zzplay: MP3 MHI loop %lu: %lu input bytes\n",
           (unsigned long)completed,
           (unsigned long)(sink.input_bytes > 0xffffffffULL
                               ? 0xffffffffULL
                               : sink.input_bytes));
    if (zzplay_controller_loop_item(engine->ctl) ||
        (run->options->loop_mode == ZZPLAY_LOOP_FINITE &&
         repeats != 0U)) {
      if (run->options->loop_mode == ZZPLAY_LOOP_FINITE) {
        repeats--;
      }
      bridge.offset = engine->audio_start;
      bridge.base_ms = 0U;
      completed++;
      continue;
    }
    zzplay_info("zzplay: MP3 MHI playback complete, %lu loops\n",
           (unsigned long)completed);
    result = ZZPLAY_ENGINE_EOF;
    break;
  }

done:
  zzplay_mhi_release(&sink);
  return result;
}

ZZPlayEngineResult zzplay_mp3_run(const ZZPlayEngineRun *run)
{
  ZZPlayCodecEngine engine;
  ZZPlayMHIStatus mhi_status = ZZPLAY_MHI_MISSING;
  ZZPlayAudioBackend requested;
  int strict = 0;

  if (!run || !run->ctl || !run->path || !run->probe ||
      !run->options || !run->prefs) {
    return ZZPLAY_ENGINE_FAILED;
  }
  memset(&engine, 0, sizeof(engine));
  engine.run = run;
  engine.ctl = run->ctl;
  engine.info = &run->probe->mp3;
  engine.codec = ZZ9K_AUDIO_CODEC_MP3;
  engine.sample_rate = engine.info->sample_rate;
  engine.channels = engine.info->channels;
  engine.pcm_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE;
  engine.pcm_width = 2U;
  engine.min_pcm_bytes = 1152U * 2U * 2U;
  engine.codec_name = "MP3";
  engine.seekable = 1;

  /* The application read these boundaries with the probing handle, so this
   * engine needs no metadata-only open before its streaming handles. */
  engine.audio_start = run->audio_start;
  engine.audio_end = run->file_size > (uint64_t)run->trailer_bytes
                         ? run->file_size - run->trailer_bytes
                         : engine.audio_start;
  engine.total_ms = zzplay_mp3_duration_ms(
      engine.audio_end - engine.audio_start,
      engine.info->bitrate_kbps);
  zzplay_controller_set_duration(run->ctl, engine.total_ms);
  zzplay_controller_set_capabilities(run->ctl, 1, 0, 0);
  {
    char format[ZZPLAY_NOW_TEXT_MAX];

    sprintf(format, "MP3 %lu Hz, %s, %lu kbps",
            (unsigned long)engine.info->sample_rate,
            engine.info->channels == 1U ? "mono" : "stereo",
            (unsigned long)engine.info->bitrate_kbps);
    zzplay_controller_set_format(run->ctl, format);
  }

  requested = zzplay_prefs_requested_backend(
      run->prefs, run->options, ZZPLAY_MEDIA_AUDIO_MP3, &strict);
  if (requested == ZZPLAY_AUDIO_AX) {
    if (strict) {
      zzplay_launch_reportf(
          engine.ctl, engine.run->options,
          "direct AX is not a standalone MP3 backend; use MHI or AHI");
      return ZZPLAY_ENGINE_FAILED;
    }
    /* A saved (non-strict) AX preference falls back like AUTO. */
    requested = ZZPLAY_AUDIO_AUTO;
  }
  if (requested == ZZPLAY_AUDIO_MHI || requested == ZZPLAY_AUDIO_AUTO) {
    ZZPlayEngineResult mhi_result = zzplay_mp3_mhi(&engine, &mhi_status);

    if (mhi_result != ZZPLAY_ENGINE_FAILED) {
      return mhi_result;
    }
    if (run->ctl->request != ZZPLAY_REQUEST_NONE) {
      return ZZPLAY_ENGINE_STOPPED;
    }
    if (strict && requested == ZZPLAY_AUDIO_MHI) {
      zzplay_launch_reportf(engine.ctl, engine.run->options,
                            "MHI playback failed: %s",
                            zzplay_mhi_status_name(mhi_status));
      return ZZPLAY_ENGINE_FAILED;
    }
    if (mhi_status == ZZPLAY_MHI_IO_ERROR) {
      zzplay_launch_reportf(engine.ctl, engine.run->options,
                            "MHI playback failed: %s",
                            zzplay_mhi_status_name(mhi_status));
      return ZZPLAY_ENGINE_FAILED;
    }
    zzplay_info("zzplay: MHI unavailable before playback (%s); "
           "AUTO falling back to accelerated decode + AHI\n",
           zzplay_mhi_status_name(mhi_status));
  }
  if (requested == ZZPLAY_AUDIO_NONE) {
    zzplay_info("zzplay: selected audio backend NONE "
           "(accelerated Layer III decode benchmark)\n");
    engine.muted = 1;
    zzplay_controller_set_capabilities(run->ctl, 1, 0, 0);
    zzplay_codec_set_output(&engine, "none");
  } else {
    zzplay_info("zzplay: selected audio backend AHI "
           "(accelerated Layer III decode, S16BE)\n");
    zzplay_controller_set_capabilities(run->ctl, 1, 1, 0);
    sprintf(engine.output_base, "AHI unit %lu",
            (unsigned long)run->prefs->ahi_unit);
    zzplay_codec_refresh_output(&engine);
  }
  if (requested == ZZPLAY_AUDIO_AHI && !strict) {
    /* A saved AHI preference falls back like AUTO when its path cannot
     * be had before playback; an explicit --audio=ahi does not. */
    int unavailable = 0;
    ZZPlayEngineResult result = zzplay_codec_accelerated(&engine, &unavailable);

    if (!unavailable || run->ctl->request != ZZPLAY_REQUEST_NONE) {
      return result;
    }
    zzplay_info("zzplay: saved AHI output unavailable before playback; "
           "falling back to MHI\n");
    result = zzplay_mp3_mhi(&engine, &mhi_status);
    if (result != ZZPLAY_ENGINE_FAILED) {
      return result;
    }
    if (run->ctl->request != ZZPLAY_REQUEST_NONE) {
      return ZZPLAY_ENGINE_STOPPED;
    }
    zzplay_launch_reportf(engine.ctl, engine.run->options,
                          "AHI output unavailable; MHI playback failed: %s",
                          zzplay_mhi_status_name(mhi_status));
    return ZZPLAY_ENGINE_FAILED;
  }
  return zzplay_codec_accelerated(&engine, 0);
}
