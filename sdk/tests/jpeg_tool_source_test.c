/*
 * Source guard for zz9k-jpeg allocator diagnostics.
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

static char *read_image_window_helper(const char *path)
{
  char *source;
  char *header_path;
  char *header;
  char *combined;
  size_t path_len;
  size_t header_len;
  size_t source_len;

  source = read_file(path);
  if (!source) {
    return 0;
  }

  path_len = strlen(path);
  if (path_len < 2U || strcmp(path + path_len - 2U, ".c") != 0) {
    return source;
  }

  header_path = (char *)malloc(path_len + 1U);
  if (!header_path) {
    return source;
  }
  memcpy(header_path, path, path_len + 1U);
  header_path[path_len - 1U] = 'h';

  header = read_file(header_path);
  free(header_path);
  if (!header) {
    return source;
  }

  header_len = strlen(header);
  source_len = strlen(source);
  combined = (char *)malloc(header_len + 1U + source_len + 1U);
  if (!combined) {
    free(header);
    return source;
  }

  memcpy(combined, header, header_len);
  combined[header_len] = '\n';
  memcpy(combined + header_len + 1U, source, source_len + 1U);
  free(header);
  free(source);
  return combined;
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

int main(int argc, char **argv)
{
  char *source;
  char *helper;
  int ok;

  if (argc != 3) {
    printf("usage: %s <tools/zz9k-jpeg.c> <tools/zz9k-image-window.c>\n",
           argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }
  helper = read_image_window_helper(argv[2]);
  if (!helper) {
    printf("failed to read %s\n", argv[2]);
    free(source);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "print_diag(ctx, \"before input alloc\"");
  ok &= expect_contains(source, "print_diag(ctx, \"after input alloc failure\"");
  ok &= expect_contains(source, "JPEG input bytes:");
  ok &= expect_contains(source, "Shared buffers used:");
  ok &= expect_contains(source, "Surfaces used:");
  ok &= expect_contains(source, "Invalid alloc slots:");
  ok &= expect_contains(source, "Largest free block:");
  ok &= expect_contains(source, "Last status:        %s (%lu)");
  ok &= expect_contains(source, "zz9k_status_name((int)diag.last_status)");
  ok &= expect_contains(source, "zz9k_image_window_config_init");
  ok &= expect_contains(source, "zz9k_image_window_open");
  ok &= expect_contains(source, "zz9k_image_window_poll");
  ok &= expect_contains(source, "zz9k_image_window_close");
  ok &= expect_contains(source, "--nearest");
  ok &= expect_contains(source, "--bilinear");
  ok &= expect_contains(source, "argv[i][0] == '-'");
  ok &= expect_not_contains(source, "--shared-decode");
  ok &= expect_not_contains(source, "--top-pad");
  ok &= expect_not_contains(source, "shared_decode_surface");
  ok &= expect_not_contains(source, "top_pad");
  ok &= expect_contains(source, "zz9k-image-window.h");
  ok &= expect_contains(source, "#include \"zz9k-jpeg-view.h\"");
  ok &= expect_contains(source, "zz9k_image_window_visible_clips");
  ok &= expect_contains(source, "--view");
  ok &= expect_contains(source, "showing until close");
  ok &= expect_contains(source, "zz9k_jpeg_framebuffer_restore_visible");
  ok &= expect_contains(source, "zz9k_image_window_scale_sliced");
  ok &= expect_contains(source, "zz9k_jpeg_scale_framebuffer_sliced");
  ok &= expect_contains(source, "zz9k_image_stream_required_service_flags");
  ok &= expect_contains(source, "zz9k_image_scale_filter_supported_by_service");
  ok &= expect_contains(source, "zz9k_image_service_supports_clipped_scale");
  ok &= expect_contains(source, "#include \"zz9k/surface.h\"");
  ok &= expect_contains(source, "zz9k_surface_native_rtg_format()");
  ok &= expect_contains(source,
                        "zz9k_surface_is_native_rtg_format(framebuffer.format)");
  ok &= expect_contains(source, "zz9k_surface_bytes_per_pixel(output_format)");
  ok &= expect_contains(source, "zz9k_surface_layout(");
  ok &= expect_contains(source,
                        "zz9k_surface_min_pitch(width, output_format, &row_bytes)");
  ok &= expect_contains(source,
                        "zz9k_jpeg_choose_tile_rows(jpeg_input->width,");
  ok &= expect_contains(source, "zz9k_jpeg_decode_viewer_image");
  ok &= expect_contains(
    source, "output_format = zz9k_picture_viewer_decode_format();");
  ok &= expect_contains(source, "#define ZZ9K_JPEG_STAGING_BYTES (32UL * 1024UL)");
  ok &= expect_contains(source, "ZZ9K_ALLOC_HOST_WINDOW, &staging");
  ok &= expect_contains(source, "ZZ9KPictureViewerImage");
  ok &= expect_contains(source, "ZZ9K_PICTURE_VIEWER_CODEC_JPEG");
  ok &= expect_contains(source, "output_bpp");
  ok &= expect_not_contains(source, "zz9k_jpeg_output_bytes");
  ok &= expect_not_contains(source, "uint32_t bytes_per_pixel");
  ok &= expect_not_contains(source, "ZZ9K_SURFACE_FORMAT_BGRA8888");
  ok &= expect_not_contains(source, "WA_GimmeZeroZero");
  ok &= expect_not_contains(source, "layer->bounds");
  ok &= expect_contains(helper, "zz9k_image_window_drain_messages");
  ok &= expect_contains(helper, "ModifyIDCMP(ui->window, 0L)");
  ok &= expect_contains(helper, "CloseWindow(ui->window)");
  ok &= expect_contains(helper, "WA_SizeGadget, FALSE");
  ok &= expect_contains(helper, "WA_SizeGadget, TRUE");
  ok &= expect_contains(helper, "WA_SizeBRight, TRUE");
  ok &= expect_contains(helper, "WA_SizeBBottom, TRUE");
  ok &= expect_contains(helper, "WA_SmartRefresh, TRUE");
  ok &= expect_contains(helper, "WLayer->ClipRect");
  ok &= expect_contains(helper, "clip_rect->obscured");
  ok &= expect_contains(helper, "typedef struct ZZ9KImageWindowEvent");
  ok &= expect_contains(helper, "zz9k_image_window_poll_event");
  ok &= expect_contains(helper, "IDCMP_VANILLAKEY");
  ok &= expect_contains(helper, "IDCMP_RAWKEY");
  ok &= expect_contains(helper, "SetWindowTitles");
  ok &= expect_contains(helper, "zz9k_image_window_set_title");
  ok &= expect_contains(helper,
                        "idcmp = (ULONG)(IDCMP_CLOSEWINDOW | "
                        "IDCMP_REFRESHWINDOW |");
  ok &= expect_contains(helper, "IDCMP_VANILLAKEY | IDCMP_RAWKEY);");
  ok &= expect_contains(helper, "if (config->resizable) {");
  ok &= expect_contains(helper, "idcmp |= IDCMP_NEWSIZE;");
  ok &= expect_contains(helper, "if (klass == IDCMP_REFRESHWINDOW) {");
  ok &= expect_contains(helper, "event->changed = 1;");
  ok &= expect_contains(helper,
                        "} else if (ui->resizable && klass == IDCMP_NEWSIZE) {");
  ok &= expect_contains(helper, "WA_IDCMP, idcmp");
  ok &= expect_not_contains(helper, "WA_GimmeZeroZero");
  ok &= expect_not_contains(helper, "layer->bounds");

  free(helper);
  free(source);
  return ok ? 0 : 1;
}
