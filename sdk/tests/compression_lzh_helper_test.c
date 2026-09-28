#include "zz9k/compression.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL: %s\n", #c); fails++; } }while(0)

int main(void) {
  const uint32_t algos[4] = { ZZ9K_COMPRESSION_LH1, ZZ9K_COMPRESSION_LH5,
                              ZZ9K_COMPRESSION_LH6, ZZ9K_COMPRESSION_LH7 };
  int i;
  CHECK(ZZ9K_COMPRESSION_LH1 == 7);
  CHECK(ZZ9K_COMPRESSION_LH5 == 8);
  CHECK(ZZ9K_COMPRESSION_LH6 == 9);
  CHECK(ZZ9K_COMPRESSION_LH7 == 10);
  CHECK(ZZ9K_SERVICE_FLAG_CODEC_LZH == (1U << 29));
  for (i = 0; i < 4; i++) {
    CHECK(zz9k_compression_algorithm_known(algos[i]) == 1);
    CHECK(zz9k_compression_required_service_flags(algos[i]) ==
          ZZ9K_SERVICE_FLAG_CODEC_LZH);
    CHECK(zz9k_compression_required_feed_service_flags(algos[i]) == 0U);
  }
  printf(fails ? "%d FAILED\n" : "OK\n", fails);
  return fails ? 1 : 0;
}
