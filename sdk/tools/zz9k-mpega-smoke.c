/*
 * Smoke tool for the experimental ZZ9000-backed mpega.library path.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <devices/timer.h>
#include <SDI_compiler.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/mpega.h>
#include <proto/timer.h>
#include <proto/zz9k.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zz9k/library_vectors.h"

#define MPEGA_OUTPUT_BUFFER_BYTES (32UL * 1024UL)
#define MPEGA_FIND_SYNC_BUFFER_BYTES (64UL * 1024UL)

typedef uint64_t MPEGATick;

typedef struct MPEGATimer {
  uint32_t ticks_per_second;
  int high_resolution;
  struct MsgPort *timer_port;
  struct timerequest *timer_request;
} MPEGATimer;

typedef struct MPEGAStats {
  MPEGATimer timer;
  MPEGATick total_ticks;
  MPEGATick decode_ticks;
  MPEGATick write_ticks;
  unsigned long output_flushes;
} MPEGAStats;

typedef struct MPEGAOutputBuffer {
  FILE *file;
  unsigned long used;
  unsigned long flushes;
  unsigned char data[MPEGA_OUTPUT_BUFFER_BYTES];
} MPEGAOutputBuffer;

struct Library *MPEGABase;
struct Library *ZZ9KBase;
struct Device *TimerBase;

static void usage(const char *name)
{
  printf("usage: %s [--trace] [--stats] [--open-only] [--stream-only] "
         "[--stream-info] [--all] [--wav] [--hook-access] "
         "[--force-mono] [--no-check-mpeg] [--expect-open-fail] "
         "[--find-sync] [--null-api-check] "
         "[--freq-div <1|2|4>] [--freq-max <hz>] "
         "[--scale <percent>] [--seek-ms <ms>] "
         "[--stream-buffer-size <bytes>] [--lib <library>] "
         "[--frames <count>] [--out <pcm|wav>] <input.mp3>\n", name);
  printf("       --all is equivalent to --frames 0 and decodes until EOF\n");
}

static ULONG mpega_smoke_bs_access(REG(a0, struct Hook *hook),
                                   REG(a2, APTR handle),
                                   REG(a1, MPEGA_ACCESS *access))
{
  BPTR file;
  LONG got;
  LONG end_pos;

  (void)hook;
  if (!access) {
    return 0;
  }

  file = (BPTR)handle;
  switch (access->func) {
    case MPEGA_BSFUNC_OPEN:
      file = Open((CONST_STRPTR)access->data.open.stream_name, MODE_OLDFILE);
      if (!file) {
        return 0;
      }
      end_pos = Seek(file, 0, OFFSET_END);
      if (end_pos >= 0) {
        access->data.open.stream_size = end_pos;
      } else {
        access->data.open.stream_size = 0;
      }
      (void)Seek(file, 0, OFFSET_BEGINNING);
      (void)access->data.open.buffer_size;
      return (ULONG)file;

    case MPEGA_BSFUNC_CLOSE:
      if (file) {
        Close(file);
      }
      return 0;

    case MPEGA_BSFUNC_READ:
      if (!file || !access->data.read.buffer ||
          access->data.read.num_bytes <= 0) {
        return 0;
      }
      got = Read(file, access->data.read.buffer,
                 access->data.read.num_bytes);
      return got > 0 ? (ULONG)got : 0;

    case MPEGA_BSFUNC_SEEK:
      if (!file) {
        return 1;
      }
      return Seek(file, access->data.seek.abs_byte_seek_pos,
                  OFFSET_BEGINNING) >= 0 ? 0U : 1U;
  }

  return 0;
}

static struct Hook mpega_smoke_bs_access_hook = {
  {0, 0}, (ULONG (*)())mpega_smoke_bs_access, 0, 0
};

static void mpega_timer_close(MPEGATimer *timer)
{
  if (!timer) {
    return;
  }
  if (timer->timer_request) {
    if (timer->high_resolution) {
      CloseDevice((struct IORequest *)timer->timer_request);
    }
    DeleteIORequest(timer->timer_request);
  }
  if (timer->timer_port) {
    DeleteMsgPort(timer->timer_port);
  }
  if (TimerBase && timer->high_resolution) {
    TimerBase = 0;
  }
}

static void mpega_timer_open(MPEGATimer *timer)
{
  memset(timer, 0, sizeof(*timer));

  timer->timer_port = CreateMsgPort();
  if (timer->timer_port) {
    timer->timer_request = (struct timerequest *)CreateIORequest(
        timer->timer_port, sizeof(*timer->timer_request));
  }
  if (timer->timer_request &&
      OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                 (struct IORequest *)timer->timer_request, 0) == 0) {
    struct EClockVal value;

    TimerBase = (struct Device *)timer->timer_request->tr_node.io_Device;
    timer->ticks_per_second = ReadEClock(&value);
    if (timer->ticks_per_second != 0U) {
      timer->high_resolution = 1;
      return;
    }
    CloseDevice((struct IORequest *)timer->timer_request);
  }

  mpega_timer_close(timer);
  memset(timer, 0, sizeof(*timer));
  timer->ticks_per_second = 50U;
}

static MPEGATick mpega_eclock_to_tick(uint32_t hi, uint32_t lo)
{
  return (((MPEGATick)hi) << 32) | (MPEGATick)lo;
}

static MPEGATick mpega_timer_now(const MPEGATimer *timer)
{
  if (timer && timer->high_resolution) {
    struct EClockVal value;

    ReadEClock(&value);
    return mpega_eclock_to_tick(value.ev_hi, value.ev_lo);
  } else {
    struct DateStamp stamp;
    uint32_t minutes;

    DateStamp(&stamp);
    minutes = ((uint32_t)stamp.ds_Days * 24U * 60U) +
              (uint32_t)stamp.ds_Minute;
    return ((MPEGATick)minutes * 60ULL * 50ULL) + (uint32_t)stamp.ds_Tick;
  }
}

static uint32_t mpega_ticks_to_ms(MPEGATick ticks, uint32_t ticks_per_second)
{
  uint64_t ms;

  if (ticks_per_second == 0U) {
    return 0U;
  }
  ms = (ticks * 1000ULL) / ticks_per_second;
  return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static void mpega_stats_add(MPEGATick *target, MPEGATick start, MPEGATick end)
{
  if (target && end >= start) {
    *target += end - start;
  }
}

static int parse_ulong(const char *text, unsigned long *value)
{
  char *end;
  unsigned long parsed;

  parsed = strtoul(text, &end, 0);
  if (!text[0] || *end != '\0') {
    return 0;
  }

  *value = parsed;
  return 1;
}

static LONG read_find_sync_buffer(const char *path, unsigned char *buffer,
                                  LONG length)
{
  BPTR file;
  LONG got;

  if (!path || !buffer || length <= 0) {
    return -1;
  }

  file = Open((CONST_STRPTR)path, MODE_OLDFILE);
  if (!file) {
    return -1;
  }

  got = Read(file, buffer, length);
  Close(file);
  return got >= 0 ? got : -1;
}

static int run_null_api_check(void)
{
  ULONG time_ms = 0x12345678UL;
  LONG status;

  status = MPEGA_seek(0, 0);
  if (status != MPEGA_ERR_EOF) {
    printf("MPEGA_seek(NULL) returned %ld, expected %ld\n",
           (long)status, (long)MPEGA_ERR_EOF);
    return 0;
  }

  status = MPEGA_time(0, &time_ms);
  if (status != MPEGA_ERR_EOF) {
    printf("MPEGA_time(NULL) returned %ld, expected %ld\n",
           (long)status, (long)MPEGA_ERR_EOF);
    return 0;
  }

  printf("null-api check ok\n");
  return 1;
}

static void mpega_output_init(MPEGAOutputBuffer *output, FILE *file)
{
  memset(output, 0, sizeof(*output));
  output->file = file;
}

static int mpega_output_flush(MPEGAOutputBuffer *output)
{
  if (!output || !output->file || output->used == 0UL) {
    return 1;
  }

  if (fwrite(output->data, 1U, output->used, output->file) != output->used) {
    return 0;
  }
  output->used = 0UL;
  output->flushes++;
  return 1;
}

static int mpega_output_write(MPEGAOutputBuffer *output, const void *data,
                              unsigned long length)
{
  const unsigned char *src = (const unsigned char *)data;

  if (!output || !output->file || (length != 0UL && !src)) {
    return 0;
  }

  while (length != 0UL) {
    unsigned long space = MPEGA_OUTPUT_BUFFER_BYTES - output->used;
    unsigned long part;

    if (space == 0UL && !mpega_output_flush(output)) {
      return 0;
    }
    space = MPEGA_OUTPUT_BUFFER_BYTES - output->used;
    part = length < space ? length : space;
    memcpy(output->data + output->used, src, part);
    output->used += part;
    src += part;
    length -= part;
  }

  return 1;
}

static void put_le16(unsigned char *dst, unsigned long value)
{
  dst[0] = (unsigned char)(value & 0xffU);
  dst[1] = (unsigned char)((value >> 8) & 0xffU);
}

static void put_le32(unsigned char *dst, unsigned long value)
{
  dst[0] = (unsigned char)(value & 0xffU);
  dst[1] = (unsigned char)((value >> 8) & 0xffU);
  dst[2] = (unsigned char)((value >> 16) & 0xffU);
  dst[3] = (unsigned char)((value >> 24) & 0xffU);
}

static int write_wav_header(FILE *file, unsigned long data_bytes,
                            unsigned long sample_rate,
                            unsigned long channels)
{
  unsigned char header[44];
  unsigned long byte_rate;
  unsigned long block_align;

  if (!file || sample_rate == 0UL || channels == 0UL || channels > 2UL ||
      data_bytes > 0xffffffffUL - 36UL) {
    return 0;
  }

  byte_rate = sample_rate * channels * 2UL;
  block_align = channels * 2UL;
  memset(header, 0, sizeof(header));
  memcpy(&header[0], "RIFF", 4U);
  put_le32(&header[4], 36UL + data_bytes);
  memcpy(&header[8], "WAVEfmt ", 8U);
  put_le32(&header[16], 16UL);
  put_le16(&header[20], 1UL);
  put_le16(&header[22], channels);
  put_le32(&header[24], sample_rate);
  put_le32(&header[28], byte_rate);
  put_le16(&header[32], block_align);
  put_le16(&header[34], 16UL);
  memcpy(&header[36], "data", 4U);
  put_le32(&header[40], data_bytes);

  return fwrite(header, 1U, sizeof(header), file) == sizeof(header);
}

static int patch_wav_header(FILE *file, unsigned long data_bytes,
                            unsigned long sample_rate,
                            unsigned long channels)
{
  if (!file || fseek(file, 0L, SEEK_SET) != 0 ||
      !write_wav_header(file, data_bytes, sample_rate, channels)) {
    return 0;
  }
  return fseek(file, 0L, SEEK_END) == 0;
}

static void store_wav_sample(unsigned char *dst, WORD sample)
{
  unsigned int value = (unsigned int)(UWORD)sample;

  dst[0] = (unsigned char)(value & 0xffU);
  dst[1] = (unsigned char)((value >> 8) & 0xffU);
}

static int write_frame(FILE *output_file, MPEGAOutputBuffer *output,
                       WORD *left, WORD *right, LONG samples, WORD channels,
                       int wav_output, unsigned long *output_bytes)
{
  static WORD interleaved[MPEGA_PCM_SIZE * MPEGA_MAX_CHANNELS];
  static unsigned char wav_chunk[MPEGA_PCM_SIZE * MPEGA_MAX_CHANNELS * 2U];
  unsigned long bytes;
  LONG i;

  if (!output_file) {
    return 1;
  }

  if (channels <= 0 || channels > MPEGA_MAX_CHANNELS || samples < 0) {
    return 0;
  }
  bytes = (unsigned long)samples * (unsigned long)channels * 2UL;
  if (output_bytes && bytes > 0xffffffffUL - *output_bytes) {
    return 0;
  }

  if (wav_output) {
    unsigned char *dst = wav_chunk;

    if (channels > 1) {
      const unsigned char *left_bytes = (const unsigned char *)left;
      const unsigned char *right_bytes = (const unsigned char *)right;

      for (i = 0; i < samples; i++) {
        dst[0] = left_bytes[1];
        dst[1] = left_bytes[0];
        dst[2] = right_bytes[1];
        dst[3] = right_bytes[0];
        left_bytes += 2;
        right_bytes += 2;
        dst += 4;
      }
    } else {
      const unsigned char *left_bytes = (const unsigned char *)left;

      for (i = 0; i < samples; i++) {
        dst[0] = left_bytes[1];
        dst[1] = left_bytes[0];
        left_bytes += 2;
        dst += 2;
      }
    }
    if (!mpega_output_write(output, wav_chunk, bytes)) {
      return 0;
    }
    if (output_bytes) {
      *output_bytes += bytes;
    }
    return 1;
  }

  if (channels <= 1) {
    if (!mpega_output_write(output, left, bytes)) {
      return 0;
    }
    if (output_bytes) {
      *output_bytes += bytes;
    }
    return 1;
  }

  for (i = 0; i < samples; i++) {
    interleaved[(i * 2) + 0] = left[i];
    interleaved[(i * 2) + 1] = right[i];
  }

  if (!mpega_output_write(output, interleaved, bytes)) {
    return 0;
  }
  if (output_bytes) {
    *output_bytes += bytes;
  }
  return 1;
}

static int check_zz9k_audio_stream(void)
{
  ZZ9KServiceInfo service;
  int status;

  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME, ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    printf("open zz9k.library failed; install current Libs/zz9k.library\n");
    return 0;
  }

  printf("zz9k.library version=%u revision=%u\n",
         (unsigned)ZZ9KBase->lib_Version,
         (unsigned)ZZ9KBase->lib_Revision);
  if (ZZ9KBase->lib_Revision < ZZ9K_LIBRARY_MIN_REVISION_AUDIO_STREAM) {
    printf("zz9k.library too old; need revision %u for audio streaming\n",
           (unsigned)ZZ9K_LIBRARY_MIN_REVISION_AUDIO_STREAM);
    CloseLibrary(ZZ9KBase);
    ZZ9KBase = 0;
    return 0;
  }

  status = ZZ9KQueryService(ZZ9K_SERVICE_AUDIO, &service);
  if (status != ZZ9K_STATUS_OK) {
    printf("query audio service failed: %d\n", status);
    CloseLibrary(ZZ9KBase);
    ZZ9KBase = 0;
    return 0;
  }
  if ((service.flags & ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM) == 0U) {
    printf("audio service does not advertise mp3-stream\n");
    CloseLibrary(ZZ9KBase);
    ZZ9KBase = 0;
    return 0;
  }

  CloseLibrary(ZZ9KBase);
  ZZ9KBase = 0;
  return 1;
}

int main(int argc, char **argv)
{
  const char *library_name = "mpega.library";
  const char *output_path = 0;
  const char *input_path = 0;
  unsigned long frame_limit = 16UL;
  unsigned long seek_ms = 0UL;
  unsigned long output_bytes = 0UL;
  MPEGA_CTRL ctrl;
  MPEGA_STREAM *stream = 0;
  FILE *output_file = 0;
  static MPEGAOutputBuffer output_buffer;
  static unsigned char find_sync_buffer[MPEGA_FIND_SYNC_BUFFER_BYTES];
  static WORD pcm_left[MPEGA_PCM_SIZE];
  static WORD pcm_right[MPEGA_PCM_SIZE];
  WORD *pcm[MPEGA_MAX_CHANNELS];
  MPEGAStats stats;
  MPEGATick total_start = 0U;
  ULONG time_ms = 0;
  unsigned long frames = 0;
  unsigned long samples = 0;
  unsigned long zero_frames = 0;
  int i;
  int ok = 0;
  int trace = 0;
  int show_stats = 0;
  int wav_output = 0;
  int open_only = 0;
  int stream_only = 0;
  int stream_info = 0;
  int hook_access = 0;
  int force_mono = 0;
  int check_mpeg = 1;
  int expect_open_fail = 0;
  int find_sync_only = 0;
  int null_api_check = 0;
  unsigned long freq_div = 1UL;
  unsigned long freq_max = 0UL;
  unsigned long scale_percent = 100UL;
  unsigned long stream_buffer_size = 0UL;
  int freq_div_requested = 0;
  int freq_max_requested = 0;
  int scale_requested = 0;
  int seek_requested = 0;
  int stream_buffer_size_requested = 0;

  memset(&stats, 0, sizeof(stats));
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--trace") == 0) {
      trace = 1;
    } else if (strcmp(argv[i], "--stats") == 0) {
      show_stats = 1;
    } else if (strcmp(argv[i], "--all") == 0) {
      frame_limit = 0UL;
    } else if (strcmp(argv[i], "--wav") == 0) {
      wav_output = 1;
    } else if (strcmp(argv[i], "--hook-access") == 0) {
      hook_access = 1;
    } else if (strcmp(argv[i], "--force-mono") == 0) {
      force_mono = 1;
    } else if (strcmp(argv[i], "--no-check-mpeg") == 0) {
      check_mpeg = 0;
    } else if (strcmp(argv[i], "--expect-open-fail") == 0) {
      expect_open_fail = 1;
    } else if (strcmp(argv[i], "--find-sync") == 0) {
      find_sync_only = 1;
    } else if (strcmp(argv[i], "--null-api-check") == 0) {
      null_api_check = 1;
    } else if (strcmp(argv[i], "--freq-div") == 0 && i + 1 < argc) {
      if (!parse_ulong(argv[++i], &freq_div) ||
          (freq_div != 1UL && freq_div != 2UL && freq_div != 4UL)) {
        usage(argv[0]);
        return 2;
      }
      freq_div_requested = 1;
      freq_max_requested = 0;
    } else if (strcmp(argv[i], "--freq-max") == 0 && i + 1 < argc) {
      if (!parse_ulong(argv[++i], &freq_max)) {
        usage(argv[0]);
        return 2;
      }
      freq_max_requested = 1;
      freq_div_requested = 0;
    } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
      if (!parse_ulong(argv[++i], &scale_percent) ||
          scale_percent == 0UL || scale_percent > 10000UL) {
        usage(argv[0]);
        return 2;
      }
      scale_requested = 1;
    } else if (strcmp(argv[i], "--seek-ms") == 0 && i + 1 < argc) {
      if (!parse_ulong(argv[++i], &seek_ms)) {
        usage(argv[0]);
        return 2;
      }
      seek_requested = 1;
    } else if (strcmp(argv[i], "--stream-buffer-size") == 0 &&
               i + 1 < argc) {
      if (!parse_ulong(argv[++i], &stream_buffer_size)) {
        usage(argv[0]);
        return 2;
      }
      stream_buffer_size_requested = 1;
    } else if (strcmp(argv[i], "--open-only") == 0) {
      open_only = 1;
    } else if (strcmp(argv[i], "--stream-only") == 0) {
      stream_only = 1;
    } else if (strcmp(argv[i], "--stream-info") == 0) {
      stream_info = 1;
    } else if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
      library_name = argv[++i];
    } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      if (!parse_ulong(argv[++i], &frame_limit)) {
        usage(argv[0]);
        return 2;
      }
    } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      output_path = argv[++i];
    } else if (!input_path) {
      input_path = argv[i];
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  if (!input_path && !null_api_check) {
    usage(argv[0]);
    return 2;
  }

  if (!null_api_check && !check_zz9k_audio_stream()) {
    return 1;
  }

  if (trace) {
    printf("trace: opening MPEGA library %s\n", library_name);
    fflush(stdout);
  }
  MPEGABase = OpenLibrary((CONST_STRPTR)library_name, 0);
  if (!MPEGABase) {
    printf("open %s failed\n", library_name);
    return 1;
  }
  printf("mpega.library version=%u revision=%u\n",
         (unsigned)MPEGABase->lib_Version,
         (unsigned)MPEGABase->lib_Revision);
  if (trace) {
    printf("trace: MPEGA library open ok\n");
    fflush(stdout);
  }
  if (null_api_check) {
    ok = run_null_api_check();
    goto cleanup;
  }
  if (find_sync_only) {
    LONG bytes;
    LONG sync_pos;

    bytes = read_find_sync_buffer(input_path, find_sync_buffer,
                                  (LONG)sizeof(find_sync_buffer));
    if (bytes < 0) {
      printf("find-sync read failed\n");
      goto cleanup;
    }
    sync_pos = MPEGA_find_sync((BYTE *)find_sync_buffer, bytes);
    printf("find-sync: offset=%ld bytes=%ld\n", (long)sync_pos,
           (long)bytes);
    ok = sync_pos >= 0;
    goto cleanup;
  }
  if (open_only) {
    if (trace) {
      printf("trace: open-only complete\n");
      fflush(stdout);
    }
    ok = 1;
    goto cleanup;
  }

  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.layer_1_2.mono.quality = MPEGA_QUALITY_HIGH;
  ctrl.layer_1_2.stereo.quality = MPEGA_QUALITY_HIGH;
  ctrl.layer_3.mono.quality = MPEGA_QUALITY_HIGH;
  ctrl.layer_3.stereo.quality = MPEGA_QUALITY_HIGH;
  ctrl.stream_buffer_size = (LONG)stream_buffer_size;
  ctrl.check_mpeg = (WORD)check_mpeg;
  if (stream_buffer_size_requested && trace) {
    printf("trace: stream buffer size %lu\n", stream_buffer_size);
    fflush(stdout);
  }
  if (!check_mpeg && trace) {
    printf("trace: disabling MPEGA start check\n");
    fflush(stdout);
  }
  if (hook_access) {
    ctrl.bs_access = &mpega_smoke_bs_access_hook;
    if (trace) {
      printf("trace: using hook-backed bitstream access\n");
      fflush(stdout);
    }
  }
  if (force_mono) {
    ctrl.layer_1_2.force_mono = 1;
    ctrl.layer_3.force_mono = 1;
    if (trace) {
      printf("trace: forcing mono output\n");
      fflush(stdout);
    }
  }
  if (freq_div_requested) {
    ctrl.layer_1_2.mono.freq_div = (WORD)freq_div;
    ctrl.layer_1_2.stereo.freq_div = (WORD)freq_div;
    ctrl.layer_3.mono.freq_div = (WORD)freq_div;
    ctrl.layer_3.stereo.freq_div = (WORD)freq_div;
    if (trace) {
      printf("trace: forcing frequency division %lu\n", freq_div);
      fflush(stdout);
    }
  }
  if (freq_max_requested) {
    ctrl.layer_1_2.mono.freq_div = 0;
    ctrl.layer_1_2.stereo.freq_div = 0;
    ctrl.layer_3.mono.freq_div = 0;
    ctrl.layer_3.stereo.freq_div = 0;
    ctrl.layer_1_2.mono.freq_max = (LONG)freq_max;
    ctrl.layer_1_2.stereo.freq_max = (LONG)freq_max;
    ctrl.layer_3.mono.freq_max = (LONG)freq_max;
    ctrl.layer_3.stereo.freq_max = (LONG)freq_max;
    if (trace) {
      printf("trace: setting max output frequency %lu\n", freq_max);
      fflush(stdout);
    }
  }

  if (trace) {
    printf("trace: calling MPEGA_open\n");
    fflush(stdout);
  }
  stream = MPEGA_open((char *)input_path, &ctrl);
  if (!stream) {
    if (expect_open_fail) {
      printf("MPEGA_open failed as expected\n");
      ok = 1;
      goto cleanup;
    }
    printf("MPEGA_open failed\n");
    goto cleanup;
  }
  if (expect_open_fail) {
    printf("MPEGA_open unexpectedly succeeded\n");
    MPEGA_close(stream);
    stream = 0;
    goto cleanup;
  }
  if (trace) {
    printf("trace: MPEGA_open ok\n");
    fflush(stdout);
  }
  if (scale_requested) {
    if (trace) {
      printf("trace: calling MPEGA_scale %lu\n", scale_percent);
      fflush(stdout);
    }
    if (MPEGA_scale(stream, (LONG)scale_percent) != MPEGA_ERR_NONE) {
      printf("MPEGA_scale failed: %lu%%\n", scale_percent);
      MPEGA_close(stream);
      stream = 0;
      goto cleanup;
    }
    if (trace) {
      printf("trace: MPEGA_scale ok\n");
      fflush(stdout);
    }
  }
  if (stream_info) {
    printf("stream info: norm=%d layer=%d mode=%d bitrate=%d frequency=%ld "
           "channels=%d duration=%lu private=%d copyright=%d original=%d "
           "dec_frequency=%ld dec_channels=%d dec_quality=%d\n",
           (int)stream->norm, (int)stream->layer, (int)stream->mode,
           (int)stream->bitrate, (long)stream->frequency,
           (int)stream->channels, (unsigned long)stream->ms_duration,
           (int)stream->private_bit, (int)stream->copyright,
           (int)stream->original, (long)stream->dec_frequency,
           (int)stream->dec_channels, (int)stream->dec_quality);
  }
  if (stream_only) {
    if (trace) {
      printf("trace: stream-only complete\n");
      fflush(stdout);
    }
    MPEGA_close(stream);
    stream = 0;
    ok = 1;
    goto cleanup;
  }
  if (seek_requested) {
    if (trace) {
      printf("trace: calling MPEGA_seek\n");
      fflush(stdout);
    }
    if (MPEGA_seek(stream, (ULONG)seek_ms) != MPEGA_ERR_NONE) {
      printf("MPEGA_seek failed: %lu ms\n", seek_ms);
      MPEGA_close(stream);
      stream = 0;
      goto cleanup;
    }
    if (trace) {
      printf("trace: MPEGA_seek ok\n");
      fflush(stdout);
    }
  }

  if (output_path) {
    output_file = fopen(output_path, "wb");
    if (!output_file) {
      printf("open output failed: %s\n", output_path);
      MPEGA_close(stream);
      goto cleanup;
    }
    mpega_output_init(&output_buffer, output_file);
    if (wav_output && !write_wav_header(output_file, 0UL, 44100UL, 2UL)) {
      printf("WAV header write failed\n");
      MPEGA_close(stream);
      goto cleanup;
    }
  }

  pcm[0] = pcm_left;
  pcm[1] = pcm_right;
  if (show_stats) {
    mpega_timer_open(&stats.timer);
    total_start = mpega_timer_now(&stats.timer);
  }
  while (frame_limit == 0UL || frames < frame_limit) {
    LONG count;
    MPEGATick start;

    if (trace) {
      printf("trace: calling MPEGA_decode_frame\n");
      fflush(stdout);
    }
    start = show_stats ? mpega_timer_now(&stats.timer) : 0U;
    count = MPEGA_decode_frame(stream, pcm);
    if (show_stats) {
      mpega_stats_add(&stats.decode_ticks, start,
                      mpega_timer_now(&stats.timer));
    }
    if (trace) {
      printf("trace: MPEGA_decode_frame returned %ld\n", (long)count);
      fflush(stdout);
    }
    if (count == MPEGA_ERR_EOF) {
      break;
    }
    if (count < 0) {
      printf("MPEGA_decode_frame failed: %ld\n", (long)count);
      MPEGA_close(stream);
      stream = 0;
      goto cleanup;
    }
    if (count == 0) {
      if (++zero_frames > 64UL) {
        printf("MPEGA_decode_frame made no sample progress\n");
        MPEGA_close(stream);
        goto cleanup;
      }
      continue;
    }
    zero_frames = 0;

    start = show_stats ? mpega_timer_now(&stats.timer) : 0U;
    if (!write_frame(output_file, &output_buffer, pcm_left, pcm_right, count,
                     stream->dec_channels, wav_output, &output_bytes)) {
      printf("output write failed\n");
      MPEGA_close(stream);
      goto cleanup;
    }
    if (show_stats) {
      mpega_stats_add(&stats.write_ticks, start,
                      mpega_timer_now(&stats.timer));
    }
    frames++;
    samples += (unsigned long)count;
  }

  (void)MPEGA_time(stream, &time_ms);
  if (output_file && !mpega_output_flush(&output_buffer)) {
    printf("output flush failed\n");
    MPEGA_close(stream);
    stream = 0;
    goto cleanup;
  }
  stats.output_flushes = output_buffer.flushes;
  if (output_file && wav_output) {
    if (!patch_wav_header(output_file, output_bytes,
                          (unsigned long)stream->dec_frequency,
                          (unsigned long)stream->dec_channels)) {
      printf("WAV header patch failed\n");
      MPEGA_close(stream);
      stream = 0;
      goto cleanup;
    }
  }
  printf("mpega smoke ok: frames=%lu samples=%lu bytes=%lu rate=%ld channels=%d "
         "time=%lu ms\n",
         frames, samples, output_bytes, (long)stream->dec_frequency,
         (int)stream->dec_channels, (unsigned long)time_ms);
  if (output_path) {
    printf("wrote %s to %s\n", wav_output ? "WAV" : "raw PCM", output_path);
  }
  if (show_stats) {
    uint32_t ticks_per_second = stats.timer.ticks_per_second;
    MPEGATick total_end = mpega_timer_now(&stats.timer);

    stats.total_ticks = total_end >= total_start ? total_end - total_start : 0U;
    printf("stats: total=%lu ms decode=%lu ms write=%lu ms flushes=%lu\n",
           (unsigned long)mpega_ticks_to_ms(stats.total_ticks,
                                            ticks_per_second),
           (unsigned long)mpega_ticks_to_ms(stats.decode_ticks,
                                            ticks_per_second),
           (unsigned long)mpega_ticks_to_ms(stats.write_ticks,
                                            ticks_per_second),
           stats.output_flushes);
  }
  MPEGA_close(stream);
  stream = 0;
  ok = 1;

cleanup:
  if (output_file) {
    (void)mpega_output_flush(&output_buffer);
  }
  if (show_stats) {
    mpega_timer_close(&stats.timer);
  }
  if (stream) {
    MPEGA_close(stream);
  }
  if (output_file) {
    fclose(output_file);
  }
  if (MPEGABase) {
    CloseLibrary(MPEGABase);
    MPEGABase = 0;
  }

  return ok ? 0 : 1;
}
