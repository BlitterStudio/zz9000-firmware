/*
 * zz9k-mediaout logic: command line, MPEG probing, geometry, checksum and the
 * conversion of finished pictures back to pixels.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-mediaout-logic.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(expr) do { \
  checks++; \
  if (!(expr)) { \
    failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
  } \
} while (0)

/* The arguments up to the first null make the command line. */
static int parse_args(ZZ9KMediaOutOptions *o, const char *const *args)
{
  char *argv[16];
  int argc = 0;

  argv[argc++] = (char *)"zz9k-mediaout";
  while (args[argc - 1] && argc < 15) {
    argv[argc] = (char *)args[argc - 1];
    argc++;
  }
  return zz9k_mediaout_parse(argc, argv, o);
}
#define parse(o, ...) parse_args(o, (const char *const[]){ __VA_ARGS__, 0 })

static void test_parse(void)
{
  ZZ9KMediaOutOptions o;
  uint32_t w, h;

  CHECK(parse(&o, "a.mpg", 0, 0, 0, 0, 0));
  CHECK(o.format == ZZ9K_SURFACE_FORMAT_RGB565 && o.depth == 8U &&
        o.width == 0U && o.height == 0U && o.flags == 0U &&
        o.count == 50U && o.checksum == 1 && !o.single_slot && !o.window);
  CHECK(strcmp(o.path, "a.mpg") == 0);
  CHECK(parse(&o, "-f", "planar", "-d", "5", "a.mpg", "-i"));
  CHECK(o.format == ZZ9K_SURFACE_FORMAT_PLANAR && o.depth == 5U &&
        (o.flags & ZZ9K_MEDIA_OUTPUT_PLANAR_INTERLEAVED) != 0U);
  CHECK(parse(&o, "a.mpg", "-s", "320x200", "-c", "7", "-o"));
  CHECK(o.width == 320U && o.height == 200U && o.count == 7U &&
        (o.flags & ZZ9K_MEDIA_OUTPUT_DITHER_ORDERED) != 0U);
  CHECK(parse(&o, "-n", "-x", "-1", "-w", "a.mpg", 0));
  CHECK((o.flags & ZZ9K_MEDIA_OUTPUT_SCALE_NEAREST) != 0U && !o.checksum &&
        o.single_slot && o.window);
  CHECK(parse(&o, "-f", "index8", "-H", "a.mpg", 0, 0));
  CHECK((o.flags & ZZ9K_MEDIA_OUTPUT_PALETTE_HOST) != 0U);
  CHECK(parse(&o, "-f", "rgb555", "a.mpg", 0, 0, 0) &&
        o.format == ZZ9K_SURFACE_FORMAT_RGB555);

  /* everything the usage text should answer */
  CHECK(!parse(&o, 0, 0, 0, 0, 0, 0));                  /* no file */
  CHECK(!parse(&o, "a.mpg", "b.mpg", 0, 0, 0, 0));      /* two files */
  CHECK(!parse(&o, "-h", "a.mpg", 0, 0, 0, 0));
  CHECK(!parse(&o, "-z", "a.mpg", 0, 0, 0, 0));
  CHECK(!parse(&o, "-f", "bgra", "a.mpg", 0, 0, 0));
  CHECK(!parse(&o, "-f", 0, 0, 0, 0, 0));
  CHECK(!parse(&o, "-d", "0", "a.mpg", 0, 0, 0));
  CHECK(!parse(&o, "-d", "9", "a.mpg", 0, 0, 0));
  CHECK(!parse(&o, "-d", "x", "a.mpg", 0, 0, 0));
  CHECK(!parse(&o, "-c", "0", "a.mpg", 0, 0, 0));
  CHECK(!parse(&o, "-s", "320", "a.mpg", 0, 0, 0));
  CHECK(!parse(&o, "-s", "320x", "a.mpg", 0, 0, 0));
  CHECK(!parse(&o, "-s", "0x200", "a.mpg", 0, 0, 0));
  CHECK(!parse(&o, "-s", "1921x200", "a.mpg", 0, 0, 0));
  CHECK(!parse(&o, "-s", "320x1089", "a.mpg", 0, 0, 0));
  CHECK(!parse(&o, "-i", "a.mpg", 0, 0, 0, 0));         /* not planar */
  CHECK(!parse(&o, "-f", "rgb565", "-H", "a.mpg", 0, 0));
  CHECK(!parse(&o, "-fx", "a.mpg", 0, 0, 0, 0));

  CHECK(zz9k_mediaout_parse_size("640X480", &w, &h) && w == 640U && h == 480U);
  CHECK(!zz9k_mediaout_parse_size("", &w, &h));
  CHECK(!zz9k_mediaout_parse_size(0, &w, &h));
}

