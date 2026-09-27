/*
 * Source guard for the ZZ9000 picture DataType class.
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

static int expect_contains(const char *source, const char *needle)
{
  if (strstr(source, needle)) {
    return 1;
  }

  printf("missing %s\n", needle);
  return 0;
}

static int expect_not_contains(const char *source, const char *needle)
{
  if (!strstr(source, needle)) {
    return 1;
  }

  printf("unexpected %s\n", needle);
  return 0;
}

static int expect_region_not_contains(const char *source,
                                      const char *begin_needle,
                                      const char *end_needle,
                                      const char *needle)
{
  const char *begin;
  const char *end;
  const char *match;

  begin = strstr(source, begin_needle);
  if (!begin) {
    printf("missing region start %s\n", begin_needle);
    return 0;
  }
  end = strstr(begin, end_needle);
  if (!end) {
    printf("missing region end %s\n", end_needle);
    return 0;
  }
  match = strstr(begin, needle);
  if (!match || match >= end) {
    return 1;
  }

  printf("unexpected %s between %s and %s\n",
         needle, begin_needle, end_needle);
  return 0;
}

int main(int argc, char **argv)
{
  char *source;
  char *helper_header;
  char *helper_source;
  int ok;

  if (argc != 4) {
    printf("usage: %s <amiga/datatypes/zz9k_picture_datatype.c> "
           "<tools/zz9k-image-window.h> <tools/zz9k-image-window.c>\n",
           argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }
  helper_header = read_file(argv[2]);
  if (!helper_header) {
    printf("failed to read %s\n", argv[2]);
    free(source);
    return 2;
  }
  helper_source = read_file(argv[3]);
  if (!helper_source) {
    printf("failed to read %s\n", argv[3]);
    free(helper_header);
    free(source);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "ZZ9K_PICTURE_DATATYPE_NAME");
  ok &= expect_contains(
      source, "Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio");
  ok &= expect_contains(source, "\"zz9k-picture.datatype\"");
  ok &= expect_contains(source, "ZZ9K_PICTURE_DATATYPE_VERSION 42");
  ok &= expect_contains(source, "ZZ9K_PICTURE_DATATYPE_REVISION 151");
  ok &= expect_contains(source, "$VER: zz9k-picture.datatype 42.151");
  ok &= expect_contains(source, "ZZ9K_PICTURE_BUILD_MARKER");
  ok &= expect_contains(
      source,
      "\"metadata: build 2026-09-27 png-streaming-tiles-v151\"");
  ok &= expect_contains(source, "ZZ9K_PICTURE_PNG_TRANSPARENT_NONE 0x0100U");
  ok &= expect_contains(source, "ZZ9K_PICTURE_LUT8_MAP_SLOTS 512U");
  ok &= expect_contains(source, "ZZ9KPicturePngPalette png_palette;");
  ok &= expect_contains(source, "uint8_t transparent_mask[");
  ok &= expect_contains(source, "uint8_t partial_alpha;");
  ok &= expect_contains(source, "palette->partial_alpha = 1U;");
  ok &= expect_contains(source, "zz9k_picture_png_palette_lut8_eligible");
  ok &= expect_contains(
      source, "zz9k_picture_png_palette_lut8_eligible(&instance->png_palette)");
  ok &= expect_contains(source, "zz9k_picture_prepare_png_palette_lut8_v43");
  ok &= expect_contains(source, "zz9k_picture_write_palette_lut8_tile_to_object");
  ok &= expect_contains(source, "zz9k_picture_build_png_lut8_map");
  ok &= expect_contains(source, "zz9k_picture_png_lut8_lookup");
  ok &= expect_contains(source, "zz9k_picture_png_nearest_palette_index");
  ok &= expect_contains(
      source, "\"metadata: datatype png palette lut8 path\"");
  ok &= expect_contains(source, "\"metadata: png lut8 ready\"");
  ok &= expect_contains(source, "\"metadata: png palette entries\"");
  ok &= expect_contains(source, "\"decode: datatype lut8 tile written\"");
  ok &= expect_contains(source, "\"decode: datatype lut8 map alloc failed\"");
  ok &= expect_contains(source, "PBPAFMT_LUT8");
  ok &= expect_contains(source, "PDTA_NumColors, (ULONG)palette->count");
  ok &= expect_contains(source, "header->bmh_Depth = 8;");
  ok &= expect_contains(source, "header->bmh_Masking = mskHasTransparentColor;");
  ok &= expect_contains(source, "target->png_lut8");
  ok &= expect_not_contains(source, "ZZ9K_PICTURE_FORCE_ALPHA_RGB_COMPAT");
  ok &= expect_contains(source, "ZZ9K_PICTURE_OBJECT_NAME_BYTES 128U");
  ok &= expect_contains(source, "char object_name[ZZ9K_PICTURE_OBJECT_NAME_BYTES];");
  ok &= expect_contains(source, "uint8_t flatten_png_alpha;");
  ok &= expect_contains(source, "zz9k_picture_capture_screen_alpha_policy");
  ok &= expect_contains(source, "GetTagData(");
  ok &= expect_contains(source, "PDTA_Screen, 0UL");
  ok &= expect_contains(source, "GetBitMapAttr(screen->RastPort.BitMap, BMA_DEPTH)");
  ok &= expect_contains(source, "screen_depth <= 16U");
  ok &= expect_contains(
      source, "\"metadata: png alpha flattened for low-depth screen\"");
  ok &= expect_contains(source, "zz9k_picture_capture_object_name");
  ok &= expect_contains(source, "zz9k_picture_apply_object_name");
  ok &= expect_contains(source, "zz9k_picture_instance_object_name");
  ok &= expect_not_contains(source, "zz9k_picture_capture_alpha_matte");
  ok &= expect_not_contains(source, "zz9k_picture_screen_pen_rgb");
  ok &= expect_not_contains(source, "GetScreenDrawInfo");
  ok &= expect_not_contains(source, "LockPubScreen");
  ok &= expect_not_contains(source, "GetRGB32");
  ok &= expect_contains(source, "DTA_Name, (ULONG)&source_name");
  ok &= expect_contains(source, "DTA_ObjName, (ULONG)zz9k_picture_instance_object_name(instance)");
  ok &= expect_contains(source, "\"metadata: source object name\"");
  ok &= expect_contains(source, "\"metadata: source object name applied\"");
  ok &= expect_contains(
      source, "ZZ9K_PICTURE_FORCE_DATATYPE_V47_TRUECOLOR 0");
  ok &= expect_contains(
      source, "ZZ9K_PICTURE_FORCE_DATATYPE_V47_DIRECT 0");
  ok &= expect_contains(
      source, "ZZ9K_PICTURE_FORCE_DATATYPE_V43_WRITEPIXELS 0");
  ok &= expect_contains(
      source, "ZZ9K_PICTURE_DYNAMIC_DATATYPE_FEATURES 1");
  ok &= expect_contains(
      source, "ZZ9K_PICTURE_ENABLE_DATATYPE_V47_DIRECT 0");
  ok &= expect_contains(
      source, "ZZ9K_PICTURE_ENABLE_JPEG_DATATYPE_V47_RGB_DIRECT 0");
  ok &= expect_contains(
      source, "ZZ9K_PICTURE_FORCE_REFERENCE_V43_WRITEPIXELS 1");
  ok &= expect_contains(
      source, "ZZ9K_PICTURE_ENABLE_PNG_ALPHA_EXPERIMENTS 0");
  ok &= expect_contains(source, "zz9k_picture_read_render_mode_env");
  ok &= expect_contains(source, "char value[32];");
  ok &= expect_contains(source, "zz9k_picture_forced_render_mode_allows_env");
  /* The ENV override must be read once and cached. GM_RENDER runs with the
   * window layer locked, and Open() on ENV: sleeps on a DOS packet; doing
   * that per render deadlocks Intuition during a window resize. */
  ok &= expect_contains(source, "static uint8_t zz9k_picture_render_mode_ready;");
  ok &= expect_contains(
      source,
      "static ZZ9KPictureRenderMode zz9k_picture_cached_render_mode;");
  ok &= expect_contains(
      source,
      "  if (zz9k_picture_render_mode_ready) {\n"
      "    return zz9k_picture_cached_render_mode;\n"
      "  }\n"
      "\n"
      "  render_mode = ZZ9K_PICTURE_RENDER_MODE_DATATYPE;\n"
      "  (void)zz9k_picture_read_render_mode_env(&render_mode);\n"
      "  zz9k_picture_cached_render_mode = render_mode;\n"
      "  zz9k_picture_render_mode_ready = 1U;\n"
      "  return render_mode;");
  ok &= expect_not_contains(
      source,
      "#if ZZ9K_PICTURE_FORCE_DATATYPE_V47_TRUECOLOR || \\\n"
      "    ZZ9K_PICTURE_FORCE_DATATYPE_V47_DIRECT || \\\n"
      "    ZZ9K_PICTURE_FORCE_DATATYPE_V43_WRITEPIXELS\n"
      "  return ZZ9K_PICTURE_RENDER_MODE_DATATYPE;\n"
      "#else\n"
      "  char value[16];");
  ok &= expect_not_contains(source, "aros");
  ok &= expect_not_contains(source, "AROS");
  ok &= expect_not_contains(source, "Aros");
  ok &= expect_contains(source, "zz9k_picture_trace_reset");
  ok &= expect_contains(source, "zz9k_picture_trace_reset_path");
  ok &= expect_contains(source, "zz9k_picture_trace_reset();");
  ok &= expect_contains(source, "PICTUREDTCLASS");
  ok &= expect_contains(source, "PictureBase");
  ok &= expect_contains(
      source, "OpenLibrary((CONST_STRPTR)\"datatypes/picture.datatype\", 39)");
  ok &= expect_contains(source, "CloseLibrary(PictureBase)");
  ok &= expect_contains(source, "OpenLibrary((CONST_STRPTR)\"graphics.library\", 39)");
  ok &= expect_contains(source, "CloseLibrary((struct Library *)GfxBase)");
  ok &= expect_contains(source, "struct ClassLibrary");
  ok &= expect_contains(source, "MakeClass");
  ok &= expect_contains(source, "AddClass");
  ok &= expect_contains(source, "RemoveClass");
  ok &= expect_contains(source, "FreeClass");
  ok &= expect_contains(source, "zz9k_picture_datatype_get_class");
  ok &= expect_contains(source, "OM_NEW");
  ok &= expect_contains(source, "OM_DISPOSE");
  ok &= expect_contains(source, "GM_RENDER");
  ok &= expect_contains(source, "DTM_PROCLAYOUT");
  ok &= expect_contains(source, "DTM_ASYNCLAYOUT");
  ok &= expect_contains(source, "zz9k_picture_trace");
  ok &= expect_contains(source, "ZZ9K_PICTURE_TRACE_ENABLED 0");
  /* Render-path traces are gated separately. The trace sinks are dos.library
   * Open/Write/Close, and GM_RENDER runs with the window layer locked, so
   * tracing there deadlocks Intuition on resize. Enabling the master switch
   * alone must not put DOS I/O back on the render path. */
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_TRACE_ENABLED 0");
  ok &= expect_contains(
      source,
      "static void zz9k_picture_trace_render(const char *message)\n"
      "{\n"
      "  if (ZZ9K_PICTURE_RENDER_TRACE_ENABLED) {\n"
      "    zz9k_picture_trace(message);\n"
      "  } else {\n"
      "    (void)message;\n"
      "  }\n"
      "}");
  ok &= expect_contains(
      source,
      "  instance->render_trace_mask |= flag;\n"
      "  zz9k_picture_trace_render(message);");
  ok &= expect_contains(source, "ZZ9K_PICTURE_TRACE_RESET_ENABLED 0");
  ok &= expect_contains(source, "ZZ9K_PICTURE_SOURCE_TRACE_ENABLED 0");
  ok &= expect_contains(source, "\"T:zz9k-picture.datatype.log\"");
  ok &= expect_contains(source, "\"SYS:zz9k-picture.datatype.log\"");
  ok &= expect_contains(source, "zz9k_picture_trace_path");
  ok &= expect_contains(source, "zz9k_picture_trace_source");
  ok &= expect_contains(source, "zz9k_picture_trace_source_hex");
  ok &= expect_contains(source, "\"metadata: invalid dimensions\"");
  ok &= expect_contains(source, "zz9k_picture_set_placeholder_bitmap");
  ok &= expect_contains(source, "zz9k_picture_set_placeholder_pixels");
  ok &= expect_contains(source, "zz9k_picture_set_placeholder_best");
  ok &= expect_contains(source, "zz9k_picture_set_placeholder_obtained_pixels");
  ok &= expect_contains(source, "zz9k_picture_set_reference_pattern");
  ok &= expect_contains(source, "zz9k_picture_set_v43_reference_pattern");
  ok &= expect_contains(source, "zz9k_picture_set_v43_attrs_reference_pattern");
  ok &= expect_contains(source, "zz9k_picture_set_reference_v43_attrs");
  ok &= expect_contains(source, "zz9k_picture_prepare_datatype_v43");
  ok &= expect_contains(source, "zz9k_picture_prepare_v43_reference_header");
  ok &= expect_contains(source, "zz9k_picture_fill_reference_row");
  ok &= expect_contains(source, "zz9k_picture_fill_reference_pattern");
  ok &= expect_contains(source, "zz9k_picture_set_v43_alpha_reference_pattern");
  ok &= expect_contains(source, "zz9k_picture_set_alpha_reference_v43_attrs");
  ok &= expect_contains(source, "zz9k_picture_prepare_v43_alpha_reference_header");
  ok &= expect_contains(source, "zz9k_picture_fill_alpha_reference_row");
  ok &= expect_contains(source, "zz9k_picture_fill_alpha_reference_pattern");
  ok &= expect_contains(source, "zz9k_picture_write_v43_rgba_buffer");
  ok &= expect_contains(source, "zz9k_picture_superclass_version");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_DATATYPE");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_REFERENCE");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_REFERENCE_NOLAYOUT");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE");
  ok &= expect_contains(
      source, "ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE_NOLAYOUT");
  ok &= expect_contains(source, "zz9k_picture_trace_u32");
  ok &= expect_contains(source, "zz9k_picture_trace_hex");
  ok &= expect_contains(source, "zz9k_picture_trace_pixel_buffer");
  ok &= expect_contains(source, "datatype_sync_sent");
  ok &= expect_contains(source, "png_alpha_known");
  ok &= expect_contains(source, "png_has_alpha");
  ok &= expect_contains(source, "zz9k_picture_notify_datatype_sync");
  ok &= expect_contains(source, "zz9k_picture_decode_to_datatype_pixels");
  ok &= expect_contains(source, "zz9k_picture_decode_png_to_datatype_pixels");
  ok &= expect_contains(source, "typedef struct ZZ9KPictureStreamInput");
  ok &= expect_contains(source, "typedef struct ZZ9KPictureSource");
  ok &= expect_contains(source, "zz9k_picture_source_init_file");
  ok &= expect_contains(source, "zz9k_picture_source_init_memory");
  ok &= expect_contains(source, "zz9k_picture_get_source");
  ok &= expect_contains(source, "zz9k_picture_source_read");
  ok &= expect_contains(source, "zz9k_picture_source_seek");
  ok &= expect_contains(source, "zz9k_picture_source_size");
  ok &= expect_contains(source, "DTA_SourceAddress");
  ok &= expect_contains(source, "DTA_SourceSize");
  ok &= expect_contains(source, "DTST_MEMORY");
  ok &= expect_contains(source, "\"metadata: source file\"");
  ok &= expect_contains(source, "\"metadata: source memory\"");
  ok &= expect_contains(source, "\"metadata: source type\"");
  ok &= expect_contains(source, "\"metadata: source size\"");
  ok &= expect_contains(source, "\"decode: source unavailable\"");
  ok &= expect_contains(
      source, "zz9k_picture_trace_source(\"metadata: codec jpeg\")");
  ok &= expect_contains(
      source, "zz9k_picture_trace_source(\"metadata: codec png\")");
  ok &= expect_contains(
      source, "zz9k_picture_trace_source_hex(\"metadata: image width\"");
  ok &= expect_contains(
      source, "zz9k_picture_trace_source_hex(\"metadata: image height\"");
  ok &= expect_contains(source, "\"metadata: png alpha\"");
  ok &= expect_contains(
      source, "zz9k_picture_trace_source(\"metadata: datatype decode begin\")");
  ok &= expect_contains(
      source, "zz9k_picture_trace_source(\"metadata: datatype decode ok\")");
  ok &= expect_contains(
      source, "zz9k_picture_trace_source(\"metadata: datatype decode failed\")");
  ok &= expect_contains(source, "zz9k_picture_init_stream_input");
  ok &= expect_contains(source, "zz9k_picture_alloc_stream_input");
  ok &= expect_contains(source, "zz9k_picture_free_stream_input");
  ok &= expect_contains(source, "zz9k_picture_init_datatype_target");
  ok &= expect_contains(source, "zz9k_picture_choose_datatype_tile_layout");
  ok &= expect_contains(source, "stream_input.staging");
  ok &= expect_contains(source, "stream_input.read_scratch");
  ok &= expect_contains(source, "stream_input.read_scratch_bytes");
  ok &= expect_not_contains(source, "int staging_allocated;");
  ok &= expect_not_contains(source, "int read_scratch_allocated;");
  ok &= expect_not_contains(source, "if (tile_format == ZZ9K_SURFACE_FORMAT_RGB888)");
  ok &= expect_not_contains(source, "zz9k_picture_write_rgb_surface_to_datatype");
  ok &= expect_not_contains(source, "zz9k_picture_copy_rgb_surface_rows_to_public");
  ok &= expect_contains(source, "zz9k_picture_write_png_surface_to_datatype");
  ok &= expect_contains(source, "zz9k_picture_copy_png_local_surface_to_datatype");
  ok &= expect_contains(source, "zz9k_picture_try_png_direct_surface");
  ok &= expect_contains(source, "zz9k_picture_try_png_local_surface");
  ok &= expect_not_contains(source, "zz9k_picture_try_png_datatype_surface");
  ok &= expect_contains(source, "zz9k_picture_feed_stream_to_datatype");
  ok &= expect_not_contains(source, "zz9k_picture_copy_bgra_tile_to_rgb_pixels");
  ok &= expect_not_contains(source, "zz9k_picture_matte_alpha_white");
  ok &= expect_not_contains(source, "zz9k_picture_bgra_to_rgb_matte");
  ok &= expect_contains(source, "zz9k_picture_bgra_to_rgb");
  ok &= expect_contains(source, "zz9k_picture_bgra_to_rgba");
  ok &= expect_contains(source, "zz9k_picture_mark_alpha_header");
  ok &= expect_contains(source, "zz9k_picture_clear_alpha_header");
  ok &= expect_contains(source, "zz9k_picture_prepare_png_transparent_bitmap");
  ok &= expect_contains(source, "zz9k_picture_write_alpha_bitmap_tile");
  ok &= expect_contains(source, "zz9k_picture_write_alpha_surface_to_bitmap");
  ok &= expect_contains(source, "zz9k_picture_rgba_to_transparent_lut8_index");
  ok &= expect_contains(source, "zz9k_picture_prepare_alpha_lut8_palette");
  ok &= expect_contains(source, "mskHasAlpha");
  ok &= expect_contains(source, "PDTA_AlphaChannel, TRUE");
  ok &= expect_contains(source, "PDTA_AlphaChannel, FALSE");
  ok &= expect_contains(source, "mskHasTransparentColor");
  ok &= expect_contains(source, "bmh_Transparent = 0");
  ok &= expect_contains(source, "PDTA_BitMap");
  ok &= expect_contains(source, "PDTA_FreeSourceBitMap");
  ok &= expect_contains(source, "AllocBitMap(");
  ok &= expect_contains(source, "WriteChunkyPixels(");
  ok &= expect_contains(source, "struct RastPort");
  ok &= expect_not_contains(source, "zz9k_picture_prepare_png_transparent_lut8");
  ok &= expect_not_contains(source, "zz9k_picture_write_alpha_lut8_tile_to_object");
  ok &= expect_not_contains(source, "PDTA_MaskPlane");
  ok &= expect_not_contains(source, "mskHasMask");
  ok &= expect_not_contains(source, "AllocRaster");
  ok &= expect_not_contains(source, "FreeRaster");
  ok &= expect_not_contains(source, "alpha_mask");
  ok &= expect_contains(source, "zz9k_picture_obtain_png_direct_pixels");
  ok &= expect_contains(source, "zz9k_picture_set_png_v47_direct_attrs");
  ok &= expect_contains(source, "zz9k_picture_copy_raw_tile_to_direct_pixels");
  ok &= expect_contains(source, "zz9k_picture_png_has_alpha");
  ok &= expect_contains(source, "zz9k_picture_prepare_png_datatype_v43");
  ok &= expect_contains(source, "PDTA_Remap, FALSE");
  ok &= expect_contains(source, "zz9k_picture_choose_datatype_tile_format");
  ok &= expect_contains(source, "zz9k_picture_choose_png_datatype_tile_format");
  ok &= expect_contains(source, "zz9k_picture_write_rgb_tile_to_object");
  ok &= expect_not_contains(source, "zz9k_picture_write_alpha_compat_tile_to_object");
  ok &= expect_not_contains(source, "zz9k_picture_rgba_like_to_matte_rgb");
  ok &= expect_not_contains(source, "zz9k_picture_alpha_matte_component");
  ok &= expect_contains(source, "zz9k_picture_write_alpha_tile_to_object");
  ok &= expect_contains(source, "zz9k_picture_write_alpha_tile_rows_to_object");
  ok &= expect_contains(source, "zz9k_picture_clear_transparent_alpha_rgb");
  ok &= expect_contains(source, "zz9k_picture_alpha_surface_all_opaque");
  ok &= expect_contains(source, "zz9k_picture_write_alpha_surface_as_rgb");
  ok &= expect_contains(source, "\"decode: png alpha surface is opaque\"");
  ok &= expect_contains(source, "\"decode: png opaque rgba surface write\"");
  ok &= expect_contains(source, "pixel_format == PBPAFMT_RGBA");
  ok &= expect_contains(source, "pixel_format == PBPAFMT_ARGB");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_IMAGE_RGB888_OUTPUT");
  ok &= expect_contains(source, "ZZ9K_SURFACE_FORMAT_RGB888");
  ok &= expect_contains(source, "ZZ9K_SURFACE_FORMAT_RGBA8888");
  ok &= expect_contains(source, "\"metadata: datatype png rgba alpha tiles\"");
  ok &= expect_contains(source, "ZZ9K_SURFACE_FLAG_ARM_LOCAL");
  ok &= expect_contains(source, "zz9k_surface_build_copy_desc");
  ok &= expect_contains(source, "zz9k_copy_surface");
  ok &= expect_contains(source, "\"metadata: datatype rgb888 tiles\"");
  ok &= expect_contains(source, "\"metadata: datatype direct rgb888 tiles\"");
  ok &= expect_contains(
      source,
      "\"metadata: datatype direct rgb888 unavailable; v43 fallback\"");
  ok &= expect_contains(source, "\"metadata: datatype bgra fallback tiles\"");
  ok &= expect_contains(source, "zz9k_picture_direct_pixels_prefer_rgb888_tiles");
  ok &= expect_contains(
      source,
      "direct_pixels->pbpa_PixelFormat == PBPAFMT_RGB");
  ok &= expect_contains(
      source,
      "if (target.direct &&\n"
      "      !zz9k_picture_direct_pixels_prefer_rgb888_tiles(\n"
      "          &image_service, target.direct_pixels))");
  ok &= expect_contains(
      source,
      "target.direct = 0U;\n"
      "    target.direct_pixels = 0;");
  ok &= expect_not_contains(source, "zz9k_picture_write_full_rgb_to_object");
  ok &= expect_not_contains(source, "\"metadata: datatype full buffer ready\"");
  ok &= expect_not_contains(source, "\"decode: datatype before full buffer alloc\"");
  ok &= expect_not_contains(source, "\"decode: datatype full pixels written\"");
  ok &= expect_contains(source, "zz9k_picture_write_bgra_tile_to_object");
  ok &= expect_contains(source, "zz9k_picture_write_argb_tile_to_object");
  ok &= expect_contains(
      source,
      "target->output_format == ZZ9K_SURFACE_FORMAT_RGB888 &&\n"
      "      target->direct_pixels->pbpa_PixelFormat == PBPAFMT_RGB");
  ok &= expect_contains(
      source,
      "zz9k_picture_copy_raw_tile_to_direct_pixels(\n"
      "            tile, result, target->direct_pixels, tile_stride)");
  ok &= expect_not_contains(
      source,
      "zz9k_picture_copy_bgra_tile_to_rgb_pixels(\n"
      "              tile, result, target->direct_pixels, tile_stride)");
  ok &= expect_contains(source, "result->tile_x > target->width");
  ok &= expect_contains(source, "result->tile_height > (target->height - result->tile_y)");
  ok &= expect_contains(source, "zz9k_picture_choose_tile_layout");
  ok &= expect_contains(
      source, "zz9k_picture_choose_png_full_datatype_tile_layout");
  ok &= expect_contains(source, "ZZ9K_PICTURE_TILE_TARGET_BYTES (128UL * 1024UL)");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RGB888_TILE_MAX_ROWS 256U");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RGB888_TILE_TARGET_BYTES (768UL * 1024UL)");
  ok &= expect_contains(
      source,
      "ZZ9K_PICTURE_DIRECT_DATATYPE_SURFACE_MAX_BYTES \\\n"
      "  (512UL * 1024UL)");
  ok &= expect_contains(source, "ZZ9K_PICTURE_STAGING_BYTES (256UL * 1024UL)");
  ok &= expect_contains(source, "ZZ9K_PICTURE_Z2_STAGING_BYTES (24UL * 1024UL)");
  ok &= expect_contains(source,
                        "ZZ9K_PICTURE_Z2_TILE_TARGET_BYTES (32UL * 1024UL)");
  ok &= expect_contains(source, "ZZ9K_CAP_APERTURE_LAYOUT");
  ok &= expect_contains(source, "ZZ9K_ALLOC_HOST_WINDOW : 0U");
  ok &= expect_contains(source, "ZZ9K_PICTURE_READ_CHUNK_BYTES (256UL * 1024UL)");
  ok &= expect_contains(source, "zz9k_picture_stream_input_bytes");
  ok &= expect_contains(source, "zz9k_picture_file_size");
  ok &= expect_contains(source, "source_size + 1U");
  ok &= expect_not_contains(source, "ZZ9K_PICTURE_PNG_STAGING_BYTES");
  ok &= expect_not_contains(source, "ZZ9K_PICTURE_PNG_READ_CHUNK_BYTES");
  ok &= expect_not_contains(source, "(uint8_t *)AllocMem((ULONG)tile_bytes, MEMF_PUBLIC)");
  ok &= expect_not_contains(source, "FreeMem(scratch_pixels, (ULONG)tile_bytes)");
  ok &= expect_contains(source, "\"decode: png before direct surface alloc\"");
  ok &= expect_contains(source, "\"decode: png direct surface ready\"");
  ok &= expect_contains(source, "\"decode: png direct surface too large\"");
  ok &= expect_contains(source, "\"decode: png before local surface alloc\"");
  ok &= expect_contains(source, "\"decode: png local surface ready\"");
  ok &= expect_contains(source, "\"decode: png before tile surface alloc\"");
  ok &= expect_contains(source, "\"decode: png tile surface copied\"");
  ok &= expect_contains(source, "\"decode: png direct surface write\"");
  ok &= expect_not_contains(source, "\"decode: png rgb scratch alloc failed\"");
  ok &= expect_not_contains(source, "\"decode: png rgba scratch alloc failed\"");
  ok &= expect_contains(source, "pixels.pbpa_PixelFormat = PBPAFMT_RGB;");
  ok &= expect_contains(source, "pixels.pbpa_PixelFormat = pixel_format;");
  ok &= expect_contains(
      source,
      "result->tile_width, result->tile_height, tile_stride))");
  ok &= expect_contains(source, "pixels.pbpa_PixelArrayMod = 0;");
  ok &= expect_contains(source, "pixels.pbpa_Height = 1;");
  ok &= expect_contains(source, "\"decode: datatype alpha transparent rgb cleared\"");
  ok &= expect_contains(source, "\"decode: datatype alpha tile written\"");
  ok &= expect_contains(source, "tile.data = surface->data;");
  ok &= expect_contains(source, "tile.length = surface->length;");
  ok &= expect_contains(source, "tile.data = tile_surface.data;");
  ok &= expect_contains(source, "tile.length = tile_surface.length;");
  ok &= expect_contains(source, "zz9k_picture_copy_tile_to_datatype(");
  ok &= expect_contains(source, "\"decode: datatype direct alpha tile copied\"");
  ok &= expect_not_contains(source, "\"decode: png tile scratch alloc failed\"");
  ok &= expect_not_contains(source, "\"decode: png tile copy to public failed\"");
  ok &= expect_not_contains(source, "\"decode: png surface data unavailable\"");
  ok &= expect_contains(source, "read_scratch");
  ok &= expect_contains(source, "zz9k_picture_query_cached_caps");
  ok &= expect_contains(source, "zz9k_picture_query_cached_image_service");
  ok &= expect_contains(source, "zz9k_picture_open_cached_context");
  ok &= expect_contains(source, "zz9k_picture_close_cached_context");
  ok &= expect_contains(source, "zz9k_picture_cached_ctx");
  ok &= expect_contains(source, "zz9k_picture_cached_caps_ready");
  ok &= expect_contains(source, "zz9k_picture_cached_image_service_ready");
  ok &= expect_contains(source, "#include <exec/semaphores.h>");
  ok &= expect_contains(
      source, "struct SignalSemaphore zz9k_picture_decode_semaphore");
  ok &= expect_contains(
      source, "InitSemaphore(&zz9k_picture_decode_semaphore)");
  ok &= expect_contains(source, "zz9k_picture_decode_lock_obtain");
  ok &= expect_contains(source, "zz9k_picture_decode_lock_release");
  ok &= expect_contains(
      source, "ObtainSemaphore(&zz9k_picture_decode_semaphore)");
  ok &= expect_contains(
      source, "ReleaseSemaphore(&zz9k_picture_decode_semaphore)");
  ok &= expect_contains(source, "\"decode: serialize obtain\"");
  ok &= expect_contains(source, "\"decode: serialize release\"");
  ok &= expect_contains(
      source,
      "if (!zz9k_picture_decode_lock_obtain()) {\n"
      "    failure = \"decode: serialize unavailable\";\n"
      "    goto cleanup;\n"
      "  }\n"
      "  decode_lock_held = 1;");
  ok &= expect_contains(
      source,
      "if (decode_lock_held) {\n"
      "    zz9k_picture_decode_lock_release();\n"
      "  }");
  ok &= expect_contains(
      source, "zz9k_picture_open_cached_context(&ctx)");
  ok &= expect_contains(
      source, "zz9k_picture_close_cached_context();");
  ok &= expect_contains(source, "zz9k_picture_require_datatype_caps");
  ok &= expect_contains(source, "zz9k_picture_obtain_direct_pixels");
  ok &= expect_contains(source, "zz9k_picture_obtain_truecolor_direct_pixels");
  ok &= expect_contains(source, "zz9k_picture_prepare_datatype_v47_direct");
  ok &= expect_contains(source, "zz9k_picture_prepare_datatype_v47_truecolor");
  ok &= expect_contains(source, "zz9k_picture_set_v47_direct_attrs");
  ok &= expect_contains(source, "zz9k_picture_pixel_buffer_format_usable");
  ok &= expect_contains(source, "zz9k_picture_direct_pixel_buffer_valid");
  ok &= expect_contains(source, "zz9k_picture_prepare_lut8_palette");
  ok &= expect_contains(source, "PDTM_OBTAINPIXELARRAY");
  ok &= expect_contains(source, "struct pdtObtainPixelArray");
  ok &= expect_contains(source, "POPAF_WRITEGREY8");
  ok &= expect_contains(source, "\"metadata: datatype decode begin\"");
  ok &= expect_contains(source, "\"metadata: datatype pixels ready\"");
  ok &= expect_contains(source, "zz9k_picture_finalize_datatype_v43_attrs");
  ok &= expect_contains(
      source,
      "if (target.legacy_bitmap) {\n"
      "      if (!zz9k_picture_publish_legacy_bitmap(\n"
      "              object, instance, target.legacy_bitmap)) {\n"
      "        failure = \"metadata: datatype legacy bitmap publish failed\";\n"
      "        ok = 0;\n"
      "      } else {\n"
      "        target.legacy_bitmap = 0;\n"
      "      }\n"
      "    } else if (!zz9k_picture_finalize_datatype_v43_attrs(");
  ok &= expect_contains(
      source,
      "if (decode_lock_held) {\n"
      "    zz9k_picture_decode_lock_release();\n"
      "  }\n"
      "  if (scratch_allocated) {\n"
      "    FreeMem(scratch_pixels, (ULONG)scratch_bytes);\n"
      "  }\n"
      "  if (ok) {\n"
      "    if (target.legacy_bitmap) {");
  ok &= expect_contains(
      source,
      "      DTA_NominalHoriz, instance->width,\n"
      "      DTA_NominalVert, instance->height,\n"
      "      DTA_ObjName, (ULONG)zz9k_picture_object_name(instance->codec),");
  ok &= expect_contains(
      source, "\"metadata: datatype final attrs ready\"");
  ok &= expect_contains(
      source,
      "if (png_has_alpha) {\n"
      "    zz9k_picture_trace_source(\n"
      "        \"metadata: datatype png alpha v43 rgba path\");");
  ok &= expect_contains(
      source, "\"metadata: datatype png alpha surface path\"");
  ok &= expect_contains(
      source, "\"metadata: datatype png alpha rgb compatibility path\"");
  ok &= expect_contains(
      source,
      "if (version >= 43U && png_has_alpha &&\n"
      "      instance->flatten_png_alpha) {\n"
      "    zz9k_picture_trace_source(\n"
      "        \"metadata: datatype png alpha rgb compatibility path\");\n"
      "    png_has_alpha = 0;\n"
      "    instance->png_has_alpha = 0U;\n"
      "  }");
  ok &= expect_contains(
      source, "\"metadata: datatype png alpha v43 rgba path\"");
  ok &= expect_contains(
      source,
      "#if ZZ9K_PICTURE_ENABLE_PNG_ALPHA_EXPERIMENTS\n"
      "    zz9k_picture_trace_source(\n"
      "        \"metadata: datatype png alpha surface path\");\n"
      "    if (!zz9k_picture_decode_png_to_datatype_pixels(");
  ok &= expect_not_contains(source, "int png_alpha_rgb_compat");
  ok &= expect_not_contains(
      source,
      "png_has_alpha || png_alpha_rgb_compat");
  ok &= expect_not_contains(
      source,
      "target.alpha_rgb_compat = png_alpha_rgb_compat ? 1U : 0U;");
  ok &= expect_not_contains(
      source,
      "target.output_format == ZZ9K_SURFACE_FORMAT_BGRA8888 ||\n"
      "       target.alpha_rgb_compat");
  ok &= expect_not_contains(
      source, "\"decode: datatype alpha rgb matte tile written\"");
  ok &= expect_contains(
      source, "\"decode: png alpha surface is opaque\"");
  ok &= expect_contains(
      source, "\"metadata: png datatype deferred rgb prepare\"");
  ok &= expect_contains(
      source, "\"metadata: png datatype classic bitmap prepare\"");
  ok &= expect_contains(
      source, "\"decode: datatype alpha bitmap written\"");
  ok &= expect_contains(source, "zz9k_picture_write_png_alpha_tile_to_object");
  ok &= expect_contains(
      source,
      "return zz9k_picture_write_png_alpha_tile_to_object(\n"
      "        tile, result, target, tile_stride);");
  ok &= expect_contains(
      source,
      "if (has_alpha) {\n"
      "    *output_format = ZZ9K_SURFACE_FORMAT_RGBA8888;\n"
      "    *output_bpp = ZZ9K_PICTURE_RGBA_BYTES_PER_PIXEL;\n"
      "    zz9k_picture_trace(\"metadata: datatype png rgba alpha tiles\");");
  ok &= expect_not_contains(
      source,
      "#if ZZ9K_PICTURE_ENABLE_PNG_ALPHA_EXPERIMENTS\n"
      "  if (has_alpha) {\n"
      "    *output_format = ZZ9K_SURFACE_FORMAT_RGBA8888;");
  ok &= expect_contains(source, "int *opaque_alpha_out");
  ok &= expect_contains(source, "int png_alpha_opaque");
  ok &= expect_contains(
      source, "\"metadata: datatype png opaque alpha rgb tile path\"");
  ok &= expect_contains(
      source,
      "if (opaque_alpha_out) {\n"
      "      *opaque_alpha_out = 1;\n"
      "    }\n"
      "    return 0;");
  ok &= expect_contains(
      source,
      "      if (png_alpha_opaque) {\n"
      "        zz9k_picture_trace_source(\n"
      "            \"metadata: datatype png opaque alpha rgb tile path\");\n"
      "        png_has_alpha = 0;\n"
      "        instance->png_has_alpha = 0U;\n"
      "      } else {\n"
      "        return 0;\n"
      "      }");
  ok &= expect_contains(
      source, "zz9k_picture_finalize_datatype_v43_attrs(object, instance)");
  ok &= expect_contains(
      source,
      "if (!zz9k_picture_decode_lock_obtain()) {\n"
      "    failure = \"decode: serialize unavailable\";");
  ok &= expect_contains(source, "\"metadata: datatype decode failed; placeholder\"");
  ok &= expect_contains(source, "\"metadata: png datatype decode failed; aborting\"");
  ok &= expect_contains(source, "codec == ZZ9K_PICTURE_CODEC_PNG");
  ok &= expect_not_contains(
      source,
      "\"metadata: datatype png alpha unsupported; aborting safely\"");
  ok &= expect_not_contains(
      source,
      "\"metadata: png alpha unsupported; aborting safely\"");
  ok &= expect_contains(source, "SetIoErr(DTERROR_INVALID_DATA);");
  ok &= expect_contains(source, "\"metadata: reference pattern begin\"");
  ok &= expect_contains(source, "\"metadata: reference before attrs\"");
  ok &= expect_contains(source, "\"metadata: reference attrs ready\"");
  ok &= expect_contains(source, "\"metadata: reference header skipped\"");
  ok &= expect_not_contains(source, "zz9k_picture_prepare_reference_header");
  ok &= expect_not_contains(source, "\"metadata: reference header ready\"");
  ok &= expect_contains(source, "\"metadata: reference before pixel alloc\"");
  ok &= expect_contains(source, "\"metadata: reference pixels ready\"");
  ok &= expect_contains(source, "\"metadata: reference before pixel write\"");
  ok &= expect_contains(source, "\"metadata: reference pixels written\"");
  ok &= expect_contains(source, "\"metadata: reference pixels retained\"");
  ok &= expect_contains(
      source, "\"metadata: forced datatype v43 writepixelarray\"");
  ok &= expect_contains(source, "\"decode: datatype before tile layout\"");
  ok &= expect_contains(
      source, "\"metadata: forced datatype v47 direct\"");
  ok &= expect_contains(
      source, "\"metadata: forced datatype v47 truecolor\"");
  ok &= expect_contains(
      source, "\"metadata: datatype v43 prepare begin\"");
  ok &= expect_contains(
      source, "\"metadata: datatype v43 prepare ready\"");
  ok &= expect_contains(
      source, "\"metadata: datatype v47 direct prepare begin\"");
  ok &= expect_contains(
      source, "\"metadata: datatype v47 direct prepare ready\"");
  ok &= expect_contains(
      source, "\"metadata: datatype v47 direct path\"");
  ok &= expect_contains(
      source, "\"metadata: datatype v47 direct unavailable; v43 fallback\"");
  ok &= expect_contains(
      source, "\"metadata: datatype v47 truecolor prepare begin\"");
  ok &= expect_contains(
      source, "\"metadata: datatype v47 truecolor prepare ready\"");
  ok &= expect_contains(
      source, "\"metadata: datatype v47 truecolor path\"");
  ok &= expect_contains(
      source, "\"metadata: datatype v47 truecolor unavailable; v43 fallback\"");
  ok &= expect_contains(
      source, "\"metadata: v47 indexed buffer rejected\"");
  ok &= expect_contains(source, "\"metadata: v43 reference before header\"");
  ok &= expect_contains(source, "\"metadata: v43 reference header ready\"");
  ok &= expect_contains(
      source, "\"metadata: v43 reference before pixel alloc\"");
  ok &= expect_contains(
      source, "\"metadata: v43 reference full buffer ready\"");
  ok &= expect_contains(source, "\"metadata: v43 reference before attrs\"");
  ok &= expect_contains(
      source, "\"metadata: v43 reference attrs ready\"");
  ok &= expect_contains(
      source,
      "static int zz9k_picture_set_reference_v43_attrs");
  ok &= expect_contains(
      source,
      "DTA_ErrorNumber, 0,\n"
      "      DTA_NominalHoriz, width,\n"
      "      DTA_NominalVert, height,\n"
      "      PDTA_ModeID, 0,\n"
      "      PDTA_SourceMode, PMODE_V43,\n"
      "      PDTA_DestMode, PMODE_V43,\n"
      "      PDTA_SubClassRendersAll, FALSE,\n"
      "      PDTA_Remap, TRUE,\n"
      "      DTA_ObjName");
  ok &= expect_contains(
      source,
      "      DTA_ErrorNumber, 0,\n"
      "      DTA_NominalHoriz, width,\n"
      "      DTA_NominalVert, height,\n"
      "      PDTA_ModeID, 0,\n"
      "      PDTA_SourceMode, PMODE_V43,\n"
      "      PDTA_DestMode, PMODE_V43,\n"
      "      PDTA_SubClassRendersAll, FALSE,\n"
      "      PDTA_Remap, TRUE,\n"
      "      DTA_ObjName, (ULONG)zz9k_picture_object_name(codec),");
  ok &= expect_contains(source, "zz9k_picture_write_v43_reference_pattern");
  ok &= expect_contains(source, "zz9k_picture_write_v43_rgb_buffer");
  ok &= expect_contains(
      source,
      "if (!zz9k_picture_write_v43_rgb_buffer(\n"
      "          cl, object, pixel_data, 0U, 0U, width, height, row_bytes))");
  ok &= expect_contains(
      source,
      "if (!zz9k_picture_write_v43_rgb_buffer(\n"
      "      target->cl, target->object, (uint8_t *)tile->data,\n"
      "      result->tile_x, result->tile_y,\n"
      "      result->tile_width, result->tile_height, tile_stride))");
  ok &= expect_not_contains(
      source,
      "pixels.pbpa_PixelData = (APTR)tile->data;\n"
      "  pixels.pbpa_PixelFormat = PBPAFMT_RGB;\n"
      "  pixels.pbpa_PixelArrayMod = tile_stride;");
  ok &= expect_not_contains(source,
                            "zz9k_picture_write_v43_reference_rows");
  ok &= expect_contains(
      source, "\"metadata: v43 reference full buffer ready\"");
  ok &= expect_contains(
      source, "\"metadata: v43 reference pixel write done\"");
  ok &= expect_contains(source, "\"metadata: v43 reference final attrs ready\"");
  ok &= expect_contains(source, "DoSuperMethodA(cl, object, (Msg)&pixels)");
  ok &= expect_contains(source, "pixels.pbpa_Height = height;");
  ok &= expect_contains(source,
                        "\"metadata: v43 alpha reference before header\"");
  ok &= expect_contains(source,
                        "\"metadata: v43 alpha reference header ready\"");
  ok &= expect_contains(source,
                        "\"metadata: v43 alpha reference before attrs\"");
  ok &= expect_contains(source,
                        "\"metadata: v43 alpha reference attrs ready\"");
  ok &= expect_contains(source,
                        "\"metadata: v43 alpha reference full buffer ready\"");
  ok &= expect_contains(source,
                        "\"metadata: v43 alpha reference pixel write done\"");
  ok &= expect_contains(source,
                        "\"metadata: v43 alpha reference final attrs ready\"");
  ok &= expect_contains(source,
                        "static int zz9k_picture_set_alpha_reference_v43_attrs");
  ok &= expect_contains(
      source,
      "      PDTA_ModeID, 0,\n"
      "      PDTA_SourceMode, PMODE_V43,\n"
      "      PDTA_DestMode, PMODE_V43,\n"
      "      PDTA_SubClassRendersAll, FALSE,\n"
      "      PDTA_Remap, FALSE,\n"
      "      PDTA_AlphaChannel, TRUE,\n");
  ok &= expect_contains(source, "header->bmh_Depth = 32;");
  ok &= expect_contains(source, "header->bmh_Masking = mskHasAlpha;");
  ok &= expect_contains(source, "pixels.pbpa_PixelFormat = PBPAFMT_RGBA;");
  ok &= expect_contains(source,
      "if (!zz9k_picture_write_v43_rgba_buffer(\n"
      "          cl, object, pixel_data, 0U, 0U, width, height, row_bytes))");
  ok &= expect_contains(source,
      "if (zz9k_picture_alpha_reference_mode(render_mode))");
  ok &= expect_contains(source,
      "zz9k_picture_set_v43_alpha_reference_pattern(");
  ok &= expect_contains(source, "\"metadata: obtain pixel buffer tag\"");
  ok &= expect_contains(source, "\"metadata: obtain pixel buffer method\"");
  ok &= expect_contains(source, "\"metadata: v47 request format\"");
  ok &= expect_contains(source, "\"metadata: superclass version\"");
  ok &= expect_contains(source, "\"metadata: image width\"");
  ok &= expect_contains(source, "\"metadata: image height\"");
  ok &= expect_contains(source, "\"metadata: v47 tag result\"");
  ok &= expect_contains(source, "\"metadata: v47 method result\"");
  ok &= expect_contains(source, "\"metadata: v47 pixeldata\"");
  ok &= expect_contains(source, "\"metadata: v47 pixelformat\"");
  ok &= expect_contains(source, "\"metadata: v47 pixelmod\"");
  ok &= expect_contains(source, "\"metadata: v47 pixelwidth\"");
  ok &= expect_contains(source, "\"metadata: v47 pixelheight\"");
  ok &= expect_contains(source, "\"metadata: v47 direct required; placeholder\"");
  ok &= expect_contains(source, "\"metadata: v43 fallback allowed\"");
  ok &= expect_contains(source, "\"metadata: datatype writepixelarray path\"");
  ok &= expect_contains(source, "\"metadata: datatype writepixelarray ready\"");
  ok &= expect_contains(source, "\"metadata: png alpha detected\"");
  ok &= expect_contains(source, "\"metadata: png alpha scan failed\"");
  ok &= expect_not_contains(
      source, "\"metadata: png interlace unsupported; declining\"");
  ok &= expect_not_contains(source, "\"metadata: png alpha unsupported; aborting safely\"");
  ok &= expect_contains(source, "\"metadata: datatype png alpha detected\"");
  ok &= expect_contains(source, "\"metadata: datatype png alpha scan failed\"");
  ok &= expect_not_contains(
      source,
      "\"metadata: datatype png alpha unsupported; aborting safely\"");
  ok &= expect_contains(source, "\"metadata: png datatype v43 prepare begin\"");
  ok &= expect_contains(source, "\"metadata: png datatype v43 prepare ready\"");
  ok &= expect_contains(
      source,
      "if (has_alpha) {\n"
      "    if (!zz9k_picture_prepare_v43_alpha_reference_header(\n"
      "            object, instance->width, instance->height))");
  ok &= expect_contains(
      source,
      "    if (!zz9k_picture_set_alpha_reference_v43_attrs(\n"
      "            object, instance->codec, instance->width, instance->height))");
  ok &= expect_not_contains(source, "zz9k_picture_png_interlace_supported");
  ok &= expect_contains(source, "\"metadata: png v47 direct pixels ready\"");
  ok &= expect_contains(source, "\"metadata: png v47 direct pixels unavailable\"");
  ok &= expect_contains(source, "\"metadata: png v47 direct attrs ready\"");
  ok &= expect_not_contains(source, "\"metadata: png datatype v47 direct path\"");
  ok &= expect_not_contains(source, "\"metadata: png v47 direct fallback to v43 alpha\"");
  ok &= expect_contains(
      source,
      "zz9k_picture_prepare_direct_pixel_header(\n"
      "            object, pixels, instance->width, instance->height)");
  ok &= expect_contains(source, "output_format = ZZ9K_SURFACE_FORMAT_RGB888;");
  ok &= expect_contains(
      source,
      "if (!zz9k_picture_prepare_png_datatype_v43(\n"
      "            object, instance, 0))");
  ok &= expect_contains(
      source,
      "if (!zz9k_picture_write_alpha_surface_to_bitmap(");
  ok &= expect_not_contains(
      source,
      "if (!zz9k_picture_prepare_png_datatype_v43(\n"
      "          object, instance, has_alpha)) {\n"
      "    zz9k_picture_trace(\"metadata: datatype png v43 prepare failed\");\n"
      "    return 0;\n"
      "  }\n"
      "\n"
      "  if (!zz9k_picture_decode_lock_obtain())");
  ok &= expect_not_contains(
      source,
      "zz9k_picture_trace_source(\n"
      "        \"metadata: png datatype deferred rgb prepare\");\n"
      "    if (!zz9k_picture_prepare_png_datatype_v43(object, instance, 0))");
  ok &= expect_contains(
      source, "\"decode: datatype png full tile layout failed\"");
  ok &= expect_contains(source, "\"decode: png trying cpu-visible direct surface\"");
  ok &= expect_contains(source, "\"decode: png trying local direct surface\"");
  ok &= expect_not_contains(source, "\"decode: png falling back to bgra surface\"");
  ok &= expect_not_contains(source, "\"decode: rgb888 output missing\"");
  ok &= expect_contains(source, "\"decode: png before direct surface alloc\"");
  ok &= expect_contains(source, "\"decode: png direct surface unavailable\"");
  ok &= expect_contains(source, "\"decode: png before local surface alloc\"");
  ok &= expect_contains(source, "\"decode: png local surface unavailable\"");
  ok &= expect_contains(source, "\"decode: png surface write failed\"");
  ok &= expect_contains(source, "instance->codec == ZZ9K_PICTURE_CODEC_PNG");
  ok &= expect_contains(source, "ZZ9K_IMAGE_OUTPUT_SURFACE");
  ok &= expect_contains(source, "ZZ9K_SURFACE_FORMAT_RGB888");
  ok &= expect_contains(source, "ZZ9K_SURFACE_FORMAT_ARGB8888");
  ok &= expect_contains(source, "ZZ9K_SURFACE_FORMAT_RGBA8888");
  ok &= expect_contains(source, "ZZ9K_SURFACE_FORMAT_BGRA8888");
  ok &= expect_contains(source, "ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL");
  ok &= expect_contains(source, "zz9k_image_build_surface_session_begin_desc");
  ok &= expect_contains(source, "surface.flags & ZZ9K_SURFACE_FLAG_CPU_VISIBLE");
  ok &= expect_contains(source, "local_surface.flags & ZZ9K_SURFACE_FLAG_ARM_LOCAL");
  ok &= expect_contains(source, "tile_surface.flags & ZZ9K_SURFACE_FLAG_CPU_VISIBLE");
  ok &= expect_contains(source, "tile_surface.data");
  ok &= expect_contains(source, "\"metadata: v43 pixel attrs set\"");
  ok &= expect_contains(source, "\"layout: datatype pixels; superclass\"");
  ok &= expect_contains(source, "\"layout: datatype superclass returned\"");
  ok &= expect_contains(source, "\"layout: datatype sync notified\"");
  ok &= expect_contains(source, "\"layout: reference superclass returned\"");
  ok &= expect_contains(source, "\"layout: reference nolayout skip\"");
  ok &= expect_contains(source, "\"layout: reference nolayout notified\"");
  ok &= expect_contains(source, "\"layout: v43 reference superclass returned\"");
  ok &= expect_contains(source, "\"render: mode datatype; superclass\"");
  ok &= expect_contains(source, "\"render: mode reference; superclass\"");
  ok &= expect_contains(
      source,
      "zz9k_picture_decode_to_datatype_pixels(\n"
      "            cl, object, &source, instance)");
  ok &= expect_contains(source, "target->cl = cl;");
  ok &= expect_contains(source, "DoSuperMethodA(target->cl, target->object, (Msg)&pixels)");
  ok &= expect_contains(source, "zz9k_picture_load_metadata(cl, new_object, instance)");
  ok &= expect_contains(source, "\"decode: datatype tile layout ready\"");
  ok &= expect_contains(source, "\"decode: datatype tile max rows\"");
  ok &= expect_contains(source, "\"decode: datatype tile target bytes\"");
  ok &= expect_contains(source, "\"decode: datatype tile rows\"");
  ok &= expect_contains(source, "\"decode: datatype tile pitch\"");
  ok &= expect_contains(source, "\"decode: datatype tile bytes\"");
  ok &= expect_contains(source, "\"decode: datatype staging bytes\"");
  ok &= expect_contains(source, "\"decode: datatype before staging alloc\"");
  ok &= expect_contains(source, "\"decode: datatype staging alloc ok\"");
  ok &= expect_contains(source, "\"decode: datatype before tile alloc\"");
  ok &= expect_contains(source, "\"decode: datatype tile alloc ok\"");
  ok &= expect_contains(source, "\"decode: datatype image codec\"");
  ok &= expect_contains(source, "\"decode: datatype tile handle\"");
  ok &= expect_contains(source, "\"decode: datatype before session desc\"");
  ok &= expect_contains(source, "\"decode: datatype session desc ready\"");
  ok &= expect_contains(source, "\"decode: datatype before session begin\"");
  ok &= expect_contains(source, "\"decode: datatype session begin status\"");
  ok &= expect_contains(source, "\"decode: datatype session id\"");
  ok &= expect_contains(source, "\"decode: datatype session state\"");
  ok &= expect_contains(source, "\"decode: datatype session begin ok\"");
  ok &= expect_contains(source, "\"decode: datatype before seek\"");
  ok &= expect_contains(source, "\"decode: datatype seek ready\"");
  ok &= expect_contains(source, "\"decode: datatype before feed\"");
  ok &= expect_contains(source, "ZZ9K_PICTURE_DATATYPE_TRACE_VERBOSE 0");
  ok &= expect_contains(source, "ZZ9K_PICTURE_DATATYPE_TRACE_CHUNKS 0U");
  ok &= expect_contains(source, "uint32_t trace_chunks");
  ok &= expect_contains(source, "uint32_t *trace_chunks");
  ok &= expect_contains(source, "\"decode: datatype feed loop\"");
  ok &= expect_contains(source, "\"decode: datatype before fill staging\"");
  ok &= expect_contains(source, "\"decode: datatype fill staging ok\"");
  ok &= expect_contains(source, "\"decode: datatype buffered\"");
  ok &= expect_contains(source, "\"decode: datatype eof\"");
  ok &= expect_contains(source, "\"decode: datatype before feed desc\"");
  ok &= expect_contains(source, "\"decode: datatype feed desc ready\"");
  ok &= expect_contains(source, "\"decode: datatype feed length\"");
  ok &= expect_contains(source, "\"decode: datatype feed flags\"");
  ok &= expect_contains(source, "\"decode: datatype before session feed\"");
  ok &= expect_contains(source, "\"decode: datatype session feed status\"");
  ok &= expect_contains(source, "\"decode: datatype result state\"");
  ok &= expect_contains(source, "\"decode: datatype result consumed\"");
  ok &= expect_contains(source, "\"decode: datatype result written\"");
  ok &= expect_contains(source, "\"decode: datatype result tile x\"");
  ok &= expect_contains(source, "\"decode: datatype result tile y\"");
  ok &= expect_contains(source, "\"decode: datatype result tile width\"");
  ok &= expect_contains(source, "\"decode: datatype result tile height\"");
  ok &= expect_contains(source, "\"decode: datatype before tile copy\"");
  ok &= expect_contains(source, "\"decode: datatype tile copy ok\"");
  ok &= expect_contains(source, "zz9k_picture_accumulate_byte_offset");
  ok &= expect_contains(source, "src_row += tile_stride;");
  ok &= expect_contains(source, "dst_row += (uint32_t)pixels->pbpa_PixelArrayMod;");
  ok &= expect_not_contains(source, "\"decode: datatype direct before first luma\"");
  ok &= expect_not_contains(source, "zz9k_picture_bgra_to_luma");
  ok &= expect_contains(source, "\"stream: datatype chunk offset\"");
  ok &= expect_contains(source, "\"stream: datatype chunk capacity\"");
  ok &= expect_contains(source, "\"stream: datatype before file read\"");
  ok &= expect_contains(source, "\"stream: datatype file read bytes\"");
  ok &= expect_contains(source, "\"stream: datatype before shared byte copy\"");
  ok &= expect_contains(source, "\"stream: datatype shared byte copy ok\"");
  ok &= expect_not_contains(source, "\"decode: datatype tile copied\"");
  ok &= expect_contains(source, "\"decode: datatype tile written\"");
  ok &= expect_contains(source, "\"decode: datatype final png tile\"");
  ok &= expect_contains(source, "ZZ9K_IMAGE_SESSION_RESULT_PARTIAL");
  ok &= expect_contains(
      source,
      "codec == ZZ9K_PICTURE_CODEC_PNG &&\n"
      "            eof && buffered == 0U");
  ok &= expect_contains(source, "\"decode: datatype tiles written\"");
  ok &= expect_contains(source, "target->tiles_written++");
  ok &= expect_contains(source, "\"decode: datatype feed complete\"");
  ok &= expect_contains(source, "ZZ9K_IMAGE_OUTPUT_TILE_BUFFER");
  ok &= expect_contains(source, "zz9k_image_build_tile_session_begin_desc");
  ok &= expect_contains(source, "\"metadata: placeholder bitmap allocated\"");
  ok &= expect_contains(source, "\"metadata: placeholder bitmap alloc failed\"");
  ok &= expect_contains(source, "\"metadata: placeholder pixels written\"");
  ok &= expect_contains(source, "\"metadata: placeholder pixel write failed\"");
  ok &= expect_contains(source, "\"metadata: picture.datatype v47+ path\"");
  ok &= expect_contains(source, "\"metadata: picture.datatype v43 path\"");
  ok &= expect_contains(source, "\"metadata: picture.datatype legacy bitmap path\"");
  ok &= expect_not_contains(
      source,
      "if (version < 43U) {\n"
      "    return 0;\n"
      "  }");
  ok &= expect_contains(
      source, "static int zz9k_picture_try_jpeg_datatype_v47_rgb_direct");
  ok &= expect_contains(
      source, "static int zz9k_picture_try_datatype_v43_writepixelarray");
  ok &= expect_contains(
      source, "static int zz9k_picture_prepare_legacy_bitmap");
  ok &= expect_contains(
      source, "static int zz9k_picture_decode_to_legacy_bitmap");
  ok &= expect_contains(
      source, "\"metadata: datatype jpeg v47 rgb direct path\"");
  ok &= expect_contains(
      source, "\"metadata: datatype jpeg v47 rgb unavailable; v43 fallback\"");
  ok &= expect_contains(
      source, "\"metadata: datatype v47 png uses v43 writepixelarray\"");
  ok &= expect_contains(
      source,
      "#if ZZ9K_PICTURE_ENABLE_JPEG_DATATYPE_V47_RGB_DIRECT\n"
      "  if (version >= 47U &&\n"
      "      instance->codec == ZZ9K_PICTURE_CODEC_JPEG");
  ok &= expect_contains(
      source, "\"metadata: datatype dynamic v43 writepixelarray path\"");
  ok &= expect_contains(
      source, "\"metadata: datatype dynamic legacy bitmap path\"");
  ok &= expect_contains(
      source, "\"metadata: datatype legacy bitmap ready\"");
  ok &= expect_contains(source, "WritePixelLine8(");
  ok &= expect_contains(source, "mskHasTransparentColor");
  ok &= expect_contains(
      source, "\"metadata: obtained pixel buffer unavailable\"");
  ok &= expect_not_contains(
      source, "\"metadata: obtained pixel buffer unavailable; v43 fallback\"");
  ok &= expect_contains(source, "\"metadata: obtained pixel buffer ready\"");
  ok &= expect_contains(source, "\"metadata: obtained pixel buffer cleared\"");
  ok &= expect_contains(source, "PDTA_ObtainPixelBuffer");
  ok &= expect_contains(
      source,
      "static const ULONG formats[] = {\n"
      "    PBPAFMT_RGB, PBPAFMT_RGBA, PBPAFMT_ARGB, PBPAFMT_GREY8,\n"
      "    PBPAFMT_LUT8\n"
      "  };");
  ok &= expect_contains(
      source,
      "static const ULONG truecolor_formats[] = {\n"
      "    PBPAFMT_RGB, PBPAFMT_RGBA, PBPAFMT_ARGB\n"
      "  };");
  ok &= expect_contains(source, "case PBPAFMT_LUT8:");
  ok &= expect_contains(source, "case PBPAFMT_GREY8:");
  ok &= expect_not_contains(source, "struct ColorRegister lut_colors[256];");
  ok &= expect_not_contains(source, "ULONG lut_cregs[256U * 3U];");
  ok &= expect_contains(source, "\"metadata: v47 lut8 palette ready\"");
  ok &= expect_contains(source, "\"metadata: v47 lut8 palette skipped\"");
  ok &= expect_contains(source, "zz9k_picture_prepare_direct_pixel_header");
  ok &= expect_contains(source, "\"metadata: before direct header\"");
  ok &= expect_contains(source, "\"metadata: direct header unavailable\"");
  ok &= expect_contains(source, "\"metadata: direct header depth\"");
  ok &= expect_contains(source, "\"metadata: direct header ready\"");
  ok &= expect_contains(source, "\"metadata: v47 buffer accepted\"");
  ok &= expect_contains(source, "\"metadata: v47 buffer invalid\"");
  ok &= expect_contains(source, "\"metadata: v47 grey8 lut8 buffer\"");
  ok &= expect_contains(source, "formats[i] == PBPAFMT_GREY8");
  ok &= expect_contains(source, "\"metadata: obtain pixel buffer method\"");
  ok &= expect_contains(source, "continue;");
  ok &= expect_not_contains(source, "PDTA_ColorRegisters, (ULONG)instance->lut_colors");
  ok &= expect_not_contains(source, "PDTA_CRegs, (ULONG)instance->lut_cregs");
  ok &= expect_contains(
      source, "(void)SetDTAttrs(object, 0, 0, PDTA_NumColors, 256, TAG_END);");
  ok &= expect_contains(source, "\"metadata: v47 lut8 palette unavailable\"");
  ok &= expect_contains(
      source, "\"metadata: datatype legacy lut8 palette unavailable\"");
  ok &= expect_contains(
      source, "\"metadata: png alpha lut8 palette unavailable\"");
  ok &= expect_contains(source, "uint32_t tile_rows;");
  ok &= expect_contains(
      source, "result->tile_height > target->tile_rows");
  ok &= expect_contains(source, "target.tile_rows = tile_rows;");
  ok &= expect_contains(
      source, "\"decode: datatype png full layout too large\"");
  ok &= expect_contains(source, "row_alloc_bytes");
  ok &= expect_contains(source, "obtain.popa_Flags = pixel_format == PBPAFMT_GREY8");
  ok &= expect_contains(source, "\"metadata: v47 unsupported pixel format\"");
  ok &= expect_not_contains(source, "\"metadata: v47 greyscale palette ready\"");
  ok &= expect_not_contains(source, "\"metadata: v47 greyscale palette unavailable\"");
  ok &= expect_not_contains(source, "zz9k_picture_set_greyscale_palette");
  ok &= expect_not_contains(source, "zz9k_picture_prepare_direct_palette");
  ok &= expect_contains(
      source, "pixels->pbpa_PixelFormat != requested_format");
  ok &= expect_not_contains(
      source,
      "return result != 0UL &&\n"
      "         zz9k_picture_pixel_buffer_format_usable(pixels, pixel_format) &&\n"
      "         zz9k_picture_pixel_buffer_valid(");
  ok &= expect_not_contains(source, "luma = zz9k_picture_bgra_to_luma(src);");
  ok &= expect_not_contains(source, "dst[0] = luma;");
  ok &= expect_not_contains(source, "dst[0] = src[3];");
  ok &= expect_contains(source, "dst[3] = src[3];");
  ok &= expect_contains(
      source,
      "object, pixels, instance, codec, width, height, formats[i],\n"
      "            pixel_bytes");
  ok &= expect_contains(
      source,
      "      DTA_NominalHoriz, width,\n"
      "      DTA_NominalVert, height,\n"
      "      DTA_TotalHoriz, width,\n"
      "      DTA_TotalVert, height,\n"
      "      DTA_HorizUnit, 1,\n"
      "      DTA_VertUnit, 1,\n"
      "      PDTA_SourceMode, PMODE_V43,\n"
      "      PDTA_DestMode, PMODE_V43,\n"
      "      PDTA_SubClassRendersAll, FALSE,\n"
      "      PDTA_Remap, TRUE,\n"
      "      DTA_ObjName, (ULONG)zz9k_picture_object_name(codec),");
  ok &= expect_contains(
      source,
      "      PDTA_ObtainPixelBuffer, (ULONG)pixels,\n"
      "      TAG_END);");
  ok &= expect_contains(source, "\"metadata: before v43 placeholder attrs\"");
  ok &= expect_contains(source, "\"metadata: v43 placeholder attrs set\"");
  ok &= expect_contains(source, "\"metadata: before placeholder pixel sizing\"");
  ok &= expect_contains(source, "zz9k_picture_accumulate_surface_bytes");
  ok &= expect_contains(source, "\"metadata: before placeholder pixel alloc\"");
  ok &= expect_contains(source, "\"metadata: placeholder pixel alloc ok\"");
  ok &= expect_contains(source, "\"metadata: placeholder pixel descriptor ready\"");
  ok &= expect_contains(source, "\"metadata: before placeholder pixel write\"");
  ok &= expect_not_contains(
      source, "zz9k_picture_set_placeholder_header(object, width, height, 24, 0)");
  ok &= expect_not_contains(
      source, "0xffffffffUL / ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL");
  ok &= expect_not_contains(source, "0xffffffffUL / bytes_per_pixel");
  ok &= expect_not_contains(source, "0xffffffffUL / bpp");
  ok &= expect_not_contains(source, "0xffffffffUL / row_bytes");
  ok &= expect_not_contains(source, "height > (0xffffffffUL / row_bytes)");
  ok &= expect_not_contains(
      source,
      "PDTA_SubClassRendersAll, TRUE,\n"
      "      DTA_ObjName, (ULONG)zz9k_picture_object_name(codec),\n"
      "      TAG_END);\n"
      "  zz9k_picture_trace(\"metadata: v43 placeholder attrs set\")");
  ok &= expect_contains(source, "AllocBitMap(");
  ok &= expect_contains(source, "BMF_CLEAR | BMF_DISPLAYABLE");
  ok &= expect_contains(source, "ZZ9K_PICTURE_SMALL_PLACEHOLDER_SIZE");
  ok &= expect_contains(source, "\"metadata: small placeholder active\"");
  ok &= expect_contains(source, "\"metadata: off placeholder capped\"");
  ok &= expect_contains(source, "\"metadata: datatype placeholder capped\"");
  ok &= expect_contains(source, "\"metadata: v43 small placeholder active\"");
  ok &= expect_contains(
      source,
      "render_mode == ZZ9K_PICTURE_RENDER_MODE_OFF ||\n"
      "      render_mode == ZZ9K_PICTURE_RENDER_MODE_SMALLOFF ||\n"
      "      render_mode == ZZ9K_PICTURE_RENDER_MODE_V43SMALL");
  ok &= expect_contains(source, "PDTA_BitMapHeader");
  ok &= expect_contains(source, "PDTA_BitMap");
  ok &= expect_contains(source, "PDTA_NumColors");
  ok &= expect_contains(source, "PDTA_ModeID");
  ok &= expect_contains(source, "PDTA_Remap, TRUE");
  ok &= expect_contains(source, "PDTA_Remap, remap");
  ok &= expect_contains(
      source,
      "zz9k_picture_set_v43_pixel_attrs(\n"
      "            object, instance->codec, instance->width, instance->height,\n"
      "            FALSE, TRUE)");
  ok &= expect_contains(source, "LORES_KEY");
  ok &= expect_contains(source, "(void)SetDTAttrs(");
  ok &= expect_not_contains(source, "return SetDTAttrs(");
  ok &= expect_not_contains(source, "\"metadata: SetDTAttrs failed\"");
  ok &= expect_contains(source, "\"decode: zz9k_open failed\"");
  ok &= expect_contains(source, "\"decode: required capabilities missing\"");
  ok &= expect_contains(source, "\"decode: stream service missing\"");
  ok &= expect_contains(source, "\"decode: feed failed\"");
  ok &= expect_contains(source, "DTA_Handle");
  ok &= expect_contains(source, "DTA_SourceType");
  ok &= expect_contains(source, "DTST_FILE");
  ok &= expect_contains(source, "layout->gpl_GInfo->gi_Window");
  ok &= expect_contains(source, "layout->gpl_GInfo->gi_Requester");
  ok &= expect_contains(source, "DTA_Busy, FALSE");
  ok &= expect_contains(source, "DTA_Sync, TRUE");
  ok &= expect_contains(source, "PDTA_SourceMode");
  ok &= expect_contains(source, "PMODE_V43");
  ok &= expect_contains(source, "PDTA_DestMode, PMODE_V43");
  ok &= expect_contains(source, "PDTA_SubClassRendersAll, renders_all");
  ok &= expect_contains(source, "PDTA_SubClassRendersAll, TRUE");
  ok &= expect_contains(source, "PDTA_SubClassRendersAll, FALSE");
  ok &= expect_contains(source, "PDTM_WRITEPIXELARRAY");
  ok &= expect_contains(source, "PBPAFMT_RGB");
  ok &= expect_contains(source,
      "zz9k_picture_render_mode_matches(value, length, \"reference\")");
  ok &= expect_contains(source,
      "zz9k_picture_render_mode_matches(value, length, \"referencenolayout\")");
  ok &= expect_contains(source,
      "zz9k_picture_render_mode_matches(value, length, \"alphareference\")");
  ok &= expect_contains(source,
      "zz9k_picture_render_mode_matches(\n"
      "          value, length, \"alphareferencenolayout\")");
  ok &= expect_contains(source,
      "render_mode == ZZ9K_PICTURE_RENDER_MODE_REFERENCE");
  ok &= expect_contains(source,
      "render_mode == ZZ9K_PICTURE_RENDER_MODE_REFERENCE_NOLAYOUT");
  ok &= expect_contains(source,
      "render_mode == ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE");
  ok &= expect_contains(source,
      "render_mode == ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE_NOLAYOUT");
  ok &= expect_contains(source,
      "zz9k_picture_reference_mode(render_mode)");
  ok &= expect_contains(source,
      "zz9k_picture_alpha_reference_mode(render_mode)");
  ok &= expect_contains(source,
      "zz9k_picture_reference_nolayout_mode(render_mode)");
  ok &= expect_contains(source, "uint8_t *reference_pixels;");
  ok &= expect_contains(source, "uint32_t reference_pixel_bytes;");
  ok &= expect_contains(source, "instance->reference_pixels = pixel_data;");
  ok &= expect_contains(source, "instance->reference_pixel_bytes = pixel_bytes;");
  ok &= expect_contains(source, "FreeMem(reference_pixels, (ULONG)reference_pixel_bytes);");
  ok &= expect_contains(source, "DTA_NominalHoriz");
  ok &= expect_contains(source, "DTA_NominalVert");
  ok &= expect_contains(source, "zz9k_picture_read_jpeg_dimensions");
  ok &= expect_contains(source, "zz9k_picture_read_png_dimensions");
  ok &= expect_contains(source, "zz9k_picture_read_png_metadata");
  ok &= expect_contains(source, "zz9k_picture_read_png_metadata_with_alpha");
  ok &= expect_not_contains(source, "zz9k_picture_skip_bytes(file, 4U)");
  ok &= expect_contains(source, "instance->png_alpha_known");
  ok &= expect_contains(source, "instance->png_has_alpha");
  ok &= expect_contains(
      source, "&source, &codec, &width, &height, &png_has_alpha");
  ok &= expect_contains(source, "zz9k_picture_decode_to_surface");
  ok &= expect_contains(source, "zz9k_picture_feed_stream");
  ok &= expect_contains(source, "zz9k_picture_read_chunk_to_shared");
  ok &= expect_contains(source, "ZZ9K_PICTURE_MAX_SURFACE_BYTES");
  ok &= expect_contains(source, "zz9k_picture_surface_layout");
  ok &= expect_contains(source, "\"decode: before local surface layout\"");
  ok &= expect_contains(source, "\"decode: surface pitch ready\"");
  ok &= expect_contains(source, "\"decode: surface bytes ready\"");
  ok &= expect_contains(source, "zz9k_picture_accumulate_surface_bytes");
  ok &= expect_contains(source, "for (row = 0U; row < height; row++)");
  ok &= expect_contains(source, "\"decode: surface pitch overflow\"");
  ok &= expect_contains(source, "\"decode: surface size overflow\"");
  ok &= expect_contains(source, "\"decode: surface too large\"");
  ok &= expect_contains(source, "\"decode: before output surface alloc\"");
  ok &= expect_contains(source, "\"decode: output surface alloc ok\"");
  ok &= expect_not_contains(
      source,
      "zz9k_surface_layout(instance->width, instance->height");
  ok &= expect_not_contains(source, "0xffffffffUL / *pitch");
  ok &= expect_not_contains(source, "zz9k_picture_decode_source_from_object");
  ok &= expect_contains(source, "zz9k_picture_prepare_hardware");
  ok &= expect_contains(source, "zz9k_picture_enable_hardware_render");
  ok &= expect_contains(source, "zz9k_picture_hardware_screen_ok");
  ok &= expect_not_contains(source, "zz9k_picture_render_is_full_redraw");
  ok &= expect_not_contains(source, "instance->rendered_once");
  ok &= expect_not_contains(source, "instance->hardware_render_count");
  ok &= expect_not_contains(source, "zz9k_picture_render_should_skip_incremental");
  ok &= expect_not_contains(source, "zz9k_picture_render_mode_uses_subclass_attrs");
  ok &= expect_not_contains(source, "zz9k_picture_render_should_skip_border_drag");
  ok &= expect_not_contains(source, "ZZ9K_PICTURE_RENDER_MODE_FILL1SUPER");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_SMALLOFF");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_V43SMALL");
  ok &= expect_not_contains(source, "ZZ9K_PICTURE_RENDER_MODE_SURFACEFILL1SUPER");
  ok &= expect_not_contains(source, "ZZ9K_PICTURE_RENDER_MODE_SCALE1");
  ok &= expect_not_contains(source, "ZZ9K_PICTURE_RENDER_MODE_SCALE2");
  ok &= expect_not_contains(source, "ZZ9K_PICTURE_RENDER_MODE_SCALE4");
  ok &= expect_not_contains(source, "ZZ9K_PICTURE_RENDER_MODE_SCALE8");
  ok &= expect_not_contains(source, "zz9k_picture_render_budget");
  ok &= expect_contains(source, "\"fill1super\"");
  ok &= expect_contains(source, "\"smalloff\"");
  ok &= expect_contains(source, "\"datatype\"");
  ok &= expect_contains(source, "\"auto\"");
  ok &= expect_contains(source, "\"v43small\"");
  ok &= expect_contains(source, "\"surfacefill1super\"");
  ok &= expect_not_contains(source, "\"render: source fill ok\"");
  ok &= expect_not_contains(source, "zz9k_surface_build_fill_desc");
  ok &= expect_contains(source, "\"scale1\"");
  ok &= expect_contains(source, "\"scale1super\"");
  ok &= expect_contains(source, "\"scale2\"");
  ok &= expect_contains(source, "\"scale4\"");
  ok &= expect_contains(source, "\"scale8\"");
  ok &= expect_contains(source, "zz9k_picture_window_content_rect");
  ok &= expect_contains(source, "zz9k_picture_intersect_rects");
  ok &= expect_contains(source, "zz9k_picture_clip_rect_to_framebuffer");
  ok &= expect_contains(source, "render->gpr_GInfo->gi_Window");
  ok &= expect_contains(source, "window->LeftEdge");
  ok &= expect_contains(source, "window->TopEdge");
  ok &= expect_contains(source, "window->BorderLeft");
  ok &= expect_contains(source, "window->BorderTop");
  ok &= expect_contains(source, "window->BorderRight");
  ok &= expect_contains(source, "window->BorderBottom");
  ok &= expect_contains(source, "zz9k_picture_choose_draw_rect_in_area");
  ok &= expect_contains(source, "zz9k_picture_fit_size_to_area");
  ok &= expect_contains(source, "zz9k_picture_muldiv_floor_u32");
  ok &= expect_contains(source, "zz9k_picture_divmod_u32");
  ok &= expect_not_contains(source, "value / divisor");
  ok &= expect_not_contains(source, "value % divisor");
  ok &= expect_not_contains(source, "/ 2U");
  ok &= expect_contains(
      source, "zz9k_picture_render_area(instance, render, &area)");
  ok &= expect_contains(source, "render->gpr_GInfo->gi_Domain.Width");
  ok &= expect_contains(source, "render->gpr_GInfo->gi_Domain.Height");
  ok &= expect_contains(source, "render->gpr_GInfo->gi_Domain.Left");
  ok &= expect_contains(source, "render->gpr_GInfo->gi_Domain.Top");
  ok &= expect_contains(source, "x += (int32_t)window->LeftEdge");
  ok &= expect_contains(source, "y += (int32_t)window->TopEdge");
  ok &= expect_not_contains(source, "zz9k_picture_render_area((const struct Gadget *)object");
  ok &= expect_not_contains(source, "gadget->Width");
  ok &= expect_not_contains(source, "gadget->Height");
  ok &= expect_not_contains(source, "gadget->LeftEdge");
  ok &= expect_not_contains(source, "gadget->TopEdge");
  ok &= expect_contains(source, "render_trace_mask");
  ok &= expect_contains(source, "zz9k_picture_trace_render_once");
  ok &= expect_contains(source, "ZZ9K_PICTURE_USE_LAYER_CLIPS 0");
  ok &= expect_contains(source, "#if ZZ9K_PICTURE_USE_LAYER_CLIPS");
  ok &= expect_contains(source, "visible[0] = area;");
  ok &= expect_contains(source, "visible_count = 1U;");
  ok &= expect_contains(
      source, "\"render: screen info unavailable; superclass\"");
  ok &= expect_contains(
      source, "\"render: screen bounds mismatch; superclass\"");
  ok &= expect_not_contains(source, "screen->BitMap.Depth <= 8");
  ok &= expect_not_contains(source, "gpr_RPort->BitMap->Depth <= 8");
  ok &= expect_contains(source,
                        "(uint32_t)screen->Width != "
                        "instance->framebuffer_width");
  ok &= expect_contains(source,
                        "(uint32_t)screen->Height != "
                        "instance->framebuffer_height");
  ok &= expect_contains(source, "ZZ9K_PICTURE_TRACE_STREAM_CHUNKS 0");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_HARDWARE 1");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_ENV_PATH");
  ok &= expect_contains(source, "\"ENV:ZZ9K_PICTURE_RENDER_MODE\"");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_OFF");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_DECODE");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_SUBCLASS");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_SUPER");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_SCREEN");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_AREA");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_DRAWCOPY");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_DRAWFIT");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_DRAWCENTER");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_DRAWTRACE");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_DRAW");
  ok &= expect_contains(source, "ZZ9K_PICTURE_RENDER_MODE_PROBE");
  ok &= expect_not_contains(source, "ZZ9K_PICTURE_RENDER_MODE_FILL");
  ok &= expect_not_contains(source, "ZZ9K_PICTURE_RENDER_MODE_SCALE");
  ok &= expect_contains(source, "zz9k_picture_render_mode()");
  ok &= expect_contains(source, "zz9k_picture_render_mode_from_env");
  ok &= expect_contains(source, "\"render: mode off; superclass\"");
  ok &= expect_contains(source, "\"render: mode decode; superclass\"");
  ok &= expect_contains(source, "\"render: mode subclass complete\"");
  ok &= expect_contains(source, "\"render: mode super; superclass\"");
  ok &= expect_contains(source, "\"render: mode screen complete\"");
  ok &= expect_contains(source, "\"render: mode area complete\"");
  ok &= expect_contains(source, "\"render: mode drawcopy complete\"");
  ok &= expect_contains(source, "\"render: mode drawfit complete\"");
  ok &= expect_contains(source, "\"render: mode drawcenter complete\"");
  ok &= expect_contains(source, "\"render: mode drawtrace complete\"");
  ok &= expect_contains(source, "\"render: mode draw complete\"");
  ok &= expect_contains(
      source,
      "render_mode == ZZ9K_PICTURE_RENDER_MODE_DRAWCOPY ||\n"
      "      render_mode == ZZ9K_PICTURE_RENDER_MODE_DRAW");
  ok &= expect_contains(source, "\"render: mode probe complete\"");
  ok &= expect_not_contains(source, "\"render: mode fill complete\"");
  ok &= expect_contains(source, "return 1;");
  ok &= expect_contains(
      source, "\"layout: hardware decode only; using placeholder\"");
  ok &= expect_contains(source, "\"layout: subclass render enabled\"");
  ok &= expect_contains(source, "\"decode\"");
  ok &= expect_contains(source, "return ZZ9K_PICTURE_RENDER_MODE_OFF;");
  ok &= expect_contains(source, "return DoSuperMethodA(cl, object, (Msg)render);");
  ok &= expect_not_contains(
      source,
      "(void)cl;\n  (void)object;\n  (void)render;\n"
      "  zz9k_picture_trace(\"render: hardware render disabled\");\n"
      "  return 1;");
  ok &= expect_contains(source, "\"metadata: decode deferred\"");
  ok &= expect_not_contains(source, "\"render: lazy decode begin\"");
  ok &= expect_not_contains(source, "\"render: lazy decode failed\"");
  ok &= expect_contains(source, "\"decode: before zz9k_open\"");
  ok &= expect_contains(source, "\"decode: before framebuffer map\"");
  ok &= expect_contains(source, "\"decode: before session feed\"");
  ok &= expect_contains(source, "\"stream: before fill staging\"");
  ok &= expect_contains(source, "\"stream: fill staging ok\"");
  ok &= expect_contains(source, "#if ZZ9K_PICTURE_TRACE_STREAM_CHUNKS");
  ok &= expect_contains(source, "\"stream: before file read\"");
  ok &= expect_contains(source, "\"stream: before shared copy\"");
  ok &= expect_contains(source, "\"stream: staging pointer unavailable\"");
  ok &= expect_contains(source, "zz9k_picture_shared_copy_to_bytes");
  ok &= expect_contains(source, "\"stream: before shared byte copy\"");
  ok &= expect_contains(source, "\"stream: shared byte copy ok\"");
  ok &= expect_contains(source, "volatile uint8_t *dst;");
  ok &= expect_contains(source, "dst[i] = src[i];");
  ok &= expect_not_contains(
      source, "return zz9k_shared_copy_to(buffer, offset, src, length);");
  ok &= expect_contains(source, "\"stream: before feed desc\"");
  ok &= expect_contains(source, "\"stream: feed desc ok\"");
  ok &= expect_contains(source, "\"stream: before session feed\"");
  ok &= expect_contains(source, "\"stream: session feed ok\"");
  ok &= expect_contains(source, "\"layout: hardware decode begin\"");
  ok &= expect_contains(source, "\"layout: hardware decode ready\"");
  ok &= expect_contains(source, "\"layout: hardware unavailable; using placeholder\"");
  ok &= expect_not_contains(source, "SetIoErr(DTERROR_NOT_AVAILABLE);");
  ok &= expect_contains(source, "zz9k_open(&instance->ctx)");
  ok &= expect_contains(source, "zz9k_map_framebuffer_surface");
  ok &= expect_contains(source, "zz9k_surface_is_native_rtg_format");
  ok &= expect_contains(source, "ZZ9K_SURFACE_FLAG_FRAMEBUFFER");
  ok &= expect_contains(source, "zz9k_alloc_shared");
  ok &= expect_contains(source, "zz9k_alloc_surface_ex");
  ok &= expect_contains(source, "zz9k_image_session_begin");
  ok &= expect_contains(source, "zz9k_image_session_feed");
  ok &= expect_contains(source, "zz9k_image_session_close");
  ok &= expect_contains(source, "zz9k_shared_move");
  ok &= expect_contains(source, "source_ready = 1");
  ok &= expect_not_contains(
      source, "zz9k_image_window_choose_draw_rect_in_area(");
  ok &= expect_contains(source, "zz9k_image_window_visible_clips_for_window");
  ok &= expect_contains(source, "zz9k_image_window_build_damage_clips");
  ok &= expect_not_contains(source, "zz9k_image_window_build_framebuffer_fill_desc");
  ok &= expect_contains(source, "zz9k_picture_legacy_framebuffer_mode");
  ok &= expect_contains(
      source,
      "return ZZ9K_PICTURE_RENDER_MODE_DATATYPE;");
  ok &= expect_not_contains(source, "zz9k_surface_color_rgb(");
  /* GM_RENDER owns a locked layer. It may do geometry and superclass work,
   * but it must never enter either blocking SDK mailbox path. */
  ok &= expect_region_not_contains(
      source,
      "static ULONG zz9k_picture_render(",
      "static void zz9k_picture_instance_dispose(",
      "zz9k_fill_surface(");
  ok &= expect_region_not_contains(
      source,
      "static ULONG zz9k_picture_render(",
      "static void zz9k_picture_instance_dispose(",
      "zz9k_image_window_scale_sliced(");
  ok &= expect_contains(source, "\"render: source not ready; superclass\"");
  ok &= expect_contains(source, "\"render: screen rejected; superclass\"");
  ok &= expect_contains(source, "\"render: hardware render begin\"");
  ok &= expect_contains(source, "\"render: area ready\"");
  ok &= expect_contains(source, "\"render: draw rect ready\"");
  ok &= expect_contains(source, "\"render: visible clips ready\"");
  ok &= expect_contains(source, "\"render: damage clips ready\"");
  ok &= expect_not_contains(source, "\"render: before fill\"");
  ok &= expect_not_contains(source, "\"render: fill ok\"");
  ok &= expect_not_contains(source, "\"render: before scale\"");
  ok &= expect_not_contains(source, "\"render: scale ok\"");
  ok &= expect_contains(source, "\"render: render area unavailable; superclass\"");
  ok &= expect_contains(source, "\"render: draw rect unavailable; superclass\"");
  ok &= expect_contains(source, "\"render: visible clips unavailable; superclass\"");
  ok &= expect_contains(source, "\"render: damage clips unavailable; superclass\"");
  ok &= expect_not_contains(source, "\"render: fill descriptor failed; superclass\"");
  ok &= expect_not_contains(source, "\"render: fill failed; superclass\"");
  ok &= expect_not_contains(source, "\"render: scale failed; superclass\"");
  ok &= expect_not_contains(source, "\"render: hardware render complete\"");
  ok &= expect_contains(source, "gpr_GInfo->gi_Window");
  ok &= expect_contains(source, "ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS");
  ok &= expect_not_contains(source, "zz9k-view");
  ok &= expect_not_contains(source, "system(");

  ok &= expect_contains(helper_header,
                        "zz9k_image_window_visible_clips_for_window");
  ok &= expect_contains(helper_source,
                        "zz9k_image_window_visible_clips_for_window");

  free(helper_source);
  free(helper_header);
  free(source);
  return ok ? 0 : 1;
}
