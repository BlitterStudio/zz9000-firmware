/*
 * Checks that important AmigaOS deliverables are included in both m68k build
 * script variants.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
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

static int expect_contains(const char *script, const char *script_name,
                           const char *needle)
{
  if (strstr(script, needle)) {
    return 1;
  }

  printf("%s: missing %s\n", script_name, needle);
  return 0;
}

static int expect_not_contains(const char *script, const char *script_name,
                               const char *needle)
{
  if (!strstr(script, needle)) {
    return 1;
  }

  printf("%s: unexpected %s\n", script_name, needle);
  return 0;
}

static int check_script(const char *path, const char *name)
{
  char *script;
  int ok;

  script = read_file(path);
  if (!script) {
    printf("failed to read %s\n", path);
    return 0;
  }

  ok = 1;
  ok &= expect_contains(
      script, name,
      "Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio");
  ok &= expect_contains(script, name,
                        "SPDX-License-Identifier: GPL-3.0-or-later");
  ok &= expect_not_contains(script, name, "tools/zz9k-event-wait.c");
  ok &= expect_not_contains(script, name, "build/m68k/zz9k-event-wait.o");
  ok &= expect_contains(script, name, "tools/zz9k-libtest.c");
  ok &= expect_contains(script, name, "tools/zz9k-libasync.c");
  ok &= expect_contains(script, name, "tools/zz9k-libbatch.c");
  ok &= expect_contains(script, name, "tools/zz9k-libcancel.c");
  ok &= expect_contains(script, name, "tools/zz9k-libwait.c");
  ok &= expect_contains(script, name, "tools/zz9k-libwaitbatch.c");
  ok &= expect_contains(script, name, "tools/zz9k-libevent.c");
  ok &= expect_contains(script, name, "tools/zz9k-libeventbatch.c");
  ok &= expect_contains(script, name, "tools/zz9k-libeventcb.c");
  ok &= expect_contains(script, name, "tools/zz9k-libeventlife.c");
  ok &= expect_contains(script, name, "tools/zz9k-libeventports.c");
  ok &= expect_contains(script, name, "tools/zz9k-libeventcancel.c");
  ok &= expect_contains(script, name, "tools/zz9k-libsmoke.c");
  ok &= expect_contains(script, name, "tools/zz9k-libdecode.c");
  ok &= expect_contains(script, name, "tools/zz9k-hash.c");
  ok &= expect_contains(script, name, "tools/zz9k-inflate.c");
  ok &= expect_contains(script, name, "tools/zz9k-archive.c");
  ok &= expect_contains(script, name, "tools/zz9k-irqtest.c");
  ok &= expect_contains(script, name, "tools/zz9k-chacha.c");
  ok &= expect_contains(script, name, "tools/zz9k-aead.c");
  ok &= expect_contains(script, name, "tools/zz9k-crypto-soft.c");
  ok &= expect_contains(script, name, "tools/zz9k-cryptobench.c");
  ok &= expect_contains(script, name, "tools/zz9k-m68kbench.c");
  ok &= expect_contains(script, name, "tools/zz9k-mp3.c");
  ok &= expect_contains(script, name, "tools/zzplay-audio.c");
  ok &= expect_contains(script, name, "tools/zzplay-mhi.c");
  ok &= expect_contains(script, name, "tools/zzplay-mp3.c");
  ok &= expect_contains(script, name, "tools/zzplay-mp3-transport.c");
  ok &= expect_contains(script, name, "tools/zzplay-core.c");
  ok &= expect_contains(script, name, "tools/zzplay-options.c");
  ok &= expect_contains(script, name, "tools/zzplay-probe.c");
  ok &= expect_contains(script, name, "tools/zzplay-stats.c");
  ok &= expect_contains(script, name, "tools/zzplay-stream.c");
  ok &= expect_contains(script, name, "tools/zzplay-video.c");
  ok &= expect_contains(script, name, "tools/zzplay.c");
  ok &= expect_contains(script, name, "tools/zz9k-mpega-smoke.c");
  ok &= expect_contains(script, name, "amiga/mpega/mpega_resident.c");
  ok &= expect_not_contains(script, name, "mpega.library.zz9k");
  ok &= expect_contains(script, name, "tools/zz9k-image-window.c");
  ok &= expect_contains(script, name, "tools/zz9k-picture-viewer.c");
  ok &= expect_contains(script, name, "build/m68k/zz9k-picture-viewer.o");
  ok &= expect_contains(script, name, "tools/zz9k-jpeg.c");
  ok &= expect_contains(script, name, "-DZZ9K_JPEG_NO_MAIN=1");
  ok &= expect_contains(script, name, "build/m68k/zz9k-jpeg-view.o");
  ok &= expect_contains(script, name,
                        "build/m68k/zz9k-picture-viewer.o "
                        "tools/zz9k-png.c");
  ok &= expect_contains(script, name, "tools/zz9k-png.c");
  ok &= expect_contains(script, name, "-DZZ9K_PNG_NO_MAIN=1");
  ok &= expect_contains(script, name, "build/m68k/zz9k-png-view.o");
  ok &= expect_contains(script, name, "tools/zz9k-view.c");
  ok &= expect_contains(script, name,
                        "build/m68k/zz9k-jpeg-view.o "
                        "build/m68k/zz9k-png-view.o "
                        "tools/zz9k-view.c");
  ok &= expect_contains(script, name, "tools/zz9k-dtprobe.c");
  ok &= expect_contains(script, name,
                        "amiga/datatypes/zz9k_picture_datatype.c");
  ok &= expect_contains(script, name, "build/zz9k-picture.datatype");
  ok &= expect_not_contains(script, name, "-Wl,--gc-sections");
  ok &= expect_contains(script, name, "tools/zz9k-surfaceops.c");
  ok &= expect_contains(script, name,
                        "examples/amiga-library/zz9k-library-demo.c");
  ok &= expect_contains(script, name,
                        "examples/amiga-jpeg-stream/zz9k-jpeg-stream-demo.c");
  ok &= expect_contains(script, name,
                        "examples/amiga-typed-decode/"
                        "zz9k-typed-decode-demo.c");
  ok &= expect_contains(script, name,
                        "examples/amiga-crypto/zz9k-crypto-demo.c");
  ok &= expect_contains(script, name, "build/zz9k-libsmoke");
  ok &= expect_contains(script, name, "build/zz9k-libcancel");
  ok &= expect_contains(script, name, "build/zz9k-libwait");
  ok &= expect_contains(script, name, "build/zz9k-libwaitbatch");
  ok &= expect_contains(script, name, "build/zz9k-libevent");
  ok &= expect_contains(script, name, "build/zz9k-libeventbatch");
  ok &= expect_contains(script, name, "build/zz9k-libeventcb");
  ok &= expect_contains(script, name, "build/zz9k-libeventlife");
  ok &= expect_contains(script, name, "build/zz9k-libeventports");
  ok &= expect_contains(script, name, "build/zz9k-libeventcancel");
  ok &= expect_contains(script, name, "build/zz9k-libdecode");
  ok &= expect_contains(script, name, "build/zz9k-library-demo");
  ok &= expect_contains(script, name, "build/zz9k-jpeg-stream-demo");
  ok &= expect_contains(script, name, "build/zz9k-typed-decode-demo");
  ok &= expect_contains(script, name, "build/zz9k-crypto-demo");
  ok &= expect_contains(script, name, "build/zz9k-hash");
  ok &= expect_contains(script, name, "build/zz9k-inflate");
  ok &= expect_contains(script, name, "build/zz9k-archive");
  ok &= expect_contains(script, name, "build/zz9k-irqtest");
  ok &= expect_contains(script, name, "build/zz9k-chacha");
  ok &= expect_contains(script, name, "build/zz9k-aead");
  ok &= expect_contains(script, name, "build/zz9k-cryptobench");
  ok &= expect_contains(script, name, "build/zz9k-m68kbench");
  ok &= expect_contains(script, name, "build/zz9k-mp3");
  ok &= expect_contains(script, name, "build/zzplay");
  ok &= expect_contains(script, name, "build/zz9k-mpega-smoke");
  ok &= expect_contains(script, name, "build/mpega.library");
  ok &= expect_not_contains(script, name, "build/mpega.library.zz9k");
  ok &= expect_contains(script, name, "build/zz9k-jpeg");
  ok &= expect_contains(script, name, "build/zz9k-png");
  ok &= expect_contains(script, name, "build/zz9k-view");
  ok &= expect_contains(script, name, "build/zz9k-dtprobe");
  ok &= expect_contains(script, name, "build/zz9k-picture.datatype");
  ok &= expect_contains(script, name, "build/zz9k-surfaceops");

  free(script);
  return ok;
}

int main(int argc, char **argv)
{
  int ok;

  if (argc != 3) {
    printf("usage: %s <build-m68k-amigaos.ps1> <build-m68k-amigaos.sh>\n",
           argv[0]);
    return 2;
  }

  ok = 1;
  ok &= check_script(argv[1], "build-m68k-amigaos.ps1");
  ok &= check_script(argv[2], "build-m68k-amigaos.sh");

  return ok ? 0 : 1;
}
