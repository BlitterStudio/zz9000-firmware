/*
 * Source guard for the public SDK library documentation.
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
    printf("usage: %s <docs/zz9k-library.md>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "#define ZZ9K_LIBRARY_REVISION 29");
  ok &= expect_contains(source, "ZZ9K_LIBRARY_MIN_REVISION_CRYPTO_KX");
  ok &= expect_contains(source, "ZZ9K_LIBRARY_MIN_REVISION_QUERY_PALETTE");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_SURFACE_PALETTE_QUERY");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_CRYPTO_X25519");
  ok &= expect_contains(source, "zz9k-services --check-release");
  ok &= expect_contains(source, "release check ok");
  ok &= expect_contains(source, "zz9k-release-smoke.md");
  ok &= expect_contains(source, "package-level runtime smoke pass");
  ok &= expect_contains(source, "SDK v2 is paired with the new SDK-service firmware");
  ok &= expect_contains(source, "pre-service firmware");
  ok &= expect_contains(source, "not a supported runtime");
  ok &= expect_contains(source, "uint32_t output_format;");
  ok &= expect_contains(source, "output_format = zz9k_surface_native_rtg_format();");
  ok &= expect_contains(source, "zz9k_surface_layout(");
  ok &= expect_contains(source, "zz9k_image_build_framebuffer_session_begin_desc");
  ok &= expect_contains(source, "zz9k_surface_is_native_rtg_format(framebuffer.format)");
  ok &= expect_contains(source,
                        "The `zz9k-surfaceops` diagnostic still uses absolute");
  ok &= expect_contains(source,
                        "shared image-window helper requests refresh events");
  ok &= expect_contains(source,
                        "User-facing viewer and DataType output should");
  ok &= expect_contains(source,
                        "enumerate visible layer or damage clip regions");
  ok &= expect_contains(source,
                        "damage-clip list construction");
  ok &= expect_contains(source,
                        "window layer `ClipRect` list");
  ok &= expect_contains(source,
                        "obscured clip rectangles");
  ok &= expect_contains(source,
                        "visible image restore");
  ok &= expect_contains(source,
                        "zz9k-view Work:Pictures/test.jpg Work:Pictures/test.png");
  ok &= expect_contains(source, "standalone ZZ9000 viewer");
  ok &= expect_contains(source, "Space/Right/Down");
  ok &= expect_contains(source, "visible layer clips");
  ok &= expect_not_contains(
      source,
      "detects JPEG or PNG from the file header and runs the matching tool");
  ok &= expect_contains(source, "Library revision 17 adds a resident event dispatcher");
  ok &= expect_contains(source, "ZZ9K_LIBRARY_MIN_REVISION_EVENT_DISPATCHER");
  ok &= expect_contains(source, "zz9k/event_wait.h");
  ok &= expect_contains(source, "ZZ9KCallAsyncMsg()");
  ok &= expect_contains(source, "zz9k_event_wait_msg()");
  ok &= expect_contains(source, "zz9k_event_wait_async_port()");
  ok &= expect_contains(source, "zz9k_event_drain_async_port()");
  ok &= expect_contains(source, "zz9k_event_wait_async_ports()");
  ok &= expect_contains(source, "zz9k_event_drain_async_ports()");
  ok &= expect_contains(source, "Empty reply-port signals are treated as stale");
  ok &= expect_contains(source, "zz9k_event_wait_signal()");
  ok &= expect_contains(source, "Wait()");
  ok &= expect_contains(source, "Callbacks delivered by the dispatcher run in the dispatcher process");
  ok &= expect_contains(source, "Library revision 20 keeps INT6 as the default");
  ok &= expect_contains(source, "ENV:ZZ9K_INT2");
  ok &= expect_not_contains(source, "ENV:ZZ9K_SDK_INT2");
  ok &= expect_contains(source, "## Decompression Jobs");
  ok &= expect_contains(source, "zz9k/compression.h");
  ok &= expect_contains(source, "ZZ9KDecompressDesc");
  ok &= expect_contains(source, "zz9k_compression_build_decompress_desc");
  ok &= expect_contains(source, "ZZ9K_OP_DECOMPRESS");
  ok &= expect_contains(source, "ZZ9K_OP_DECOMPRESS_TEST");
  ok &= expect_contains(source, "ZZ9K_OP_DECOMPRESS_STREAM_BEGIN");
  ok &= expect_contains(source, "ZZ9K_OP_DECOMPRESS_STREAM_READ");
  ok &= expect_contains(source, "ZZ9K_OP_DECOMPRESS_STREAM_CLOSE");
  ok &= expect_contains(source, "ZZ9K_OP_DECOMPRESS_STREAM_FEED");
  ok &= expect_contains(source, "zz9k_decompress_test");
  ok &= expect_contains(source, "zz9k_decompress_stream_begin");
  ok &= expect_contains(source, "zz9k_decompress_stream_feed");
  ok &= expect_contains(source, "decompress-stream");
  ok &= expect_contains(source, "decompress-feed");
  ok &= expect_contains(source, "deflate-feed");
  ok &= expect_contains(source, "zlib-feed");
  ok &= expect_contains(source, "gzip-feed");
  ok &= expect_contains(source, "streamed test path");
  ok &= expect_contains(source, "streamed extraction path");
  ok &= expect_contains(source, "reusable shared input buffer");
  ok &= expect_contains(source, "ZZ9K_COMPRESSION_LZMA_ALONE");
  ok &= expect_contains(source, "ZZ9K_COMPRESSION_LZMA2");
  ok &= expect_contains(source, "zz9k-archive t payload.lzma");
  ok &= expect_contains(source, "zz9k-archive t --capacity 1048576");
  ok &= expect_contains(source, "zz9k-archive x --overwrite");
  ok &= expect_contains(source, "zz9k-archive x --skip-existing");
  ok &= expect_contains(source, "zz9k-archive x --dry-run");
  ok &= expect_contains(source, "zz9k-archive x --match Prefs/");
  ok &= expect_contains(source, "zz9k-archive x --strip-components 1");
  ok &= expect_contains(source, "`output exists, use --overwrite:`");
  ok &= expect_contains(source, "Library revision 22 added MP3 streaming sessions");
  ok &= expect_contains(source, "ZZ9K_LIBRARY_MIN_REVISION_AUDIO_STREAM");
  ok &= expect_contains(source,
                        "ZZ9K_LIBRARY_MIN_REVISION_AUDIO_STREAM_DRAIN");
  ok &= expect_contains(source, "ZZ9K_CAP_AUDIO_STREAM_DRAIN");
  ok &= expect_contains(source, "ZZ9KAudioStreamBegin()");
  ok &= expect_contains(source, "ZZ9KAudioStreamFeed()");
  ok &= expect_contains(source, "ZZ9KAudioStreamRead()");
  ok &= expect_contains(source, "ZZ9KAudioStreamClose()");
  ok &= expect_contains(source,
                        "ZZPlay --audio=mhi Work:Audio/test.mp3");
  ok &= expect_contains(source,
                        "`zz9k-mp3` remains the low-level");
  ok &= expect_not_contains(source, "This prototype is\nvideo-only");
  ok &= expect_contains(source, "`--skip-existing`");
  ok &= expect_contains(source, "reported with an `s` line");
  ok &= expect_contains(source, "`--dry-run`");
  ok &= expect_contains(source, "reported with a `dry` line");
  ok &= expect_contains(source, "`output path is a directory:`");
  ok &= expect_contains(source, "`output path is a file:`");
  ok &= expect_contains(source, "`--match text`");
  ok &= expect_contains(source, "normalized archive path substring");
  ok &= expect_contains(source, "`--strip-components n`");
  ok &= expect_contains(source, "leading path components");
  ok &= expect_contains(source, "LZMA-alone");
  ok &= expect_contains(source, "not a ZIP, LHA, or full 7z parser");
  ok &= expect_contains(source, "LHA level-0 and level-1");
  ok &= expect_contains(source, "`-lhd-` directory entries");
  ok &= expect_contains(source, "Level-1 and level-2");
  ok &= expect_contains(source, "directory-name");
  ok &= expect_contains(source, "extension-header");
  ok &= expect_contains(source, "`-lh1-`");
  ok &= expect_contains(source, "`-lh5-`");
  ok &= expect_contains(source, "`-lh6-`");
  ok &= expect_contains(source, "`-lh7-`");
  ok &= expect_contains(source, "jca02266/lha");
  ok &= expect_contains(source,
                        "Zip64 extra fields that resolve to 32-bit sizes");
  ok &= expect_contains(source, "local-header offsets");
  ok &= expect_contains(source, "EOCD records");
  ok &= expect_contains(source,
                        "empty ZIP archives whose first record is the");
  ok &= expect_contains(source, "backslash path separators");
  ok &= expect_contains(source, "Amiga-style volume/device names");
  ok &= expect_contains(source, "SYS:Prefs");
  ok &= expect_contains(source, "current-directory prefixes");
  ok &= expect_contains(source, "current-directory path components");
  ok &= expect_contains(source, "duplicate slash separators");
  ok &= expect_contains(source, "Gzip header filenames");
  ok &= expect_contains(source, "archive-root metadata entries");
  ok &= expect_contains(source, "7z member names are decoded from UTF-16");
  ok &= expect_contains(source, "PAX `size=` records");
  ok &= expect_contains(source, "base-256 numeric encoding");
  ok &= expect_contains(source, "historical signed checksum");
  ok &= expect_contains(source, "legacy checksum-only tar headers");
  ok &= expect_contains(source, "true empty tar archives");
  ok &= expect_contains(source, "trailing slash name");
  ok &= expect_contains(source, "PAX header size");
  ok &= expect_contains(source, "512-byte fixed");
  ok &= expect_contains(source, "external attributes");
  ok &= expect_contains(source, "zz9k-archive l archive.7z");
  ok &= expect_contains(source, "unencoded 7z headers");
  ok &= expect_contains(source, "7z Copy");
  ok &= expect_contains(source, "Copy-encoded 7z headers");
  ok &= expect_contains(source, "encoded header stream has CRC");
  ok &= expect_contains(source, "Deflate, LZMA, and");
  ok &= expect_contains(source, "header compression enabled");
  ok &= expect_contains(source, "true zero-file 7z archives");
  ok &= expect_contains(source, "7z timestamp and attribute metadata");
  ok &= expect_contains(source, "single-folder 7z Deflate");
  ok &= expect_contains(source, "single-folder 7z LZMA");
  ok &= expect_contains(source, "single-folder 7z LZMA2");
  ok &= expect_contains(source, "multi-substream Deflate/LZMA/LZMA2");
  ok &= expect_contains(source, "splits the decoded bytes");
  ok &= expect_contains(source, "validates CRCs only for selected substreams");
  ok &= expect_contains(source, "streams the archive file directly from disk");
  ok &= expect_contains(source, "converted to the LZMA-alone stream shape");
  ok &= expect_contains(source, "1-byte LZMA2 property prefix");
  ok &= expect_contains(source, "falls back to a one-shot");
  ok &= expect_contains(source, "LZMA2 dictionary properties");
  ok &= expect_contains(source, "7z unsupported layout: ...");
  ok &= expect_contains(source, "complete wrapped payload");
  ok &= expect_contains(source, "7z LZMA no-memory diagnostics");
  ok &= expect_contains(source, "Largest free block");
  ok &= expect_contains(source, "encoded 7z headers");
  ok &= expect_not_contains(source,
                            "Direct-to-RTG decode should use `ZZ9K_SURFACE_FORMAT_BGRA8888`");
  ok &= expect_not_contains(source,
                            "allocates an ARM-local `ZZ9K_SURFACE_FORMAT_BGRA8888` surface");
  ok &= expect_not_contains(source,
                            "ZZ9K_SURFACE_FORMAT_BGRA8888, 0) &&");

  free(source);
  return ok ? 0 : 1;
}
