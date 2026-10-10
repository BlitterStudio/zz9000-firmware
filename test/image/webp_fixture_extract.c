/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "webp_fixtures.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
  const uint8_t *bytes;
  size_t length;
  FILE *output;

  if (argc != 3) return 2;
  if (strcmp(argv[1], "lossy") == 0) {
    bytes = webp_lossy_3x5;
    length = sizeof(webp_lossy_3x5);
  } else if (strcmp(argv[1], "alpha") == 0) {
    bytes = webp_alpha_3x4;
    length = sizeof(webp_alpha_3x4);
  } else if (strcmp(argv[1], "animation") == 0) {
    bytes = webp_animation_offset_6x4;
    length = sizeof(webp_animation_offset_6x4);
  } else {
    return 3;
  }
  output = fopen(argv[2], "wb");
  if (!output || fwrite(bytes, 1U, length, output) != length) return 4;
  return fclose(output) == 0 ? 0 : 5;
}
