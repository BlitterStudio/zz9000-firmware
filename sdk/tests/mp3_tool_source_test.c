/*
 * Source guard for the ZZ9000 SDK MP3 decode smoke tool.
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

int main(int argc, char **argv)
{
  char *source;
  int ok;

  if (argc != 2) {
    printf("usage: %s <tools/zz9k-mp3.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "#include \"zz9k/audio.h\"");
  ok &= expect_contains(source, "#include \"zz9k/caps.h\"");
  ok &= expect_contains(source, "#include \"zz9k/shared.h\"");
  ok &= expect_contains(source, "ZZ9K_CAP_AUDIO_DECODE");
  ok &= expect_contains(source, "ZZ9K_SERVICE_AUDIO");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_AUDIO_MP3_DECODE");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM");
  ok &= expect_contains(source, "ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE");
  ok &= expect_contains(source, "stream feed stayed backpressured");
  ok &= expect_contains(source, "--stats");
  ok &= expect_contains(source, "--decode-quantum");
  ok &= expect_contains(source, "--feed-chunk");
  ok &= expect_contains(source, "stream stats:");
  ok &= expect_contains(source, "feed_call=");
  ok &= expect_contains(source, "realtime=");
  ok &= expect_contains(source, "audio=");
  ok &= expect_contains(source, "output_write=");
  ok &= expect_contains(source, "STREAM_OUTPUT_CHUNK_BYTES");
  ok &= expect_contains(source, "STREAM_CHUNK_BYTES (64UL * 1024UL)");
  ok &= expect_contains(source, "STREAM_DECODE_QUANTUM_BYTES");
  ok &= expect_contains(source, "DEFAULT_STREAM_PCM_CAPACITY");
  ok &= expect_contains(source, "STREAM_PCM_ACK_BATCH_BYTES (192UL * 1024UL)");
  ok &= expect_contains(source, "mp3_stream_decode_quantum_bytes");
  ok &= expect_contains(source, "mp3_stream_feed_chunk_bytes");
  ok &= expect_contains(source, "mp3_stream_input_room_low");
  ok &= expect_contains(source, "make_stream_input_room");
  ok &= expect_contains(source, "decode_quantum=%lu");
  ok &= expect_contains(source, "feed_chunk=%lu");
  ok &= expect_contains(source, "ZZ9K_MP3_NO_MAIN");
  ok &= expect_contains(source, "mp3_stream_pcm_ack_due");
  ok &= expect_contains(source, "ack_stream_pcm");
  ok &= expect_contains(source, "flush_stream_pcm");
  ok &= expect_contains(source, "pending_ack");
  ok &= expect_contains(source,
                        "pcm_ring.length, 0U, 0U, output_format, 0U,\n"
                        "          decode_quantum, 0U))");
  ok &= expect_contains(source, "fread(chunk, 1U, feed_chunk, input_file)");
  ok &= expect_contains(source, "--wav");
  ok &= expect_contains(source, "write_wav_header");
  ok &= expect_contains(source, "patch_wav_header");
  ok &= expect_contains(source, "RIFF");
  ok &= expect_contains(source, "--wav requires little-endian S16 output");
  ok &= expect_contains(source, "--oneshot");
  ok &= expect_contains(source, "stream_decode_file(ctx, input_path");
  ok &= expect_contains(source, "ZZ9KDiagInfo");
  ok &= expect_contains(source, "zz9k_read_diag");
  ok &= expect_contains(source, "shared_heap_largest_free");
  ok &= expect_contains(source, "one-shot MP3 decode needs shared memory");
  ok &= expect_contains(source, "falling back to streaming MP3 decode");
  ok &= expect_contains(source, "streaming MP3 decode is not advertised");
  ok &= expect_contains(source, "streaming MP3 decode");
  ok &= expect_contains(source, "zz9k_audio_build_decode_desc");
  ok &= expect_contains(source, "zz9k_decode_mp3");
  ok &= expect_contains(source, "ZZ9K_AUDIO_SAMPLE_FORMAT_S16LE");
  ok &= expect_contains(source, "ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE");
  ok &= expect_contains(source, "ZZ9K_AUDIO_DECODE_FLAG_EXPECT_END");
  ok &= expect_contains(source, "bytes_consumed");
  ok &= expect_contains(source, "bytes_written");
  ok &= expect_contains(source, "input=%lu");

  free(source);
  return ok ? 0 : 1;
}
