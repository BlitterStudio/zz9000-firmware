/*
 * Source guard for zz9k-hash CLI algorithm selection.
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
    printf("usage: %s <tools/zz9k-hash.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "--alg");
  ok &= expect_contains(source, "sha1");
  ok &= expect_contains(source, "sha256");
  ok &= expect_contains(source, "poly1305");
  ok &= expect_contains(source, "ZZ9K_CRYPTO_HASH_SHA1");
  ok &= expect_contains(source, "ZZ9K_CRYPTO_HASH_SHA256");
  ok &= expect_contains(source, "ZZ9K_CRYPTO_HASH_POLY1305");
  ok &= expect_contains(source, "#include \"zz9k/crypto.h\"");
  ok &= expect_contains(source, "zz9k_crypto_digest_length");
  ok &= expect_contains(source, "zz9k_crypto_build_hash_desc");
  ok &= expect_contains(source, "zz9k_crypto_build_hmac_desc");
  ok &= expect_contains(source, "zz9k_crypto_build_poly1305_desc");
  ok &= expect_contains(source, "expected_sha1_abc");
  ok &= expect_contains(source, "expected_sha256_abc");
  ok &= expect_contains(source, "expected_poly1305_rfc8439");
  ok &= expect_contains(source, "poly1305_key_rfc8439");

  free(source);
  return ok ? 0 : 1;
}
