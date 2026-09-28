/*
 * DataTypes object creation probe for ZZ9000 picture datatype debugging.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <clib/alib_protos.h>
#include <dos/dos.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <intuition/classusr.h>
#include <intuition/gadgetclass.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/datatypes.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <string.h>

struct Library *DataTypesBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

#define ZZ9K_DTPROBE_DRAW_MAX_WIDTH 320UL
#define ZZ9K_DTPROBE_DRAW_MAX_HEIGHT 240UL

static void zz9k_dtprobe_usage(void)
{
  printf("usage: zz9k-dtprobe [--client] file.jpg|file.png\n");
  printf("       zz9k-dtprobe --read-pixels file.jpg|file.png\n");
  printf("       zz9k-dtprobe --screen-remap --layout file.jpg|file.png\n");
  printf("       zz9k-dtprobe --draw-window file.jpg|file.png\n");
  printf("       --client also queries bitmap/mask attrs used by clients\n");
  printf("       --read-pixels reads row checksums through PDTM_READPIXELARRAY\n");
  printf("       --screen-remap passes PDTA_Screen/PDTA_Remap at OM_NEW\n");
  printf("       --layout runs DTM_PROCLAYOUT and reprints attrs\n");
  printf("       --color-tables prints palette/remap table attrs\n");
  printf("       --draw-window refreshes an added DT object and hashes screen pixels\n");
}

static const char *zz9k_dtprobe_string(STRPTR value)
{
  return value ? (const char *)value : "(null)";
}

static void zz9k_dtprobe_copy_string(char *dst, size_t capacity,
                                     STRPTR value)
{
  const char *src;

  if (!dst || capacity == 0) {
    return;
  }
  src = zz9k_dtprobe_string(value);
  strncpy(dst, src, capacity - 1U);
  dst[capacity - 1U] = '\0';
}

static void zz9k_dtprobe_print_id(const char *label, ULONG value)
{
  printf("zz9k-dtprobe: %s %c%c%c%c\n",
         label,
         (int)((value >> 24) & 0xffU),
         (int)((value >> 16) & 0xffU),
         (int)((value >> 8) & 0xffU),
         (int)(value & 0xffU));
}

static int zz9k_dtprobe_parse_args(int argc, char **argv,
                                   const char **path,
                                   int *client_mode,
                                   int *read_pixels,
                                   int *screen_remap,
                                   int *layout_mode,
                                   int *color_tables,
                                   int *draw_window)
{
  int i;

  *path = 0;
  *client_mode = 0;
  *read_pixels = 0;
  *screen_remap = 0;
  *layout_mode = 0;
  *color_tables = 0;
  *draw_window = 0;

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--client") == 0) {
      *client_mode = 1;
    } else if (strcmp(argv[i], "--read-pixels") == 0) {
      *read_pixels = 1;
    } else if (strcmp(argv[i], "--screen-remap") == 0) {
      *screen_remap = 1;
    } else if (strcmp(argv[i], "--layout") == 0) {
      *layout_mode = 1;
    } else if (strcmp(argv[i], "--color-tables") == 0) {
      *color_tables = 1;
    } else if (strcmp(argv[i], "--draw-window") == 0) {
      *draw_window = 1;
    } else if (strcmp(argv[i], "--help") == 0 ||
               strcmp(argv[i], "-h") == 0) {
      return 0;
    } else if (!*path) {
      *path = argv[i];
    } else {
      return 0;
    }
  }

  return *path != 0;
}

static const char *zz9k_dtprobe_method_name(ULONG method)
{
  switch (method) {
    case PDTM_WRITEPIXELARRAY:
      return "PDTM_WRITEPIXELARRAY";
    case PDTM_READPIXELARRAY:
      return "PDTM_READPIXELARRAY";
    case PDTM_OBTAINPIXELARRAY:
      return "PDTM_OBTAINPIXELARRAY";
    case PDTM_SCALE:
      return "PDTM_SCALE";
    default:
      return "";
  }
}

static void zz9k_dtprobe_print_ulong_attr(Object *object,
                                          const char *name,
                                          ULONG tag)
{
  ULONG value;
  ULONG got;

  value = 0;
  got = GetDTAttrs(object, tag, (ULONG)&value, TAG_END);
  printf("zz9k-dtprobe: attr %s got=%lu value=0x%08lx (%lu)\n",
         name,
         (unsigned long)got,
         (unsigned long)value,
         (unsigned long)value);
}

static void zz9k_dtprobe_print_ptr_attr(Object *object,
                                        const char *name,
                                        ULONG tag)
{
  ULONG value;
  ULONG got;

  value = 0;
  got = GetDTAttrs(object, tag, (ULONG)&value, TAG_END);
  printf("zz9k-dtprobe: attr %s got=%lu ptr=0x%08lx\n",
         name, (unsigned long)got, (unsigned long)value);
}

static void zz9k_dtprobe_print_string_attr(Object *object,
                                           const char *name,
                                           ULONG tag)
{
  STRPTR value;
  ULONG got;

  value = 0;
  got = GetDTAttrs(object, tag, (ULONG)&value, TAG_END);
  printf("zz9k-dtprobe: attr %s got=%lu string='%s'\n",
         name,
         (unsigned long)got,
         zz9k_dtprobe_string(value));
}

static void zz9k_dtprobe_print_bitmap_header(Object *object)
{
  struct BitMapHeader *header;
  ULONG got;

  header = 0;
  got = GetDTAttrs(
      object,
      PDTA_BitMapHeader, (ULONG)&header,
      TAG_END);
  if (!header) {
    printf("zz9k-dtprobe: attr PDTA_BitMapHeader got=%lu ptr=0x00000000\n",
           (unsigned long)got);
    return;
  }

  printf("zz9k-dtprobe: attr PDTA_BitMapHeader got=%lu ptr=0x%08lx\n",
         (unsigned long)got, (unsigned long)(ULONG)header);
  printf("zz9k-dtprobe: bmhd width=%u height=%u depth=%u masking=%u "
         "transparent=%u compression=%u page=%u x %u\n",
         (unsigned int)header->bmh_Width,
         (unsigned int)header->bmh_Height,
         (unsigned int)header->bmh_Depth,
         (unsigned int)header->bmh_Masking,
         (unsigned int)header->bmh_Transparent,
         (unsigned int)header->bmh_Compression,
         (unsigned int)header->bmh_PageWidth,
         (unsigned int)header->bmh_PageHeight);
}

static void zz9k_dtprobe_print_methods(Object *object)
{
  ULONG *methods;
  ULONG got;
  unsigned int i;

  methods = 0;
  got = GetDTAttrs(
      object,
      DTA_Methods, (ULONG)&methods,
      TAG_END);
  if (!methods) {
    printf("zz9k-dtprobe: attr DTA_Methods got=%lu ptr=0x00000000\n",
           (unsigned long)got);
    return;
  }

  printf("zz9k-dtprobe: attr DTA_Methods got=%lu ptr=0x%08lx\n",
         (unsigned long)got, (unsigned long)(ULONG)methods);
  for (i = 0; i < 64U; ++i) {
    ULONG method;
    const char *name;

    method = methods[i];
    if (method == ~0UL) {
      printf("zz9k-dtprobe: method[%u] terminator\n", i);
      return;
    }

    name = zz9k_dtprobe_method_name(method);
    if (name[0] != '\0') {
      printf("zz9k-dtprobe: method[%u] 0x%08lx %s\n",
             i, (unsigned long)method, name);
    } else {
      printf("zz9k-dtprobe: method[%u] 0x%08lx\n",
             i, (unsigned long)method);
    }
  }

  printf("zz9k-dtprobe: method list truncated after 64 entries\n");
}

static void zz9k_dtprobe_print_picture_attrs(Object *object)
{
  zz9k_dtprobe_print_string_attr(object, "DTA_ObjName", DTA_ObjName);
  zz9k_dtprobe_print_ulong_attr(object, "DTA_NominalHoriz",
                                DTA_NominalHoriz);
  zz9k_dtprobe_print_ulong_attr(object, "DTA_NominalVert",
                                DTA_NominalVert);
  zz9k_dtprobe_print_ulong_attr(object, "DTA_TotalHoriz", DTA_TotalHoriz);
  zz9k_dtprobe_print_ulong_attr(object, "DTA_TotalVert", DTA_TotalVert);
  zz9k_dtprobe_print_bitmap_header(object);
  zz9k_dtprobe_print_ulong_attr(object, "PDTA_ModeID", PDTA_ModeID);
  zz9k_dtprobe_print_ulong_attr(object, "PDTA_SourceMode", PDTA_SourceMode);
  zz9k_dtprobe_print_ulong_attr(object, "PDTA_DestMode", PDTA_DestMode);
  zz9k_dtprobe_print_ulong_attr(object, "PDTA_NumColors", PDTA_NumColors);
  zz9k_dtprobe_print_ulong_attr(object, "PDTA_AlphaChannel",
                                PDTA_AlphaChannel);
  zz9k_dtprobe_print_methods(object);
}

static void zz9k_dtprobe_print_client_attrs(Object *object)
{
  printf("zz9k-dtprobe: client attr queries follow\n");
  zz9k_dtprobe_print_ptr_attr(object, "PDTA_BitMap", PDTA_BitMap);
  zz9k_dtprobe_print_ptr_attr(object, "PDTA_ClassBitMap",
                              PDTA_ClassBitMap);
  zz9k_dtprobe_print_ptr_attr(object, "PDTA_DestBitMap",
                              PDTA_DestBitMap);
  zz9k_dtprobe_print_ptr_attr(object, "PDTA_MaskPlane", PDTA_MaskPlane);
}

static ULONG zz9k_dtprobe_min_ulong(ULONG a, ULONG b)
{
  return a < b ? a : b;
}

static void zz9k_dtprobe_print_color_attrs(Object *object)
{
  struct ColorRegister *color_registers;
  ULONG *cregs;
  ULONG *gregs;
  UBYTE *color_table;
  UBYTE *color_table2;
  ULONG num_colors;
  ULONG num_alloc;
  ULONG got;
  ULONG limit;
  ULONG i;

  color_registers = 0;
  cregs = 0;
  gregs = 0;
  color_table = 0;
  color_table2 = 0;
  num_colors = 0;
  num_alloc = 0;
  got = GetDTAttrs(
      object,
      PDTA_NumColors, (ULONG)&num_colors,
      PDTA_NumAlloc, (ULONG)&num_alloc,
      PDTA_ColorRegisters, (ULONG)&color_registers,
      PDTA_CRegs, (ULONG)&cregs,
      PDTA_GRegs, (ULONG)&gregs,
      PDTA_ColorTable, (ULONG)&color_table,
      PDTA_ColorTable2, (ULONG)&color_table2,
      TAG_END);
  printf("zz9k-dtprobe: color attrs got=%lu numcolors=%lu "
         "numalloc=%lu ColorRegisters=0x%08lx CRegs=0x%08lx "
         "GRegs=0x%08lx ColorTable=0x%08lx ColorTable2=0x%08lx\n",
         (unsigned long)got,
         (unsigned long)num_colors,
         (unsigned long)num_alloc,
         (unsigned long)(ULONG)color_registers,
         (unsigned long)(ULONG)cregs,
         (unsigned long)(ULONG)gregs,
         (unsigned long)(ULONG)color_table,
         (unsigned long)(ULONG)color_table2);

  limit = num_colors ? zz9k_dtprobe_min_ulong(num_colors, 8UL) : 8UL;
  if (color_registers) {
    for (i = 0; i < limit; ++i) {
      printf("zz9k-dtprobe: color register[%lu] %u %u %u\n",
             (unsigned long)i,
             (unsigned int)color_registers[i].red,
             (unsigned int)color_registers[i].green,
             (unsigned int)color_registers[i].blue);
    }
  }
  if (cregs) {
    for (i = 0; i < limit; ++i) {
      printf("zz9k-dtprobe: creg[%lu] 0x%08lx 0x%08lx 0x%08lx\n",
             (unsigned long)i,
             (unsigned long)cregs[(i * 3UL) + 0UL],
             (unsigned long)cregs[(i * 3UL) + 1UL],
             (unsigned long)cregs[(i * 3UL) + 2UL]);
    }
  }
  if (gregs) {
    for (i = 0; i < limit; ++i) {
      printf("zz9k-dtprobe: greg[%lu] 0x%08lx 0x%08lx 0x%08lx\n",
             (unsigned long)i,
             (unsigned long)gregs[(i * 3UL) + 0UL],
             (unsigned long)gregs[(i * 3UL) + 1UL],
             (unsigned long)gregs[(i * 3UL) + 2UL]);
    }
  }
  if (color_table) {
    printf("zz9k-dtprobe: color table first=");
    for (i = 0; i < limit; ++i) {
      printf("%02lx", (unsigned long)color_table[i]);
    }
    printf("\n");
  }
  if (color_table2) {
    printf("zz9k-dtprobe: color table2 first=");
    for (i = 0; i < limit; ++i) {
      printf("%02lx", (unsigned long)color_table2[i]);
    }
    printf("\n");
  }
}

static ULONG zz9k_dtprobe_run_layout(Object *object)
{
  struct gpLayout layout;
  ULONG result;

  memset(&layout, 0, sizeof(layout));
  layout.MethodID = DTM_PROCLAYOUT;
  layout.gpl_GInfo = 0;
  SetIoErr(0);
  result = DoMethodA(object, (Msg)&layout);
  printf("zz9k-dtprobe: DTM_PROCLAYOUT result=%lu IoErr=%ld\n",
         (unsigned long)result, IoErr());
  return result;
}

static const char *zz9k_dtprobe_pixel_format_name(ULONG format)
{
  switch (format) {
    case PBPAFMT_RGB:
      return "PBPAFMT_RGB";
    case PBPAFMT_RGBA:
      return "PBPAFMT_RGBA";
    case PBPAFMT_ARGB:
      return "PBPAFMT_ARGB";
    case PBPAFMT_LUT8:
      return "PBPAFMT_LUT8";
    case PBPAFMT_GREY8:
      return "PBPAFMT_GREY8";
    default:
      return "unknown";
  }
}

static ULONG zz9k_dtprobe_pixel_format_bpp(ULONG format)
{
  switch (format) {
    case PBPAFMT_RGB:
      return 3UL;
    case PBPAFMT_RGBA:
    case PBPAFMT_ARGB:
      return 4UL;
    case PBPAFMT_LUT8:
    case PBPAFMT_GREY8:
      return 1UL;
    default:
      return 0UL;
  }
}

static ULONG zz9k_dtprobe_fnv1a_update(ULONG hash,
                                       const UBYTE *data,
                                       ULONG length)
{
  ULONG i;

  for (i = 0; i < length; ++i) {
    hash ^= (ULONG)data[i];
    hash *= 16777619UL;
  }
  return hash;
}

static ULONG zz9k_dtprobe_fnv1a_update_ulong(ULONG hash, ULONG value)
{
  UBYTE bytes[4];

  bytes[0] = (UBYTE)((value >> 24) & 0xffU);
  bytes[1] = (UBYTE)((value >> 16) & 0xffU);
  bytes[2] = (UBYTE)((value >> 8) & 0xffU);
  bytes[3] = (UBYTE)(value & 0xffU);
  return zz9k_dtprobe_fnv1a_update(hash, bytes, 4UL);
}

static void zz9k_dtprobe_print_hex_sample(const UBYTE *row,
                                          ULONG row_bytes)
{
  ULONG count;
  ULONG i;

  count = row_bytes < 16UL ? row_bytes : 16UL;
  printf(" sample=");
  for (i = 0; i < count; ++i) {
    printf("%02lx", (unsigned long)row[i]);
  }
  if (row_bytes > count) {
    printf("...");
  }
}

static void zz9k_dtprobe_print_pixel_format_read(Object *object,
                                                 ULONG format,
                                                 ULONG width,
                                                 ULONG height)
{
  struct pdtBlitPixelArray pixels;
  UBYTE *row;
  ULONG bpp;
  ULONG row_bytes;
  ULONG y;
  ULONG ok_rows;
  ULONG failed_at;
  ULONG hash;
  ULONG alpha_min;
  ULONG alpha_max;
  ULONG alpha_nonopaque;
  int has_alpha;
  int sample_ready;

  bpp = zz9k_dtprobe_pixel_format_bpp(format);
  if (!object || width == 0UL || height == 0UL || bpp == 0UL ||
      width > (0xffffffffUL / bpp)) {
    printf("zz9k-dtprobe: pixel %s invalid request width=%lu height=%lu\n",
           zz9k_dtprobe_pixel_format_name(format),
           (unsigned long)width,
           (unsigned long)height);
    return;
  }

  row_bytes = width * bpp;
  row = (UBYTE *)AllocMem(row_bytes, MEMF_PUBLIC);
  if (!row) {
    printf("zz9k-dtprobe: pixel %s row alloc failed bytes=%lu\n",
           zz9k_dtprobe_pixel_format_name(format),
           (unsigned long)row_bytes);
    return;
  }

  ok_rows = 0UL;
  failed_at = height;
  hash = 2166136261UL;
  alpha_min = 255UL;
  alpha_max = 0UL;
  alpha_nonopaque = 0UL;
  has_alpha = format == PBPAFMT_RGBA || format == PBPAFMT_ARGB;
  sample_ready = 0;

  for (y = 0UL; y < height; ++y) {
    ULONG result;

    memset(row, 0xa5, row_bytes);
    memset(&pixels, 0, sizeof(pixels));
    pixels.MethodID = PDTM_READPIXELARRAY;
    pixels.pbpa_PixelData = row;
    pixels.pbpa_PixelFormat = format;
    pixels.pbpa_PixelArrayMod = row_bytes;
    pixels.pbpa_Left = 0;
    pixels.pbpa_Top = y;
    pixels.pbpa_Width = width;
    pixels.pbpa_Height = 1;
    result = DoMethodA(object, (Msg)&pixels);
    if (result == 0UL) {
      failed_at = y;
      break;
    }

    if (!sample_ready) {
      sample_ready = 1;
    }
    hash = zz9k_dtprobe_fnv1a_update(hash, row, row_bytes);
    if (has_alpha) {
      ULONG x;
      for (x = 0UL; x < width; ++x) {
        ULONG alpha;

        alpha = format == PBPAFMT_RGBA ?
            (ULONG)row[(x * bpp) + 3UL] :
            (ULONG)row[x * bpp];
        if (alpha < alpha_min) {
          alpha_min = alpha;
        }
        if (alpha > alpha_max) {
          alpha_max = alpha;
        }
        if (alpha != 255UL) {
          alpha_nonopaque++;
        }
      }
    }
    ok_rows++;
  }

  printf("zz9k-dtprobe: pixel %s rows=%lu/%lu rowbytes=%lu "
         "hash=0x%08lx",
         zz9k_dtprobe_pixel_format_name(format),
         (unsigned long)ok_rows,
         (unsigned long)height,
         (unsigned long)row_bytes,
         (unsigned long)hash);
  if (failed_at != height) {
    printf(" failed_at=%lu", (unsigned long)failed_at);
  }
  if (has_alpha) {
    printf(" alpha_min=%lu alpha_max=%lu alpha_nonopaque=%lu",
           (unsigned long)alpha_min,
           (unsigned long)alpha_max,
           (unsigned long)alpha_nonopaque);
  }
  if (sample_ready) {
    zz9k_dtprobe_print_hex_sample(row, row_bytes);
  }
  printf("\n");

  FreeMem(row, row_bytes);
}

static ULONG zz9k_dtprobe_hash_rastport(struct RastPort *rast_port,
                                        ULONG width,
                                        ULONG height)
{
  ULONG hash;
  ULONG x;
  ULONG y;

  hash = 2166136261UL;
  for (y = 0UL; y < height; ++y) {
    for (x = 0UL; x < width; ++x) {
      hash = zz9k_dtprobe_fnv1a_update_ulong(
          hash, ReadPixel(rast_port, (LONG)x, (LONG)y));
    }
  }
  return hash;
}

static void zz9k_dtprobe_print_rastport_samples(struct RastPort *rast_port,
                                                ULONG width,
                                                ULONG height)
{
  ULONG x0;
  ULONG y0;
  ULONG x1;
  ULONG y1;
  ULONG x2;
  ULONG y2;

  x0 = 0UL;
  y0 = 0UL;
  x1 = width > 0UL ? width - 1UL : 0UL;
  y1 = height > 0UL ? height - 1UL : 0UL;
  x2 = width / 2UL;
  y2 = height / 2UL;

  printf(" samples[%lu,%lu]=0x%08lx",
         (unsigned long)x0,
         (unsigned long)y0,
         (unsigned long)ReadPixel(rast_port, (LONG)x0, (LONG)y0));
  printf(" [%lu,%lu]=0x%08lx",
         (unsigned long)x1,
         (unsigned long)y0,
         (unsigned long)ReadPixel(rast_port, (LONG)x1, (LONG)y0));
  printf(" [%lu,%lu]=0x%08lx",
         (unsigned long)x0,
         (unsigned long)y1,
         (unsigned long)ReadPixel(rast_port, (LONG)x0, (LONG)y1));
  printf(" [%lu,%lu]=0x%08lx",
         (unsigned long)x1,
         (unsigned long)y1,
         (unsigned long)ReadPixel(rast_port, (LONG)x1, (LONG)y1));
  printf(" [%lu,%lu]=0x%08lx",
         (unsigned long)x2,
         (unsigned long)y2,
         (unsigned long)ReadPixel(rast_port, (LONG)x2, (LONG)y2));
}

static int zz9k_dtprobe_attach_draw_object(Object *object,
                                           struct Window *window,
                                           ULONG width,
                                           ULONG height)
{
  ULONG set_result;
  LONG add_result;

  SetIoErr(0);
  set_result = SetDTAttrs(
      object, window, 0,
      GA_Left, 0,
      GA_Top, 0,
      GA_Width, width,
      GA_Height, height,
      DTA_TopHoriz, 0,
      DTA_TopVert, 0,
      DTA_VisibleHoriz, width,
      DTA_VisibleVert, height,
      TAG_END);
  printf("zz9k-dtprobe: draw attach SetDTAttrs result=%lu IoErr=%ld\n",
         (unsigned long)set_result, IoErr());

  SetIoErr(0);
  add_result = AddDTObject(window, 0, object, -1L);
  printf("zz9k-dtprobe: draw attach AddDTObject result=%ld IoErr=%ld\n",
         add_result, IoErr());
  return add_result >= 0L;
}

static void zz9k_dtprobe_refresh_once(Object *object,
                                      struct Window *window,
                                      ULONG width,
                                      ULONG height,
                                      ULONG background_pen)
{
  ULONG before_hash;
  ULONG after_hash;

  SetRast(window->RPort, background_pen);
  before_hash = zz9k_dtprobe_hash_rastport(window->RPort, width, height);

  SetIoErr(0);
  RefreshDTObjectA(object, window, 0, 0);
  after_hash = zz9k_dtprobe_hash_rastport(window->RPort, width, height);

  printf("zz9k-dtprobe: refresh bg_pen=%lu IoErr=%ld",
         (unsigned long)background_pen,
         IoErr());
  printf(" before_hash=0x%08lx after_hash=0x%08lx",
         (unsigned long)before_hash,
         (unsigned long)after_hash);
  zz9k_dtprobe_print_rastport_samples(window->RPort, width, height);
  printf("\n");
}

static void zz9k_dtprobe_print_draw_window(Object *object,
                                           struct Screen *screen)
{
  static const ULONG background_pens[] = { 0UL, 1UL, 2UL };
  struct Window *window;
  ULONG width;
  ULONG height;
  ULONG draw_width;
  ULONG draw_height;
  ULONG i;
  LONG left;
  LONG top;

  if (!object || !screen || !GfxBase) {
    printf("zz9k-dtprobe: draw window unavailable\n");
    return;
  }

  width = 0UL;
  height = 0UL;
  (void)GetDTAttrs(
      object,
      DTA_TotalHoriz, (ULONG)&width,
      DTA_TotalVert, (ULONG)&height,
      TAG_END);
  if (width == 0UL || height == 0UL) {
    (void)GetDTAttrs(
        object,
        DTA_NominalHoriz, (ULONG)&width,
        DTA_NominalVert, (ULONG)&height,
        TAG_END);
  }
  if (width == 0UL || height == 0UL) {
    struct BitMapHeader *header;

    header = 0;
    (void)GetDTAttrs(
        object,
        PDTA_BitMapHeader, (ULONG)&header,
        TAG_END);
    if (header) {
      width = (ULONG)header->bmh_Width;
      height = (ULONG)header->bmh_Height;
    }
  }
  if (width == 0UL || height == 0UL) {
    printf("zz9k-dtprobe: draw window missing dimensions\n");
    return;
  }

  draw_width = zz9k_dtprobe_min_ulong(width, (ULONG)screen->Width);
  draw_height = zz9k_dtprobe_min_ulong(height, (ULONG)screen->Height);
  draw_width = zz9k_dtprobe_min_ulong(draw_width,
                                      ZZ9K_DTPROBE_DRAW_MAX_WIDTH);
  draw_height = zz9k_dtprobe_min_ulong(draw_height,
                                       ZZ9K_DTPROBE_DRAW_MAX_HEIGHT);
  if (draw_width == 0UL || draw_height == 0UL) {
    printf("zz9k-dtprobe: draw window invalid clipped dimensions\n");
    return;
  }

  left = 0;
  top = 0;
  if ((ULONG)screen->Width > draw_width) {
    left = (LONG)(((ULONG)screen->Width - draw_width) / 2UL);
  }
  if ((ULONG)screen->Height > draw_height) {
    top = (LONG)(((ULONG)screen->Height - draw_height) / 2UL);
  }

  window = OpenWindowTags(
      0,
      WA_PubScreen, (ULONG)screen,
      WA_Left, (ULONG)left,
      WA_Top, (ULONG)top,
      WA_Width, draw_width,
      WA_Height, draw_height,
      WA_Borderless, TRUE,
      WA_SmartRefresh, TRUE,
      WA_Activate, TRUE,
      WA_RMBTrap, TRUE,
      WA_IDCMP, IDCMP_REFRESHWINDOW,
      TAG_END);
  if (!window) {
    printf("zz9k-dtprobe: OpenWindowTags draw window failed\n");
    return;
  }

  printf("zz9k-dtprobe: draw window source=%lux%lu probe=%lux%lu screen=%ux%u\n",
         (unsigned long)width,
         (unsigned long)height,
         (unsigned long)draw_width,
         (unsigned long)draw_height,
         (unsigned int)screen->Width,
         (unsigned int)screen->Height);

  if (zz9k_dtprobe_attach_draw_object(object, window, draw_width,
                                      draw_height)) {
    for (i = 0UL; i < sizeof(background_pens) / sizeof(background_pens[0]);
         ++i) {
      zz9k_dtprobe_refresh_once(object, window, draw_width, draw_height,
                                background_pens[i]);
    }
    SetIoErr(0);
    printf("zz9k-dtprobe: draw detach RemoveDTObject result=%ld IoErr=%ld\n",
           RemoveDTObject(window, object), IoErr());
  }

  CloseWindow(window);
}

static void zz9k_dtprobe_print_pixel_reads(Object *object)
{
  static const ULONG formats[] = {
    PBPAFMT_RGB,
    PBPAFMT_RGBA,
    PBPAFMT_ARGB,
    PBPAFMT_LUT8,
    PBPAFMT_GREY8
  };
  ULONG width;
  ULONG height;
  ULONG i;

  width = 0UL;
  height = 0UL;
  (void)GetDTAttrs(
      object,
      DTA_NominalHoriz, (ULONG)&width,
      DTA_NominalVert, (ULONG)&height,
      TAG_END);

  printf("zz9k-dtprobe: pixel read queries follow width=%lu height=%lu\n",
         (unsigned long)width,
         (unsigned long)height);
  for (i = 0UL; i < (ULONG)(sizeof(formats) / sizeof(formats[0])); ++i) {
    zz9k_dtprobe_print_pixel_format_read(object, formats[i], width, height);
  }
}

static int zz9k_dtprobe_format_class_path(char *dst, size_t capacity,
                                          const char *format,
                                          const char *base_name)
{
  int needed;

  if (!base_name || base_name[0] == '\0' ||
      !dst || capacity == 0) {
    return 0;
  }
  needed = snprintf(dst, capacity, format, base_name);
  return needed > 0 && (size_t)needed < capacity;
}

static int zz9k_dtprobe_try_named_open(const char *name, ULONG version)
{
  struct Library *library;

  if (!name || name[0] == '\0') {
    return 0;
  }
  library = OpenLibrary((CONST_STRPTR)name, version);
  if (!library) {
    printf("zz9k-dtprobe: OpenLibrary('%s', %lu) failed\n",
           name, (unsigned long)version);
    return 0;
  }

  printf("zz9k-dtprobe: OpenLibrary('%s', %lu) OK version %u.%u\n",
         name,
         (unsigned long)version,
         (unsigned int)library->lib_Version,
         (unsigned int)library->lib_Revision);
  CloseLibrary(library);
  return 1;
}

static void zz9k_dtprobe_try_class_open_path(const char *format,
                                             const char *base_name,
                                             ULONG version)
{
  char path[128];

  if (!zz9k_dtprobe_format_class_path(path, sizeof(path),
                                      format, base_name)) {
    printf("zz9k-dtprobe: class path unavailable for '%s'\n",
           zz9k_dtprobe_string((STRPTR)base_name));
    return;
  }
  (void)zz9k_dtprobe_try_named_open(path, version);
}

static void zz9k_dtprobe_probe_class_dependencies(const char *base_name)
{
  printf("zz9k-dtprobe: dependency open checks follow\n");
  (void)zz9k_dtprobe_try_named_open("dos.library", 36);
  (void)zz9k_dtprobe_try_named_open("intuition.library", 39);
  (void)zz9k_dtprobe_try_named_open("datatypes.library", 39);
  (void)zz9k_dtprobe_try_named_open("picture.datatype", 39);
  (void)zz9k_dtprobe_try_named_open("datatypes/picture.datatype", 39);
  zz9k_dtprobe_try_class_open_path("datatypes/%s.datatype",
                                   base_name, 0);
  zz9k_dtprobe_try_class_open_path("SYS:Classes/DataTypes/%s.datatype",
                                   base_name, 0);
  zz9k_dtprobe_try_class_open_path("CLASSES:DataTypes/%s.datatype",
                                   base_name, 0);
}

int main(int argc, char **argv)
{
  struct DataType *data_type;
  BPTR lock;
  Object *object;
  struct Screen *screen;
  LONG ioerr;
  const char *path;
  int client_mode;
  int read_pixels;
  int screen_remap;
  int layout_mode;
  int color_tables;
  int draw_window;
  int need_screen;
  int layout_done;
  char descriptor_name[64];
  char base_name[64];

  if (!zz9k_dtprobe_parse_args(
          argc, argv, &path, &client_mode, &read_pixels,
          &screen_remap, &layout_mode, &color_tables, &draw_window)) {
    zz9k_dtprobe_usage();
    return 2;
  }

  IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
  if (!IntuitionBase) {
    printf("zz9k-dtprobe: could not open intuition.library v39\n");
    return 1;
  }

  DataTypesBase = OpenLibrary((CONST_STRPTR)"datatypes.library", 39);
  if (!DataTypesBase) {
    printf("zz9k-dtprobe: could not open datatypes.library v39\n");
    CloseLibrary((struct Library *)IntuitionBase);
    return 1;
  }
  if (draw_window) {
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    if (!GfxBase) {
      printf("zz9k-dtprobe: could not open graphics.library v39\n");
      CloseLibrary(DataTypesBase);
      CloseLibrary((struct Library *)IntuitionBase);
      return 1;
    }
  }

  descriptor_name[0] = '\0';
  base_name[0] = '\0';
  screen = 0;
  layout_done = 0;
  need_screen = screen_remap || draw_window;
  lock = Lock(path, ACCESS_READ);
  if (!lock) {
    printf("zz9k-dtprobe: Lock('%s') failed IoErr=%ld\n",
           path, IoErr());
  } else {
    SetIoErr(0);
    data_type = ObtainDataType(
        DTST_FILE, (APTR)lock,
        DTA_GroupID, GID_PICTURE,
        TAG_END);
    ioerr = IoErr();
    if (!data_type) {
      printf("zz9k-dtprobe: ObtainDataType failed for '%s' IoErr=%ld\n",
             path, ioerr);
    } else if (!data_type->dtn_Header) {
      printf("zz9k-dtprobe: ObtainDataType OK but descriptor has no header\n");
      ReleaseDataType(data_type);
    } else {
      zz9k_dtprobe_copy_string(
          descriptor_name, sizeof(descriptor_name),
          data_type->dtn_Header->dth_Name);
      zz9k_dtprobe_copy_string(
          base_name, sizeof(base_name),
          data_type->dtn_Header->dth_BaseName);
      printf("zz9k-dtprobe: ObtainDataType OK name='%s' base='%s'\n",
             descriptor_name, base_name);
      zz9k_dtprobe_print_id("group",
                            data_type->dtn_Header->dth_GroupID);
      zz9k_dtprobe_print_id("id", data_type->dtn_Header->dth_ID);
      ReleaseDataType(data_type);
    }
    UnLock(lock);
  }

  if (need_screen) {
    screen = LockPubScreen(0);
    if (!screen) {
      printf("zz9k-dtprobe: LockPubScreen(0) failed\n");
      if (GfxBase) {
        CloseLibrary((struct Library *)GfxBase);
      }
      CloseLibrary(DataTypesBase);
      CloseLibrary((struct Library *)IntuitionBase);
      return 1;
    }
    printf("zz9k-dtprobe: screen/remap using public screen 0x%08lx\n",
           (unsigned long)(ULONG)screen);
  }

  SetIoErr(0);
  if (need_screen) {
    object = NewDTObject(
        (APTR)path,
        DTA_SourceType, DTST_FILE,
        DTA_GroupID, GID_PICTURE,
        PDTA_Screen, (ULONG)screen,
        PDTA_Remap, TRUE,
        TAG_END);
  } else {
    object = NewDTObject(
        (APTR)path,
        DTA_SourceType, DTST_FILE,
        DTA_GroupID, GID_PICTURE,
        TAG_END);
  }
  ioerr = IoErr();
  if (!object) {
    printf("zz9k-dtprobe: NewDTObject failed for '%s' IoErr=%ld\n",
           path, ioerr);
    zz9k_dtprobe_probe_class_dependencies(base_name);
    if (screen) {
      UnlockPubScreen(0, screen);
    }
    if (GfxBase) {
      CloseLibrary((struct Library *)GfxBase);
    }
    CloseLibrary(DataTypesBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return 1;
  }

  printf("zz9k-dtprobe: NewDTObject OK for '%s'\n", path);
  printf("zz9k-dtprobe: phase initial\n");
  zz9k_dtprobe_print_picture_attrs(object);
  if (color_tables) {
    zz9k_dtprobe_print_color_attrs(object);
  }
  if (layout_mode) {
    (void)zz9k_dtprobe_run_layout(object);
    layout_done = 1;
    printf("zz9k-dtprobe: phase after-layout\n");
    zz9k_dtprobe_print_picture_attrs(object);
    if (color_tables) {
      zz9k_dtprobe_print_color_attrs(object);
    }
  }
  if (draw_window) {
    if (!layout_done) {
      (void)zz9k_dtprobe_run_layout(object);
      layout_done = 1;
    }
    zz9k_dtprobe_print_draw_window(object, screen);
  }
  if (read_pixels) {
    zz9k_dtprobe_print_pixel_reads(object);
  }
  if (client_mode) {
    zz9k_dtprobe_print_client_attrs(object);
  }

  DisposeDTObject(object);
  if (screen) {
    UnlockPubScreen(0, screen);
  }
  if (GfxBase) {
    CloseLibrary((struct Library *)GfxBase);
  }
  CloseLibrary(DataTypesBase);
  CloseLibrary((struct Library *)IntuitionBase);
  return 0;
}
