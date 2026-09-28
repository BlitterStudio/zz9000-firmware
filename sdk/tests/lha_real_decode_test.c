/*
 * Oracle test: decode the real .lzh fixtures embedded in
 * tests/lha_real_fixtures.h with the SDK's proven lha-unix decoder and
 * confirm the output is byte-exact with the known-good plaintext and the
 * CRC-16 matches the archive's stored value.
 *
 * This validates the fixtures themselves (sliced from real Aminet archives)
 * so the firmware end-to-end test can safely reuse them as its decode
 * oracle.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k_lha_unix.h"
#include "lha_real_fixtures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int cond, const char *msg)
{
  if (cond) {
    printf("ok   %s\n", msg);
  } else {
    printf("FAIL %s\n", msg);
    failures++;
  }
}

/*
 * Decode one fixture via tmpfiles. Returns whatever
 * zz9k_lha_unix_decode_method() returned (1 on decoder success). On return,
 * *out_buf is a malloc'd buffer of fixture->uncompressed_size bytes holding
 * whatever was produced (caller frees it) and *out_len is the number of
 * bytes actually read back out of the output tmpfile.
 */
static int decode_fixture(const ZZ9KLhaFixture *fx, unsigned char **out_buf,
                           long *out_len, uint16_t *out_crc)
{
  FILE *in, *out;
  int ok;
  long n;

  *out_buf = (unsigned char *)malloc(fx->uncompressed_size ? fx->uncompressed_size : 1);
  if (!*out_buf) {
    fprintf(stderr, "lha_real_decode_test: out of memory\n");
    exit(1);
  }

  in = tmpfile();
  out = tmpfile();
  if (!in || !out) {
    fprintf(stderr, "lha_real_decode_test: tmpfile() failed\n");
    exit(1);
  }

  fwrite(fx->compressed, 1, fx->compressed_size, in);
  fflush(in);
  fseek(in, 0, SEEK_SET);

  *out_crc = 0;
  ok = zz9k_lha_unix_decode_method(in, out, fx->uncompressed_size,
                                    fx->compressed_size, fx->crc16, 1,
                                    out_crc, (int)fx->method);

  fflush(out);
  fseek(out, 0, SEEK_SET);
  n = (long)fread(*out_buf, 1, fx->uncompressed_size, out);

  fclose(in);
  fclose(out);

  *out_len = n;
  return ok;
}

int main(void)
{
  unsigned i;

  for (i = 0; i < ZZ9K_LHA_FIXTURE_COUNT; i++) {
    const ZZ9KLhaFixture *fx = &zz9k_lha_fixtures[i];
    unsigned char *out_buf = NULL;
    long out_len = 0;
    uint16_t actual_crc = 0;
    int ok;
    char msg[256];

    ok = decode_fixture(fx, &out_buf, &out_len, &actual_crc);

    snprintf(msg, sizeof msg, "%s: decode_method returns success", fx->name);
    check(ok == 1, msg);

    snprintf(msg, sizeof msg,
             "%s: actual crc16 0x%04x matches expected 0x%04x", fx->name,
             actual_crc, fx->crc16);
    check(actual_crc == fx->crc16, msg);

    snprintf(msg, sizeof msg, "%s: decoded %ld of %u expected bytes",
             fx->name, out_len, fx->uncompressed_size);
    check(out_len == (long)fx->uncompressed_size, msg);

    snprintf(msg, sizeof msg,
             "%s: decoded bytes are byte-exact with expected plaintext",
             fx->name);
    check(out_len == (long)fx->uncompressed_size &&
              memcmp(out_buf, fx->expected, fx->uncompressed_size) == 0,
          msg);

    free(out_buf);
  }

  if (failures) {
    printf("lha_real_decode_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