static void test_mpeg_info(void)
{
  /* sequence header for 352x288 */
  static const uint8_t mpeg1[] = {
    0x00, 0x00, 0x01, 0xb3, 0x16, 0x01, 0x20, 0x13, 0xff, 0xff, 0xe0, 0xa0,
    0x00, 0x00, 0x01, 0xb8
  };
  /* the same with a sequence extension, as MPEG-2 */
  static const uint8_t mpeg2[] = {
    0x00, 0x00, 0x01, 0xb3, 0x2d, 0x02, 0x40, 0x13, 0xff, 0xff, 0xe0, 0xa0,
    0x00, 0x00, 0x01, 0xb5, 0x14, 0x8a, 0x00, 0x01, 0x00, 0x00
  };
  static const uint8_t ps[] = { 0x00, 0x00, 0x01, 0xba, 0x44 };
  static uint8_t shifted[40];
  ZZ9KMediaOutMpegInfo info;

  CHECK(zz9k_mediaout_mpeg_info(mpeg1, sizeof(mpeg1), &info));
  CHECK(info.width == 352U && info.height == 288U &&
        info.video_codec == ZZ9K_VIDEO_CODEC_MPEG1);
  CHECK(zz9k_mediaout_mpeg_info(mpeg2, sizeof(mpeg2), &info));
  CHECK(info.width == 720U && info.height == 576U &&
        info.video_codec == ZZ9K_VIDEO_CODEC_MPEG2);
  /* found after leading data */
  memset(shifted, 0x55, sizeof(shifted));
  memcpy(shifted + 9, mpeg1, sizeof(mpeg1));
  CHECK(zz9k_mediaout_mpeg_info(shifted, sizeof(shifted), &info));
  CHECK(info.width == 352U);
  CHECK(!zz9k_mediaout_mpeg_info(ps, sizeof(ps), &info));
  CHECK(!zz9k_mediaout_mpeg_info(mpeg1, 8U, &info));
  CHECK(!zz9k_mediaout_mpeg_info(0, 0U, &info));
  CHECK(zz9k_mediaout_is_program_stream(ps, sizeof(ps)));
  CHECK(!zz9k_mediaout_is_program_stream(mpeg1, sizeof(mpeg1)));
  CHECK(!zz9k_mediaout_is_program_stream(ps, 3U));
}

static void test_checksum(void)
{
  /* FNV-1a reference values */
  CHECK(zz9k_mediaout_checksum(ZZ9K_MEDIAOUT_CHECKSUM_SEED, 0, 0U) ==
        0x811c9dc5UL);
  CHECK(zz9k_mediaout_checksum(ZZ9K_MEDIAOUT_CHECKSUM_SEED,
                               (const uint8_t *)"a", 1U) == 0xe40c292cUL);
  CHECK(zz9k_mediaout_checksum(ZZ9K_MEDIAOUT_CHECKSUM_SEED,
                               (const uint8_t *)"foobar", 6U) ==
        0xbf9cf968UL);
  /* blocks chain exactly like one call */
  CHECK(zz9k_mediaout_checksum(
            zz9k_mediaout_checksum(ZZ9K_MEDIAOUT_CHECKSUM_SEED,
                                   (const uint8_t *)"foo", 3U),
            (const uint8_t *)"bar", 3U) == 0xbf9cf968UL);
}

