/*
 * Source guard for the public crypto hash example.
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
    printf("usage: %s <examples/amiga-crypto/zz9k-crypto-demo.c>\n",
           argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "#include \"proto/zz9k.h\"");
  ok &= expect_contains(source, "#include \"zz9k/caps.h\"");
  ok &= expect_contains(source, "#include \"zz9k/crypto.h\"");
  ok &= expect_contains(source, "#include \"zz9k/text.h\"");
  ok &= expect_contains(source, "ZZ9K_LIBRARY_MIN_REVISION_CRYPTO_AEAD");
  ok &= expect_contains(source, "zz9k_has_capabilities");
  ok &= expect_contains(source, "zz9k_known_capability_count");
  ok &= expect_contains(source, "zz9k_capability_name");
  ok &= expect_contains(source, "ZZ9K_CAP_CRYPTO");
  ok &= expect_contains(source, "ZZ9KAllocShared");
  ok &= expect_contains(source, "ZZ9KFreeShared");
  ok &= expect_contains(source, "ZZ9KCryptoHash(&desc, &result)");
  ok &= expect_contains(source,
                        "ZZ9KCryptoHashBatch(descs, results, 2U, 2U");
  ok &= expect_contains(source, "ZZ9KCryptoStream(&stream_desc, &result)");
  ok &= expect_contains(source, "ZZ9KCryptoAead(&aead_desc, &result)");
  ok &= expect_contains(source, "zz9k_crypto_build_hash_desc");
  ok &= expect_contains(source, "zz9k_crypto_build_chacha20_desc");
  ok &= expect_contains(source,
                        "zz9k_crypto_build_chacha20_poly1305_desc");
  ok &= expect_contains(source, "ZZ9K_CRYPTO_HASH_SHA1");
  ok &= expect_contains(source, "ZZ9K_CRYPTO_HASH_SHA256");
  ok &= expect_contains(source, "ZZ9K_CRYPTO_STREAM_CHACHA20");
  ok &= expect_contains(source, "ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305");
  ok &= expect_contains(source, "ZZ9K_CRYPTO_AEAD_FLAG_DECRYPT");

  free(source);
  return ok ? 0 : 1;
}
