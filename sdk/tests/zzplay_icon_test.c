/*
 * Validates the committed Workbench icons: structure, and that every
 * ToolType they advertise is one the player actually understands.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/zzplay-options.h"

#define WB_TOOL 3
#define WB_PROJECT 4
#define ICON_WIDTH 46
#define ICON_HEIGHT 46
#define ICON_DEPTH 2

static unsigned char *icon;
static long icon_length;

static unsigned long be32(long offset)
{
  const unsigned char *p = icon + offset;

  return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
         ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static unsigned be16(long offset)
{
  const unsigned char *p = icon + offset;

  return (unsigned)((p[0] << 8) | p[1]);
}

static int load(const char *path)
{
  FILE *file = fopen(path, "rb");

  if (!file) {
    return 0;
  }
  if (fseek(file, 0L, SEEK_END) != 0 || (icon_length = ftell(file)) < 0L ||
      fseek(file, 0L, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }
  icon = (unsigned char *)malloc((size_t)icon_length);
  if (!icon) {
    fclose(file);
    return 0;
  }
  if (fread(icon, 1U, (size_t)icon_length, file) != (size_t)icon_length) {
    fclose(file);
    return 0;
  }
  fclose(file);
  return 1;
}

/* Walk one Image: 20-byte header plus planar rows padded to 16-bit words. */
static long skip_image(long offset)
{
  unsigned width;
  unsigned height;
  unsigned depth;
  long bytes;

  if (offset + 20L > icon_length) {
    return -1;
  }
  width = be16(offset + 4L);
  height = be16(offset + 6L);
  depth = be16(offset + 8L);
  if (width != ICON_WIDTH || height != ICON_HEIGHT || depth != ICON_DEPTH) {
    return -1;
  }
  bytes = (long)(((width + 15U) / 16U) * 2U) * (long)height * (long)depth;
  return offset + 20L + bytes;
}

static long skip_string(long offset)
{
  unsigned long length;

  if (offset + 4L > icon_length) {
    return -1;
  }
  length = be32(offset);
  if (length == 0UL || offset + 4L + (long)length > icon_length) {
    return -1;
  }
  /* Amiga strings in icons are stored NUL-terminated. */
  if (icon[offset + 4L + (long)length - 1L] != '\0') {
    return -1;
  }
  return offset + 4L + (long)length;
}

static int check(const char *path, int expect_type, int expect_default_tool)
{
  long offset;
  unsigned long render;
  unsigned long select;
  unsigned long default_tool;
  unsigned long tooltypes;
  int type;

  if (!load(path)) {
    printf("cannot read %s\n", path);
    return 1;
  }
  if (icon_length < 78L) return 2;
  if (be16(0L) != 0xE310U) return 3;
  if (be16(2L) != 1U) return 4;
  /* struct Gadget starts at +4: ga_Next(4) ga_LeftEdge(2) ga_TopEdge(2)
   * ga_Width(2) ga_Height(2) ... so width is at +12 and the two render
   * pointers at +22 and +26. */
  if (be16(12L) != ICON_WIDTH || be16(14L) != ICON_HEIGHT) return 5;
  render = be32(22L);
  select = be32(26L);
  if (render == 0UL) return 6;
  type = icon[48];
  if (type != expect_type) return 7;
  default_tool = be32(50L);
  tooltypes = be32(54L);
  if (expect_default_tool && default_tool == 0UL) return 8;
  if (!expect_default_tool && default_tool != 0UL) return 9;
  if (tooltypes == 0UL) return 10;

  offset = 78L;
  offset = skip_image(offset);
  if (offset < 0L) return 11;
  if (select != 0UL) {
    offset = skip_image(offset);
    if (offset < 0L) return 12;
  }
  if (default_tool != 0UL) {
    offset = skip_string(offset);
    if (offset < 0L) return 13;
  }

  /* ToolTypes: a LONG of (count + 1) * 4, then each entry. */
  {
    unsigned long header;
    unsigned long count;
    unsigned long i;
    ZZPlayOptions options;

    if (offset + 4L > icon_length) return 14;
    header = be32(offset);
    if (header < 4UL || (header % 4UL) != 0UL) return 15;
    count = header / 4UL - 1UL;
    if (count == 0UL) return 16;
    offset += 4L;
    zzplay_options_init(&options, ZZPLAY_LAUNCH_WORKBENCH);
    for (i = 0UL; i < count; i++) {
      const char *text;
      long next;

      if (offset + 4L > icon_length) return 17;
      text = (const char *)(icon + offset + 4L);
      next = skip_string(offset);
      if (next < 0L) return 18;
      /* Every ToolType the icon ships must be one the player understands.
       * They are shipped disabled, so applying them must be inert and must
       * never report an error. */
      if (!zzplay_options_apply_tooltype(&options, text)) return 19;
      if (text[0] != '(') return 20;
      {
        /* Enabling it (dropping the parentheses) must also be accepted,
         * which is what proves the icon documents real options. */
        char enabled[64];
        size_t length = strlen(text);
        ZZPlayOptions probe;

        if (length < 3U || length - 2U >= sizeof(enabled)) return 21;
        if (text[length - 1U] != ')') return 22;
        memcpy(enabled, text + 1, length - 2U);
        enabled[length - 2U] = '\0';
        zzplay_options_init(&probe, ZZPLAY_LAUNCH_WORKBENCH);
        if (!zzplay_options_apply_tooltype(&probe, enabled)) return 23;
        if (zzplay_options_key_from_tooltype(enabled, 0) ==
            ZZPLAY_OPT_NONE)
          return 24;
      }
      offset = next;
    }
    /* Nothing unaccounted for: a trailing surprise means the layout is
     * wrong somewhere above. */
    if (offset != icon_length) return 25;
  }
  free(icon);
  icon = 0;
  return 0;
}

int main(int argc, char **argv)
{
  int rc;

  if (argc != 3) {
    printf("usage: %s <zzplay.info> <zzplay-project.info>\n", argv[0]);
    return 2;
  }
  rc = check(argv[1], WB_TOOL, 0);
  if (rc != 0) { printf("tool icon check %d\n", rc); return 30 + rc; }
  rc = check(argv[2], WB_PROJECT, 1);
  if (rc != 0) { printf("project icon check %d\n", rc); return 70 + rc; }
  printf("zzplay_icon_test: all checks passed\n");
  return 0;
}