static void test_plan(void)
{
  ZZ9KMediaOutOptions o;
  ZZ9KMediaOutPlan plan;

  CHECK(parse(&o, "-f", "planar", "-d", "8", "a.mpg", 0));
  CHECK(zz9k_mediaout_plan(&o, 320U, 240U, &plan) == ZZ9K_STATUS_OK);
  CHECK(plan.width == 320U && plan.height == 240U && plan.pitch == 40U &&
        plan.depth == 8U && plan.picture_bytes == 76800U &&
        plan.slots == 2U && plan.slot_stride == 76800U &&
        plan.buffer_bytes == 153600U);
  CHECK(ZZ9K_MEDIA_OUTPUT_DEPTH(plan.flags) == 8U);
  /* a width that is no multiple of 16 pads the pitch to an even byte count */
  CHECK(zz9k_mediaout_plan(&o, 100U, 50U, &plan) == ZZ9K_STATUS_OK);
  CHECK(plan.pitch == 14U && plan.picture_bytes == 14U * 50U * 8U);
  CHECK(parse(&o, "-f", "rgb565", "-s", "160x100", "-1", "a.mpg", 0));
  CHECK(zz9k_mediaout_plan(&o, 320U, 240U, &plan) == ZZ9K_STATUS_OK);
  CHECK(plan.width == 160U && plan.height == 100U && plan.pitch == 320U &&
        plan.depth == 0U && plan.slots == 1U && plan.slot_stride == 0U &&
        plan.buffer_bytes == 32000U);
  CHECK(parse(&o, "-f", "index8", "-d", "6", "-H", "a.mpg", 0));
  CHECK(zz9k_mediaout_plan(&o, 320U, 240U, &plan) == ZZ9K_STATUS_OK);
  CHECK(plan.pitch == 320U && plan.depth == 6U &&
        (plan.flags & ZZ9K_MEDIA_OUTPUT_PALETTE_HOST) != 0U);
  /* the card's own check decides: nothing past 1920x1088 */
  CHECK(parse(&o, "a.mpg", 0, 0, 0, 0, 0));
  CHECK(zz9k_mediaout_plan(&o, 2000U, 100U, &plan) ==
        ZZ9K_STATUS_BAD_REQUEST);
  CHECK(parse(&o, "-o", "a.mpg", 0, 0, 0, 0));
  CHECK(zz9k_mediaout_plan(&o, 320U, 240U, &plan) == ZZ9K_STATUS_OK);
}

static void test_gray_palette(void)
{
  uint8_t bytes[1024];

  zz9k_mediaout_gray_palette(2U, bytes);
  CHECK(zz9k_mediaout_palette_word(bytes, 0) == 0x000000UL);
  CHECK(zz9k_mediaout_palette_word(bytes, 1) == 0x555555UL);
  CHECK(zz9k_mediaout_palette_word(bytes, 2) == 0xaaaaaaUL);
  CHECK(zz9k_mediaout_palette_word(bytes, 3) == 0xffffffUL);
  CHECK(zz9k_mediaout_palette_word(bytes, 4) == 0UL);
  CHECK(bytes[0] == 0 && bytes[4] == 0 && bytes[5] == 0x55);
  zz9k_mediaout_gray_palette(8U, bytes);
  CHECK(zz9k_mediaout_palette_word(bytes, 255) == 0xffffffUL);
  CHECK(zz9k_mediaout_palette_word(bytes, 128) == 0x808080UL);
}

