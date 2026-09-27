/*
 * Checks the AmigaOS SDK package script layout.
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
  ok &= expect_contains(script, name, "Libs");
  ok &= expect_contains(script, name, "Classes");
  ok &= expect_contains(script, name, "DataTypes");
  ok &= expect_contains(script, name, "Storage");
  ok &= expect_contains(script, name, "C");
  ok &= expect_contains(script, name, "Developer");
  ok &= expect_contains(script, name, "Include");
  ok &= expect_contains(script, name, "Include/fd");
  ok &= expect_contains(script, name, "pragmas");
  ok &= expect_contains(script, name, "FD");
  ok &= expect_contains(script, name, "Docs");
  ok &= expect_contains(script, name, "Examples");
  ok &= expect_contains(script, name, "Archives");
  ok &= expect_contains(script, name, "MANIFEST.sha256");
  ok &= expect_contains(script, name, "build/zz9k.library");
  ok &= expect_contains(script, name, "build/zz9k-info");
  ok &= expect_contains(script, name, "build/zz9k-services");
  ok &= expect_contains(script, name, "build/zz9k-bench");
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
  if (strstr(name, ".ps1")) {
    ok &= expect_contains(script, name,
                          "Copy-One \"build/mpega.library\" "
                          "\"Libs/mpega.library\"");
  } else {
    ok &= expect_contains(script, name,
                          "copy_one \"build/mpega.library\" "
                          "\"Libs/mpega.library\"");
  }
  ok &= expect_not_contains(script, name, "mpega.library.zz9k");
  ok &= expect_contains(script, name, "build/zz9k-jpeg");
  ok &= expect_contains(script, name, "build/zz9k-png");
  ok &= expect_contains(script, name, "build/zz9k-view");
  ok &= expect_contains(script, name, "build/zz9k-dtprobe");
  ok &= expect_contains(script, name, "build/zz9k-picture.datatype");
  ok &= expect_contains(script, name, "Classes/DataTypes/zz9k-picture.datatype");
  ok &= expect_contains(script, name,
                        "amiga/datatypes/descriptors/ZZ9000-JPEG.b64");
  ok &= expect_contains(script, name,
                        "amiga/datatypes/descriptors/ZZ9000-PNG.b64");
  ok &= expect_contains(script, name, "Storage/DataTypes/ZZ9000-JPEG");
  ok &= expect_contains(script, name, "Storage/DataTypes/ZZ9000-PNG");
  ok &= expect_contains(script, name,
                        "amiga/datatypes/descriptors/ZZ9000-JPEG.info");
  ok &= expect_contains(script, name,
                        "amiga/datatypes/descriptors/ZZ9000-PNG.info");
  ok &= expect_contains(script, name, "Storage/DataTypes/ZZ9000-JPEG.info");
  ok &= expect_contains(script, name, "Storage/DataTypes/ZZ9000-PNG.info");
  ok &= expect_contains(script, name, "build/zz9k-surfaceops");
  ok &= expect_contains(script, name, "build/zz9k-libdecode");
  ok &= expect_contains(script, name, "build/zz9k-libevent");
  ok &= expect_contains(script, name, "build/zz9k-libeventbatch");
  ok &= expect_contains(script, name, "build/zz9k-libeventcb");
  ok &= expect_contains(script, name, "build/zz9k-libeventlife");
  ok &= expect_contains(script, name, "build/zz9k-libeventports");
  ok &= expect_contains(script, name, "build/zz9k-libeventcancel");
  ok &= expect_contains(script, name, "build/zz9k-library-demo");
  ok &= expect_contains(script, name, "build/zz9k-jpeg-stream-demo");
  ok &= expect_contains(script, name, "build/zz9k-typed-decode-demo");
  ok &= expect_contains(script, name, "build/zz9k-crypto-demo");
  ok &= expect_contains(script, name, "include/zz9k");
  ok &= expect_contains(script, name, "host/include/zz9k");
  ok &= expect_contains(script, name, "amiga/include/proto");
  ok &= expect_contains(script, name, "amiga/include/clib");
  ok &= expect_contains(script, name, "amiga/include/libraries");
  ok &= expect_contains(script, name, "amiga/include/inline");
  ok &= expect_contains(script, name, "amiga/include/pragmas");
  ok &= expect_contains(script, name, "amiga/fd/zz9k_lib.fd");
  ok &= expect_contains(script, name, "amiga/fd/mpega_lib.fd");
  ok &= expect_contains(script, name, "amiga/fd/mpega.fd");
  ok &= expect_contains(script, name, "Developer/Include/fd/mpega.fd");
  ok &= expect_contains(script, name, "docs/zz9k-library.md");
  ok &= expect_contains(script, name, "docs/zz9k-zorro2-services.md");
  ok &= expect_contains(script, name, "docs/zz9k-modules.md");
  ok &= expect_contains(script, name, "docs/zz9k-picture-datatype.md");
  ok &= expect_contains(script, name, "docs/zz9k-68k-offload.md");
  ok &= expect_contains(script, name, "docs/zz9k-release-smoke.md");
  ok &= expect_contains(script, name, "md2guide.py");
  ok &= expect_contains(script, name, "docs/zzplay.md");
  ok &= expect_contains(script, name, "docs/zz9k-amissl-provider.md");
  ok &= expect_contains(script, name, "docs/zz9k-crypto-acceleration.md");
  ok &= expect_contains(script, name,
                        "tests/fixtures/archives/split-deflate.7z.b64");
  ok &= expect_contains(script, name,
                        "tests/fixtures/archives/split-lzma.7z.b64");
  ok &= expect_contains(script, name,
                        "tests/fixtures/archives/split-lzma2.7z.b64");
  ok &= expect_contains(script, name, "Archives/split-deflate.7z");
  ok &= expect_contains(script, name, "Archives/split-lzma.7z");
  ok &= expect_contains(script, name, "Archives/split-lzma2.7z");
  ok &= expect_contains(script, name,
                        "examples/amiga-jpeg-stream/"
                        "zz9k-jpeg-stream-demo.c");
  ok &= expect_contains(script, name,
                        "examples/amiga-typed-decode/"
                        "zz9k-typed-decode-demo.c");
  ok &= expect_contains(script, name,
                        "examples/amiga-crypto/zz9k-crypto-demo.c");
  if (strstr(name, ".ps1")) {
    ok &= expect_contains(script, name, "Get-FileHash");
    ok &= expect_contains(script, name, "ToLowerInvariant");
    ok &= expect_contains(script, name, "Make-Guide \"docs/zzplay.md\" \"ZZPlay\"");
  } else {
    ok &= expect_contains(script, name, "sha256");
    ok &= expect_contains(script, name, "SAFE_ROOT");
    ok &= expect_contains(script, name, "PACKAGE_PARENT");
    ok &= expect_contains(script, name, "docs/zzplay.md:ZZPlay");
  }

  free(script);
  return ok;
}

int main(int argc, char **argv)
{
  int ok;

  if (argc != 3) {
    printf("usage: %s <package-m68k-amigaos.ps1> "
           "<package-m68k-amigaos.sh>\n", argv[0]);
    return 2;
  }

  ok = 1;
  ok &= check_script(argv[1], "package-m68k-amigaos.ps1");
  ok &= check_script(argv[2], "package-m68k-amigaos.sh");

  return ok ? 0 : 1;
}
