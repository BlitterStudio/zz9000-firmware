/*
 * Source guard for the MPEGA compatibility smoke tool.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
  FILE *file;
  long length;
  char *data;

  file = fopen(path, "rb");
  if (!file) {
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 0;
  }
  length = ftell(file);
  if (length < 0) {
    fclose(file);
    return 0;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }

  data = (char *)malloc((size_t)length + 1U);
  if (!data) {
    fclose(file);
    return 0;
  }
  if (fread(data, 1U, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    return 0;
  }

  data[length] = '\0';
  fclose(file);
  return data;
}

static int expect_contains(const char *source, const char *needle)
{
  if (strstr(source, needle)) {
    return 1;
  }

  printf("missing %s\n", needle);
  return 0;
}

static int expect_not_contains(const char *source, const char *needle)
{
  if (!strstr(source, needle)) {
    return 1;
  }

  printf("unexpected %s\n", needle);
  return 0;
}

int main(int argc, char **argv)
{
  char *source;
  int ok;

  if (argc != 2) {
    printf("usage: %s <tools/zz9k-mpega-smoke.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "#include <SDI_compiler.h>");
  ok &= expect_contains(source, "#include <proto/mpega.h>");
  ok &= expect_contains(source, "#include <proto/zz9k.h>");
  ok &= expect_contains(source, "#include \"zz9k/library_vectors.h\"");
  ok &= expect_contains(source, "struct Library *MPEGABase");
  ok &= expect_contains(source, "struct Library *ZZ9KBase");
  ok &= expect_contains(source, "static int check_zz9k_audio_stream");
  ok &= expect_contains(source, "--lib");
  ok &= expect_contains(source, "--trace");
  ok &= expect_contains(source, "--open-only");
  ok &= expect_contains(source, "--stream-only");
  ok &= expect_contains(source, "--stream-info");
  ok &= expect_contains(source, "--all");
  ok &= expect_contains(source, "--wav");
  ok &= expect_contains(source, "--stats");
  ok &= expect_contains(source, "--hook-access");
  ok &= expect_contains(source, "--force-mono");
  ok &= expect_contains(source, "--no-check-mpeg");
  ok &= expect_contains(source, "--expect-open-fail");
  ok &= expect_contains(source, "--find-sync");
  ok &= expect_contains(source, "--null-api-check");
  ok &= expect_contains(source, "--freq-div");
  ok &= expect_contains(source, "--freq-max");
  ok &= expect_contains(source, "--scale");
  ok &= expect_contains(source, "--seek-ms");
  ok &= expect_contains(source, "--stream-buffer-size");
  ok &= expect_contains(source, "--frames");
  ok &= expect_contains(source, "--out");
  ok &= expect_contains(source,
                        "OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME, "
                        "ZZ9K_LIBRARY_VERSION)");
  ok &= expect_contains(source, "ZZ9K_LIBRARY_MIN_REVISION_AUDIO_STREAM");
  ok &= expect_contains(source, "ZZ9KQueryService(ZZ9K_SERVICE_AUDIO");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM");
  ok &= expect_contains(source, "install current Libs/zz9k.library");
  ok &= expect_contains(source, "CloseLibrary(ZZ9KBase)");
  ok &= expect_contains(source, "trace: opening MPEGA library");
  ok &= expect_contains(source, "trace: MPEGA library open ok");
  ok &= expect_contains(source, "mpega.library version=%u revision=%u");
  ok &= expect_contains(source, "MPEGABase->lib_Version");
  ok &= expect_contains(source, "MPEGABase->lib_Revision");
  ok &= expect_contains(source, "trace: open-only complete");
  ok &= expect_contains(source, "trace: calling MPEGA_open");
  ok &= expect_contains(source, "trace: MPEGA_open ok");
  ok &= expect_contains(source, "MPEGA_open failed as expected");
  ok &= expect_contains(source, "MPEGA_open unexpectedly succeeded");
  ok &= expect_contains(source, "find-sync: offset=%ld bytes=%ld");
  ok &= expect_contains(source, "static int run_null_api_check");
  ok &= expect_contains(source, "MPEGA_seek(0, 0)");
  ok &= expect_contains(source, "MPEGA_time(0, &time_ms)");
  ok &= expect_contains(source, "null-api check ok");
  ok &= expect_contains(source, "trace: calling MPEGA_seek");
  ok &= expect_contains(source, "trace: MPEGA_seek ok");
  ok &= expect_contains(source, "trace: calling MPEGA_scale");
  ok &= expect_contains(source, "trace: MPEGA_scale ok");
  ok &= expect_contains(source, "trace: using hook-backed bitstream access");
  ok &= expect_contains(source, "trace: forcing mono output");
  ok &= expect_contains(source, "trace: disabling MPEGA start check");
  ok &= expect_contains(source, "trace: forcing frequency division");
  ok &= expect_contains(source, "trace: setting max output frequency");
  ok &= expect_contains(source, "trace: stream buffer size");
  ok &= expect_contains(source, "trace: stream-only complete");
  ok &= expect_contains(source, "stream info: norm=%d layer=%d mode=%d");
  ok &= expect_contains(source, "duration=%lu");
  ok &= expect_contains(source, "trace: calling MPEGA_decode_frame");
  ok &= expect_not_contains(source, "--resident-trace");
  ok &= expect_not_contains(source, "MPEGA_DIAG_TRACE_CHECK_MPEG");
  ok &= expect_not_contains(source, "ctrl.check_mpeg = MPEGA_DIAG_TRACE_CHECK_MPEG");
  ok &= expect_contains(source, "#define MPEGA_FIND_SYNC_BUFFER_BYTES");
  ok &= expect_contains(source, "static unsigned char find_sync_buffer");
  ok &= expect_contains(source, "static LONG read_find_sync_buffer");
  ok &= expect_contains(source, "OpenLibrary((CONST_STRPTR)library_name, 0)");
  ok &= expect_contains(source, "MPEGA_find_sync((BYTE *)find_sync_buffer");
  ok &= expect_contains(source, "MPEGA_open((char *)input_path, &ctrl)");
  ok &= expect_contains(source, "if (stream_info)");
  ok &= expect_contains(source, "stream->ms_duration");
  ok &= expect_contains(source, "MPEGA_seek(stream, (ULONG)seek_ms)");
  ok &= expect_contains(source, "MPEGA_seek failed");
  ok &= expect_contains(source, "MPEGA_decode_frame(stream, pcm)");
  ok &= expect_contains(source, "MPEGA_time(stream, &time_ms)");
  ok &= expect_contains(source, "MPEGA_close(stream)");
  ok &= expect_contains(source, "CloseLibrary(MPEGABase)");
  ok &= expect_contains(source, "static ULONG mpega_smoke_bs_access");
  ok &= expect_contains(source, "static struct Hook mpega_smoke_bs_access_hook");
  ok &= expect_contains(source, "ctrl.bs_access = &mpega_smoke_bs_access_hook;");
  ok &= expect_contains(source, "ctrl.layer_1_2.force_mono = 1;");
  ok &= expect_contains(source, "ctrl.layer_3.force_mono = 1;");
  ok &= expect_contains(source, "int check_mpeg = 1;");
  ok &= expect_contains(source, "int expect_open_fail = 0;");
  ok &= expect_contains(source, "int find_sync_only = 0;");
  ok &= expect_contains(source, "int null_api_check = 0;");
  ok &= expect_contains(source, "check_mpeg = 0;");
  ok &= expect_contains(source, "expect_open_fail = 1;");
  ok &= expect_contains(source, "find_sync_only = 1;");
  ok &= expect_contains(source, "null_api_check = 1;");
  ok &= expect_contains(source, "if (!input_path && !null_api_check)");
  ok &= expect_contains(source, "if (null_api_check)");
  ok &= expect_contains(source, "ctrl.check_mpeg = (WORD)check_mpeg;");
  ok &= expect_contains(source, "unsigned long freq_div = 1UL;");
  ok &= expect_contains(source, "unsigned long freq_max = 0UL;");
  ok &= expect_contains(source, "unsigned long scale_percent = 100UL;");
  ok &= expect_contains(source, "unsigned long stream_buffer_size = 0UL;");
  ok &= expect_contains(source, "int freq_div_requested = 0;");
  ok &= expect_contains(source, "int freq_max_requested = 0;");
  ok &= expect_contains(source, "int scale_requested = 0;");
  ok &= expect_contains(source, "int stream_buffer_size_requested = 0;");
  ok &= expect_contains(source, "freq_div != 1UL && freq_div != 2UL && freq_div != 4UL");
  ok &= expect_contains(source, "if (!parse_ulong(argv[++i], &freq_max))");
  ok &= expect_not_contains(source, "freq_max == 0UL");
  ok &= expect_contains(source, "scale_percent == 0UL || scale_percent > 10000UL");
  ok &= expect_not_contains(source, "scale_percent > 800UL");
  ok &= expect_contains(source, "scale_requested = 1;");
  ok &= expect_contains(source, "stream_buffer_size_requested = 1;");
  ok &= expect_contains(source, "ctrl.stream_buffer_size = (LONG)stream_buffer_size;");
  ok &= expect_not_contains(source, "ctrl.stream_buffer_size = 8192;");
  ok &= expect_contains(source, "MPEGA_scale(stream, (LONG)scale_percent)");
  ok &= expect_contains(source, "MPEGA_scale failed");
  ok &= expect_contains(source, "ctrl.layer_1_2.mono.freq_div = (WORD)freq_div;");
  ok &= expect_contains(source, "ctrl.layer_1_2.stereo.freq_div = (WORD)freq_div;");
  ok &= expect_contains(source, "ctrl.layer_3.mono.freq_div = (WORD)freq_div;");
  ok &= expect_contains(source, "ctrl.layer_3.stereo.freq_div = (WORD)freq_div;");
  ok &= expect_contains(source, "ctrl.layer_1_2.mono.freq_div = 0;");
  ok &= expect_contains(source, "ctrl.layer_1_2.mono.freq_max = (LONG)freq_max;");
  ok &= expect_contains(source, "ctrl.layer_1_2.stereo.freq_max = (LONG)freq_max;");
  ok &= expect_contains(source, "ctrl.layer_3.mono.freq_max = (LONG)freq_max;");
  ok &= expect_contains(source, "ctrl.layer_3.stereo.freq_max = (LONG)freq_max;");
  ok &= expect_contains(source, "MPEGA_BSFUNC_OPEN");
  ok &= expect_contains(source, "MPEGA_BSFUNC_READ");
  ok &= expect_contains(source, "MPEGA_BSFUNC_SEEK");
  ok &= expect_contains(source, "MPEGA_BSFUNC_CLOSE");
  ok &= expect_contains(source, "access->data.open.stream_name");
  ok &= expect_contains(source, "access->data.open.buffer_size");
  ok &= expect_contains(source, "access->data.open.stream_size = end_pos;");
  ok &= expect_contains(source, "Read(file, access->data.read.buffer");
  ok &= expect_contains(source, "Seek(file, access->data.seek.abs_byte_seek_pos");
  ok &= expect_contains(source, "pcm_left[MPEGA_PCM_SIZE]");
  ok &= expect_contains(source, "pcm_right[MPEGA_PCM_SIZE]");
  ok &= expect_contains(source, "write_wav_header");
  ok &= expect_contains(source, "patch_wav_header");
  ok &= expect_contains(source, "RIFF");
  ok &= expect_contains(source, "const unsigned char *left_bytes = (const unsigned char *)left;");
  ok &= expect_contains(source, "const unsigned char *right_bytes = (const unsigned char *)right;");
  ok &= expect_contains(source, "dst[0] = left_bytes[1];");
  ok &= expect_contains(source, "dst[1] = left_bytes[0];");
  ok &= expect_contains(source, "dst[2] = right_bytes[1];");
  ok &= expect_contains(source, "dst[3] = right_bytes[0];");
  ok &= expect_contains(source, "left_bytes += 2;");
  ok &= expect_contains(source, "right_bytes += 2;");
  ok &= expect_not_contains(source, "store_wav_sample(dst, left[i]);");
  ok &= expect_not_contains(source, "store_wav_sample(dst, right[i]);");
  ok &= expect_contains(source, "#define MPEGA_OUTPUT_BUFFER_BYTES");
  ok &= expect_contains(source, "typedef struct MPEGAOutputBuffer");
  ok &= expect_contains(source, "mpega_output_write");
  ok &= expect_contains(source, "mpega_output_flush");
  ok &= expect_contains(source, "write_frame(output_file");
  ok &= expect_contains(source, "wav_output");
  ok &= expect_contains(source, "--all is equivalent to --frames 0");
  ok &= expect_contains(source, "frame_limit = 0UL;");
  ok &= expect_contains(source, "output_bytes");
  ok &= expect_contains(source, "output_flushes");
  ok &= expect_contains(source, "decode_ticks");
  ok &= expect_contains(source, "write_ticks");
  ok &= expect_contains(source, "stats:");
  ok &= expect_contains(source, "mpega smoke ok:");

  free(source);
  return ok ? 0 : 1;
}