static void test_row_to_rgb(void)
{
  ZZ9KMediaOutPlan plan;
  uint32_t out[16];
  uint32_t palette[256];
  uint8_t slot[512];
  uint32_t p;

  memset(&plan, 0, sizeof(plan));
  memset(slot, 0, sizeof(slot));

  /* RGB565: white, black, pure red, pure green */
  plan.format = ZZ9K_SURFACE_FORMAT_RGB565;
  plan.width = 4U;
  plan.height = 2U;
  plan.pitch = 10U;
  slot[10] = 0xff; slot[11] = 0xff;
  slot[12] = 0x00; slot[13] = 0x00;
  slot[14] = 0xf8; slot[15] = 0x00;
  slot[16] = 0x07; slot[17] = 0xe0;
  zz9k_mediaout_row_to_rgb(&plan, slot, 1U, 0, out);
  CHECK(out[0] == 0xffffffUL && out[1] == 0x000000UL &&
        out[2] == 0xff0000UL && out[3] == 0x00ff00UL);
  /* RGB555 */
  plan.format = ZZ9K_SURFACE_FORMAT_RGB555;
  slot[10] = 0x7f; slot[11] = 0xff;
  slot[14] = 0x7c; slot[15] = 0x00;
  slot[16] = 0x03; slot[17] = 0xe0;
  zz9k_mediaout_row_to_rgb(&plan, slot, 1U, 0, out);
  CHECK(out[0] == 0xffffffUL && out[2] == 0xff0000UL && out[3] == 0x00ff00UL);

  /* INDEX8 through a palette */
  for (p = 0U; p < 256U; p++) {
    palette[p] = p * 0x010101UL;
  }
  plan.format = ZZ9K_SURFACE_FORMAT_INDEX8;
  plan.pitch = 6U;
  slot[0] = 1; slot[1] = 200; slot[2] = 255; slot[3] = 0;
  zz9k_mediaout_row_to_rgb(&plan, slot, 0U, palette, out);
  CHECK(out[0] == 0x010101UL && out[1] == 0xc8c8c8UL &&
        out[2] == 0xffffffUL && out[3] == 0UL);

  /* PLANAR, 3 bits, 9 pixels wide (two bytes a row), both layouts: pixel x
   * of the row gets the index (x % 8) */
  plan.format = ZZ9K_SURFACE_FORMAT_PLANAR;
  plan.width = 9U;
  plan.height = 2U;
  plan.depth = 3U;
  plan.pitch = 2U;
  for (p = 0U; p < 16U; p++) {
    palette[p] = 0x100000UL + p;
  }
  {
    uint32_t layout;

    for (layout = 0U; layout < 2U; layout++) {
      uint32_t plane, x, y;

      plan.flags = 3U | (layout != 0U ? ZZ9K_MEDIA_OUTPUT_PLANAR_INTERLEAVED : 0U);
      memset(slot, 0, sizeof(slot));
      for (y = 0U; y < plan.height; y++) {
        for (plane = 0U; plane < plan.depth; plane++) {
          uint8_t *row = slot + zz9k_media_output_plane_row_offset(
              plan.flags, plan.height, plan.pitch, plane, y);

          for (x = 0U; x < plan.width; x++) {
            uint32_t index = (x + y) % 8U;

            if (((index >> plane) & 1U) != 0U) {
              row[x >> 3] |= (uint8_t)(0x80U >> (x & 7U));
            }
          }
        }
      }
      for (y = 0U; y < plan.height; y++) {
        zz9k_mediaout_row_to_rgb(&plan, slot, y, palette, out);
        for (x = 0U; x < plan.width; x++) {
          CHECK(out[x] == 0x100000UL + (x + y) % 8U);
        }
      }
    }
  }
}

