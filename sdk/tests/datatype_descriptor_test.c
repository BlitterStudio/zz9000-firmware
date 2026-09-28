/*
 * Guards the packaged ZZ9000 picture DataTypes descriptors.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct byte_buffer {
  unsigned char *data;
  size_t length;
};

static char *read_text_file(const char *path)
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

static struct byte_buffer read_binary_file(const char *path)
{
  struct byte_buffer buffer;
  FILE *file;
  long length;

  buffer.data = 0;
  buffer.length = 0;
  file = fopen(path, "rb");
  if (!file) {
    return buffer;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return buffer;
  }
  length = ftell(file);
  if (length <= 0) {
    fclose(file);
    return buffer;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return buffer;
  }

  buffer.data = (unsigned char *)malloc((size_t)length);
  if (!buffer.data) {
    fclose(file);
    return buffer;
  }
  if (fread(buffer.data, 1U, (size_t)length, file) != (size_t)length) {
    free(buffer.data);
    buffer.data = 0;
    fclose(file);
    return buffer;
  }

  buffer.length = (size_t)length;
  fclose(file);
  return buffer;
}

static int base64_value(int ch)
{
  if (ch >= 'A' && ch <= 'Z') {
    return ch - 'A';
  }
  if (ch >= 'a' && ch <= 'z') {
    return 26 + ch - 'a';
  }
  if (ch >= '0' && ch <= '9') {
    return 52 + ch - '0';
  }
  if (ch == '+') {
    return 62;
  }
  if (ch == '/') {
    return 63;
  }
  return -1;
}

static struct byte_buffer decode_base64(const char *text)
{
  struct byte_buffer result;
  size_t text_len;
  size_t capacity;
  int quartet[4];
  int count;
  int saw_pad;
  size_t i;

  result.data = 0;
  result.length = 0;
  text_len = strlen(text);
  capacity = (text_len / 4U + 1U) * 3U;
  result.data = (unsigned char *)malloc(capacity);
  if (!result.data) {
    return result;
  }

  count = 0;
  saw_pad = 0;
  for (i = 0; i < text_len; ++i) {
    int ch;
    int value;

    ch = (unsigned char)text[i];
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      continue;
    }
    if (ch == '=') {
      value = -2;
      saw_pad = 1;
    } else {
      value = base64_value(ch);
      if (value < 0 || saw_pad) {
        free(result.data);
        result.data = 0;
        result.length = 0;
        return result;
      }
    }

    quartet[count++] = value;
    if (count == 4) {
      if (quartet[0] < 0 || quartet[1] < 0 ||
          (quartet[2] < 0 && quartet[2] != -2) ||
          (quartet[3] < 0 && quartet[3] != -2)) {
        free(result.data);
        result.data = 0;
        result.length = 0;
        return result;
      }
      result.data[result.length++] =
        (unsigned char)((quartet[0] << 2) | (quartet[1] >> 4));
      if (quartet[2] != -2) {
        result.data[result.length++] =
          (unsigned char)(((quartet[1] & 15) << 4) | (quartet[2] >> 2));
      }
      if (quartet[3] != -2) {
        result.data[result.length++] =
          (unsigned char)(((quartet[2] & 3) << 6) | quartet[3]);
      }
      count = 0;
    }
  }

  if (count != 0) {
    free(result.data);
    result.data = 0;
    result.length = 0;
  }
  return result;
}

static unsigned long read_be32(const unsigned char *data)
{
  return ((unsigned long)data[0] << 24) |
         ((unsigned long)data[1] << 16) |
         ((unsigned long)data[2] << 8) |
         (unsigned long)data[3];
}

static unsigned int read_be16(const unsigned char *data)
{
  return ((unsigned int)data[0] << 8) | (unsigned int)data[1];
}

static int expect_contains(const char *label, const char *text,
                           const char *needle)
{
  if (strstr(text, needle)) {
    return 1;
  }

  printf("%s: missing %s\n", label, needle);
  return 0;
}

static int expect_not_contains(const char *label, const char *text,
                               const char *needle)
{
  if (!strstr(text, needle)) {
    return 1;
  }

  printf("%s: unexpected %s\n", label, needle);
  return 0;
}

static int string_at_matches(const unsigned char *data, size_t length,
                             unsigned long offset, const char *expected)
{
  size_t expected_len;

  expected_len = strlen(expected);
  if (offset > length || expected_len + 1U > length - offset) {
    return 0;
  }
  return memcmp(data + offset, expected, expected_len + 1U) == 0;
}

static int validate_descriptor(const char *label, const unsigned char *data,
                               size_t length, const char *name,
                               const char *dth_name, const char *base_name,
                               const char *group,
                               const unsigned char *ident,
                               const unsigned char *mask, size_t mask_len)
{
  const unsigned char *name_data;
  const unsigned char *dthd_data;
  size_t name_len;
  size_t dthd_len;
  size_t offset;
  unsigned long dth_name_offset;
  unsigned long base_offset;
  unsigned long pattern_offset;
  unsigned long mask_offset;
  unsigned int stored_mask_len;
  unsigned int flags;
  unsigned int priority;
  size_t i;
  int ok;

  ok = 1;
  if (length < 12U || memcmp(data, "FORM", 4U) != 0 ||
      read_be32(data + 4) != length - 8U || memcmp(data + 8, "DTYP", 4U) != 0) {
    printf("%s: not a FORM DTYP descriptor\n", label);
    return 0;
  }

  name_data = 0;
  dthd_data = 0;
  name_len = 0;
  dthd_len = 0;
  offset = 12U;
  while (offset + 8U <= length) {
    const unsigned char *chunk_data;
    unsigned long chunk_size;
    size_t chunk_total;

    chunk_size = read_be32(data + offset + 4U);
    chunk_data = data + offset + 8U;
    if (chunk_size > length - offset - 8U) {
      printf("%s: chunk extends beyond descriptor\n", label);
      return 0;
    }

    if (memcmp(data + offset, "NAME", 4U) == 0) {
      name_data = chunk_data;
      name_len = (size_t)chunk_size;
    } else if (memcmp(data + offset, "DTHD", 4U) == 0) {
      dthd_data = chunk_data;
      dthd_len = (size_t)chunk_size;
    }

    chunk_total = 8U + (size_t)chunk_size + ((chunk_size & 1U) ? 1U : 0U);
    offset += chunk_total;
  }

  if (!name_data || name_len != strlen(name) ||
      memcmp(name_data, name, name_len) != 0) {
    printf("%s: NAME chunk mismatch\n", label);
    ok = 0;
  }
  if (!dthd_data || dthd_len < 32U) {
    printf("%s: missing DTHD chunk\n", label);
    return 0;
  }

  dth_name_offset = read_be32(dthd_data);
  base_offset = read_be32(dthd_data + 4U);
  pattern_offset = read_be32(dthd_data + 8U);
  mask_offset = read_be32(dthd_data + 12U);
  stored_mask_len = read_be16(dthd_data + 24U);
  flags = read_be16(dthd_data + 28U);
  priority = read_be16(dthd_data + 30U);

  if (!string_at_matches(dthd_data, dthd_len, dth_name_offset, dth_name)) {
    printf("%s: dth_Name mismatch\n", label);
    ok = 0;
  }
  if (!string_at_matches(dthd_data, dthd_len, base_offset, base_name)) {
    printf("%s: dth_BaseName mismatch\n", label);
    ok = 0;
  }
  if (!string_at_matches(dthd_data, dthd_len, pattern_offset, "#?")) {
    printf("%s: dth_Pattern mismatch\n", label);
    ok = 0;
  }
  if (memcmp(dthd_data + 16U, group, 4U) != 0 ||
      memcmp(dthd_data + 20U, ident, 4U) != 0) {
    printf("%s: group/id mismatch\n", label);
    ok = 0;
  }
  if (stored_mask_len != mask_len ||
      mask_offset + mask_len * 2U > dthd_len) {
    printf("%s: mask bounds mismatch\n", label);
    ok = 0;
  } else {
    for (i = 0; i < mask_len; ++i) {
      if (read_be16(dthd_data + mask_offset + i * 2U) != mask[i]) {
        printf("%s: mask byte %lu mismatch\n", label, (unsigned long)i);
        ok = 0;
      }
    }
  }
  if (flags != 0U || priority != 10U) {
    printf("%s: expected binary flags 0 and priority 10\n", label);
    ok = 0;
  }

  return ok;
}

static int check_source(const char *label, const char *source,
                        const char *file_name, const char *dth_name,
                        const char *id)
{
  int ok;
  char expected[128];

  ok = 1;
  ok &= expect_contains(label, source,
                        "Copyright (C) 2024-2026, "
                        "Dimitris Panokostas / BlitterStudio");
  ok &= expect_contains(label, source,
                        "SPDX-License-Identifier: GPL-3.0-or-later");
  snprintf(expected, sizeof(expected), "FileName=Storage/DataTypes/%s",
           file_name);
  ok &= expect_contains(label, source, expected);
  ok &= expect_contains(label, source, "Version=42.5");
  snprintf(expected, sizeof(expected), "DTName=%s,zz9k-picture", dth_name);
  ok &= expect_contains(label, source, expected);
  snprintf(expected, sizeof(expected), "ID=pict,%s", id);
  ok &= expect_contains(label, source, expected);
  ok &= expect_contains(label, source, "Pattern=#?");
  ok &= expect_contains(label, source, "Flags=Binary,n,10");
  ok &= expect_contains(label, source, "Install=inactive");
  ok &= expect_not_contains(label, source,
                            "Classes/DataTypes/picture.datatype");
  return ok;
}

static int validate_icon(const char *label, const struct byte_buffer *icon)
{
  if (!icon || !icon->data || icon->length < 2U) {
    printf("%s: missing icon\n", label);
    return 0;
  }
  if (icon->data[0] != 0xe3U || icon->data[1] != 0x10U) {
    printf("%s: unexpected Workbench icon magic\n", label);
    return 0;
  }
  if (icon->length < 1024U) {
    printf("%s: icon is unexpectedly small\n", label);
    return 0;
  }
  return 1;
}

int main(int argc, char **argv)
{
  static const unsigned char jpeg_id[] = {'j', 'p', 'e', 'g'};
  static const unsigned char png_id[] = {'p', 'n', 'g', '\0'};
  static const unsigned char jpeg_mask[] = {0xff, 0xd8, 0xff};
  static const unsigned char png_mask[] = {0x89, 0x50, 0x4e,
                                           0x47, 0x0d, 0x0a};
  char *jpeg_b64_text;
  char *png_b64_text;
  char *jpeg_source;
  char *png_source;
  struct byte_buffer jpeg_blob;
  struct byte_buffer png_blob;
  struct byte_buffer jpeg_icon;
  struct byte_buffer png_icon;
  int ok;

  if (argc != 7) {
    printf("usage: %s <jpeg.b64> <png.b64> <jpeg.dtid> <png.dtid> "
           "<jpeg.info> <png.info>\n",
           argv[0]);
    return 2;
  }

  jpeg_b64_text = read_text_file(argv[1]);
  png_b64_text = read_text_file(argv[2]);
  jpeg_source = read_text_file(argv[3]);
  png_source = read_text_file(argv[4]);
  jpeg_icon = read_binary_file(argv[5]);
  png_icon = read_binary_file(argv[6]);
  if (!jpeg_b64_text || !png_b64_text || !jpeg_source || !png_source) {
    printf("failed to read descriptor source files\n");
    free(jpeg_b64_text);
    free(png_b64_text);
    free(jpeg_source);
    free(png_source);
    free(jpeg_icon.data);
    free(png_icon.data);
    return 2;
  }

  jpeg_blob = decode_base64(jpeg_b64_text);
  png_blob = decode_base64(png_b64_text);
  if (!jpeg_blob.data || !png_blob.data) {
    printf("failed to decode descriptor blobs\n");
    free(jpeg_b64_text);
    free(png_b64_text);
    free(jpeg_source);
    free(png_source);
    free(jpeg_icon.data);
    free(png_icon.data);
    free(jpeg_blob.data);
    free(png_blob.data);
    return 1;
  }

  ok = 1;
  ok &= validate_descriptor("ZZ9000-JPEG", jpeg_blob.data, jpeg_blob.length,
                            "ZZ9000-JPEG", "ZZ9000-JPEG", "zz9k-picture",
                            "pict", jpeg_id, jpeg_mask, sizeof(jpeg_mask));
  ok &= validate_descriptor("ZZ9000-PNG", png_blob.data, png_blob.length,
                            "ZZ9000-PNG", "ZZ9000-PNG", "zz9k-picture",
                            "pict", png_id, png_mask, sizeof(png_mask));
  ok &= check_source("ZZ9000-JPEG source", jpeg_source, "ZZ9000-JPEG",
                     "ZZ9000-JPEG", "jpeg");
  ok &= check_source("ZZ9000-PNG source", png_source, "ZZ9000-PNG",
                     "ZZ9000-PNG", "png<nul>");
  ok &= validate_icon("ZZ9000-JPEG.info", &jpeg_icon);
  ok &= validate_icon("ZZ9000-PNG.info", &png_icon);

  free(jpeg_b64_text);
  free(png_b64_text);
  free(jpeg_source);
  free(png_source);
  free(jpeg_icon.data);
  free(png_icon.data);
  free(jpeg_blob.data);
  free(png_blob.data);
  return ok ? 0 : 1;
}