static void test_checksum_picture_and_rate(void)
{
  ZZ9KMediaOutOptions o;
  ZZ9KMediaOutPlan plan;
  uint8_t a[4096];
  uint8_t b[4096];
  uint32_t x;
  uint32_t y;
  uint32_t p;

  /* 100x50 planar: 13 active bytes a plane row in a 14-byte pitch */
  CHECK(parse(&o, "-f", "planar", "-d", "3", "-s", "100x50", "a.mpg", 0));
  CHECK(zz9k_mediaout_plan(&o, 320U, 240U, &plan) == ZZ9K_STATUS_OK);
  CHECK(plan.pitch == 14U && plan.picture_bytes == 14U * 50U * 3U);
  memset(a, 0xa5, sizeof(a));
  memset(b, 0x00, sizeof(b));
  for (p = 0U; p < 3U; p++) {
    for (y = 0U; y < 50U; y++) {
      for (x = 0U; x < 13U; x++) {
        uint32_t off = zz9k_media_output_plane_row_offset(
            plan.flags, plan.height, plan.pitch, p, y) + x;

        a[off] = b[off] = (uint8_t)(x * 7U + y * 3U + p);
      }
    }
  }
  /* identical pictures with different padding hash alike... */
  CHECK(zz9k_mediaout_checksum_picture(&plan, a, ZZ9K_MEDIAOUT_CHECKSUM_SEED) ==
        zz9k_mediaout_checksum_picture(&plan, b, ZZ9K_MEDIAOUT_CHECKSUM_SEED));
  /* ...while hashing every byte of the picture would tell them apart */
  CHECK(zz9k_mediaout_checksum(ZZ9K_MEDIAOUT_CHECKSUM_SEED, a,
                               plan.picture_bytes) !=
        zz9k_mediaout_checksum(ZZ9K_MEDIAOUT_CHECKSUM_SEED, b,
                               plan.picture_bytes));
  /* a changed pixel does change it */
  b[5] ^= 1U;
  CHECK(zz9k_mediaout_checksum_picture(&plan, a, ZZ9K_MEDIAOUT_CHECKSUM_SEED) !=
        zz9k_mediaout_checksum_picture(&plan, b, ZZ9K_MEDIAOUT_CHECKSUM_SEED));
  /* INDEX8 5x2: 5 active bytes in a 6-byte pitch */
  CHECK(parse(&o, "-f", "index8", "-s", "5x2", "a.mpg", 0));
  CHECK(zz9k_mediaout_plan(&o, 320U, 240U, &plan) == ZZ9K_STATUS_OK);
  CHECK(plan.pitch == 6U);
  memset(a, 0x11, sizeof(a));
  memset(b, 0x11, sizeof(b));
  a[5] = 0xee; /* padding of row 0 */
  CHECK(zz9k_mediaout_checksum_picture(&plan, a, 1U) ==
        zz9k_mediaout_checksum_picture(&plan, b, 1U));
  a[4] = 0xee; /* an active byte */
  CHECK(zz9k_mediaout_checksum_picture(&plan, a, 1U) !=
        zz9k_mediaout_checksum_picture(&plan, b, 1U));

  /* 50 pictures in 2 s is 25.00 a second; the 32-bit product of the old
   * print (50 * 100000000) is above 2^32 */
  CHECK(zz9k_mediaout_rate_x100(50U, 2000000U) == 2500ULL);
  CHECK(zz9k_mediaout_rate_x100(100000U, 4000000U) == 2500000ULL);
  CHECK(zz9k_mediaout_rate_x100(5000U, 1000000U) == 500000ULL);
  CHECK(zz9k_mediaout_rate_x100(1U, 0U) == 0ULL);

  CHECK(zz9k_mediaout_smaller_chunk(16384U) == 8192U);
  CHECK(zz9k_mediaout_smaller_chunk(4096U) == 2048U);
  CHECK(zz9k_mediaout_smaller_chunk(2048U) == 0U);
}

int main(void)
{
  test_checksum_picture_and_rate();
  test_parse();
  test_mpeg_info();
  test_checksum();
  test_plan();
  test_gray_palette();
  test_row_to_rgb();
  if (failures != 0) {
    printf("mediaout_logic_test: %d failure(s) of %d checks\n", failures,
           checks);
    return 1;
  }
  printf("mediaout_logic_test: %d checks passed\n", checks);
  return 0;
}
