/*
 * Guards generated ZZ9000 picture DataTypes descriptors.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define make_directory(path) _mkdir(path)
#define remove_directory(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_directory(path) mkdir(path, 0700)
#define remove_directory(path) rmdir(path)
#endif

struct byte_buffer { unsigned char *data; size_t length; };

static struct byte_buffer read_binary_file(const char *path)
{
  FILE *file;
  long length;
  struct byte_buffer result = {0, 0};
  file = fopen(path, "rb");
  if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0 ||
      fseek(file, 0, SEEK_SET)) {
    if (file) fclose(file);
    return result;
  }
  result.data = (unsigned char *)malloc((size_t)length);
  if (!result.data || fread(result.data, 1U, (size_t)length, file) !=
                          (size_t)length) {
    free(result.data);
    result.data = 0;
  } else {
    result.length = (size_t)length;
  }
  fclose(file);
  return result;
}

static unsigned long read_be32(const unsigned char *data)
{
  return ((unsigned long)data[0] << 24) | ((unsigned long)data[1] << 16) |
         ((unsigned long)data[2] << 8) | (unsigned long)data[3];
}

static unsigned int read_be16(const unsigned char *data)
{
  return ((unsigned int)data[0] << 8) | (unsigned int)data[1];
}

static int string_matches(const unsigned char *data, size_t length,
                          unsigned long offset, const char *expected)
{
  size_t expected_length = strlen(expected);
  return offset <= length && expected_length + 1U <= length - offset &&
         !memcmp(data + offset, expected, expected_length + 1U);
}

static int validate_descriptor(const char *label, const struct byte_buffer *blob,
                               const char *name, const char *identifier,
                               const char *group, const char *base_name,
                               const int *recognition,
                               size_t recognition_length, size_t expected_length)
{
  const unsigned char *data = blob->data;
  const unsigned char *name_data = 0;
  const unsigned char *dthd = 0;
  size_t name_length = 0, dthd_length = 0, offset = 12U, i;

  if (!data || blob->length != expected_length || blob->length < 12U ||
      memcmp(data, "FORM", 4U) || read_be32(data + 4U) != blob->length - 8U ||
      memcmp(data + 8U, "DTYP", 4U)) goto invalid;
  while (offset < blob->length) {
    unsigned long chunk_length;
    size_t chunk_total;
    if (offset + 8U > blob->length) goto invalid;
    chunk_length = read_be32(data + offset + 4U);
    if (chunk_length > blob->length - offset - 8U) goto invalid;
    chunk_total = 8U + (size_t)chunk_length + (chunk_length & 1U);
    if (chunk_total > blob->length - offset) goto invalid;
    if (!memcmp(data + offset, "NAME", 4U)) {
      name_data = data + offset + 8U;
      name_length = (size_t)chunk_length;
    } else if (!memcmp(data + offset, "DTHD", 4U)) {
      dthd = data + offset + 8U;
      dthd_length = (size_t)chunk_length;
    }
    offset += chunk_total;
  }
  if (offset != blob->length || !name_data || name_length != strlen(name) ||
      memcmp(name_data, name, name_length) || !dthd ||
      dthd_length < 32U + recognition_length * 2U ||
      read_be32(dthd) != 32U + recognition_length * 2U ||
      read_be32(dthd + 4U) != 33U + recognition_length * 2U + strlen(name) ||
      read_be32(dthd + 8U) != 34U + recognition_length * 2U + strlen(name) +
                                    strlen(base_name) ||
      read_be32(dthd + 12U) != 32U ||
      read_be16(dthd + 24U) != recognition_length ||
      read_be16(dthd + 28U) != 0U || read_be16(dthd + 30U) != 10U ||
      memcmp(dthd + 16U, group, 4U) || memcmp(dthd + 20U, identifier, 4U) ||
      !string_matches(dthd, dthd_length, read_be32(dthd), name) ||
      !string_matches(dthd, dthd_length, read_be32(dthd + 4U), base_name) ||
      !string_matches(dthd, dthd_length, read_be32(dthd + 8U), "#?")) goto invalid;
  for (i = 0; i < recognition_length; ++i) {
    unsigned int expected = recognition[i] < 0 ? 0xffffU : (unsigned int)recognition[i];
    if (read_be16(dthd + 32U + i * 2U) != expected) goto invalid;
  }
  return 1;
invalid:
  printf("%s: invalid generated IFF descriptor\n", label);
  return 0;
}

static int validate_icon(const char *label, const struct byte_buffer *icon)
{
  if (!icon->data || icon->length < 1024U || icon->data[0] != 0xe3U ||
      icon->data[1] != 0x10U) {
    printf("%s: unexpected Workbench icon\n", label);
    return 0;
  }
  return 1;
}

static int run_generator(const char *python, const char *generator,
                         const char *source, const char *output,
                         const char *code_dir)
{
  char command[4096], code_argument[1024] = "";
  int length;
  if (code_dir &&
      (size_t)snprintf(code_argument, sizeof(code_argument),
                       " --code-dir \"%s\"", code_dir) >= sizeof(code_argument))
    return 0;
#ifdef _WIN32
  length = snprintf(command, sizeof(command),
      "\"\"%s\" \"%s\" --source-dir \"%s\" --output-dir \"%s\"%s\"",
      python, generator, source, output, code_argument);
#else
  length = snprintf(command, sizeof(command),
      "\"%s\" \"%s\" --source-dir \"%s\" --output-dir \"%s\"%s",
      python, generator, source, output, code_argument);
#endif
  return length >= 0 && (size_t)length < sizeof(command) && system(command) == 0;
}

/* Stand-in recognition executable: the generator only requires an AmigaDOS
 * hunk header and must embed the bytes verbatim. */
static const unsigned char placeholder_code[] = {
    0x00, 0x00, 0x03, 0xf3, 0x00, 0x00, 0x00, 0x00, 0x4e, 0x75, 0x4e, 0x71};

static int write_file(const char *path, const void *data, size_t length)
{
  FILE *file = fopen(path, "wb");
  int ok = file && fwrite(data, 1U, length, file) == length;
  if (file && fclose(file) != 0) ok = 0;
  return ok;
}

static int make_clean_directory(const char *path)
{
  if (make_directory(path) == 0 || errno == EEXIST) return 1;
  printf("failed to create %s\n", path);
  return 0;
}

static int check_malformed_metadata(const char *python, const char *generator)
{
  const char *source = "datatype_descriptor_test_bad_source";
  const char *output = "datatype_descriptor_test_bad_output";
  static const struct { const char *destination; const char *recognition; } cases[] = {
      {"Classes/DataTypes/bad", "00"},
      {"Storage/DataTypes/bad", "-1"},
      {"Storage/DataTypes/bad", "+1"}};
  char path[256], output_path[256];
  size_t index;
  int ok = 1;

  if (!make_clean_directory(source) || !make_clean_directory(output)) return 0;
  snprintf(path, sizeof(path), "%s/bad.dtid", source);
  snprintf(output_path, sizeof(output_path), "%s/bad", output);
  for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    fprintf(file, "FileName=%s\nVersion=42.5\n"
            "DTName=bad,zz9k-picture\nID=pict,bad<nul>\nRecog=%s\n"
            "Pattern=#?\nFlags=Binary,n,10\nInstall=inactive\n",
            cases[index].destination, cases[index].recognition);
    fclose(file);
    if (run_generator(python, generator, source, output, 0)) {
      printf("generator accepted malformed metadata case %lu\n",
             (unsigned long)index);
      ok = 0;
    }
    remove(output_path);
  }
  remove(path);
  remove_directory(source);
  remove_directory(output);
  return ok;
}

static int recognition_matches(const struct byte_buffer *blob,
                               const unsigned char *candidate, size_t length)
{
  size_t offset = 12U;
  const unsigned char *dthd = 0;
  size_t dthd_length = 0;
  unsigned long mask_offset;
  unsigned int mask_length;
  size_t index;

  while (offset + 8U <= blob->length) {
    unsigned long chunk_length = read_be32(blob->data + offset + 4U);
    size_t chunk_total = 8U + (size_t)chunk_length + (chunk_length & 1U);
    if (chunk_length > blob->length - offset - 8U ||
        chunk_total > blob->length - offset) return 0;
    if (!memcmp(blob->data + offset, "DTHD", 4U)) {
      dthd = blob->data + offset + 8U;
      dthd_length = (size_t)chunk_length;
      break;
    }
    offset += chunk_total;
  }
  if (!dthd || dthd_length < 32U) return 0;
  mask_offset = read_be32(dthd + 12U);
  mask_length = read_be16(dthd + 24U);
  if (mask_length > length || mask_offset > dthd_length ||
      (size_t)mask_length * 2U > dthd_length - mask_offset) return 0;
  for (index = 0; index < mask_length; ++index) {
    unsigned int mask = read_be16(dthd + mask_offset + index * 2U);
    if (mask != 0xffffU && mask != candidate[index]) return 0;
  }
  return 1;
}

static int check_webp_wildcards(const char *python, const char *generator)
{
  const char *source = "datatype_descriptor_test_webp_source";
  const char *output = "datatype_descriptor_test_webp_output";
  static const unsigned char webp_a[] =
      {'R', 'I', 'F', 'F', 0x10, 0x00, 0x00, 0x00, 'W', 'E', 'B', 'P'};
  static const unsigned char webp_b[] =
      {'R', 'I', 'F', 'F', 0x7f, 0x42, 0x01, 0x00, 'W', 'E', 'B', 'P'};
  static const unsigned char wave[] =
      {'R', 'I', 'F', 'F', 0x10, 0x00, 0x00, 0x00, 'W', 'A', 'V', 'E'};
  static const unsigned char avi[] =
      {'R', 'I', 'F', 'F', 0x10, 0x00, 0x00, 0x00, 'A', 'V', 'I', ' '};
  char source_path[256], output_path[256];
  FILE *file;
  struct byte_buffer descriptor;
  int ok;

  if (!make_clean_directory(source) || !make_clean_directory(output)) return 0;
  snprintf(source_path, sizeof(source_path), "%s/ZZ9000-WebP.dtid", source);
  file = fopen(source_path, "wb");
  if (!file) return 0;
  fputs("FileName=Storage/DataTypes/ZZ9000-WebP\nVersion=42.5\n"
        "DTName=ZZ9000-WebP,zz9k-picture\nID=pict,webp\n"
        "Recog=52 49 46 46 ?? ?? ?? ?? 57 45 42 50\n"
        "Pattern=#?\nFlags=Binary,n,10\nInstall=inactive\n", file);
  fclose(file);
  if (!run_generator(python, generator, source, output, 0)) return 0;
  snprintf(output_path, sizeof(output_path), "%s/ZZ9000-WebP", output);
  descriptor = read_binary_file(output_path);
  ok = recognition_matches(&descriptor, webp_a, sizeof(webp_a)) &&
       recognition_matches(&descriptor, webp_b, sizeof(webp_b)) &&
       !recognition_matches(&descriptor, wave, sizeof(wave)) &&
       !recognition_matches(&descriptor, avi, sizeof(avi));
  if (!ok) printf("WebP wildcard recognition mismatch\n");
  free(descriptor.data);
  remove(source_path); remove(output_path);
  remove_directory(source); remove_directory(output);
  return ok;
}

static const unsigned char *find_chunk(const struct byte_buffer *blob,
                                       const char *id, size_t *length)
{
  size_t offset = 12U;
  while (offset + 8U <= blob->length) {
    unsigned long chunk_length = read_be32(blob->data + offset + 4U);
    if (chunk_length > blob->length - offset - 8U) return 0;
    if (!memcmp(blob->data + offset, id, 4U)) {
      *length = (size_t)chunk_length;
      return blob->data + offset + 8U;
    }
    offset += 8U + (size_t)chunk_length + (chunk_length & 1U);
  }
  return 0;
}

/* The MP3 descriptor delegates recognition to its DTCD hook: the mask must be
 * empty so datatypes.library calls the hook, and the hook bytes are embedded
 * verbatim. Byte masks cannot express "optional ID3v2 tag, then Layer III". */
static int check_mp3_hook(const struct byte_buffer *descriptor)
{
  size_t length = 0;
  const unsigned char *code = find_chunk(descriptor, "DTCD", &length);
  int ok = code && length == sizeof(placeholder_code) &&
           !memcmp(code, placeholder_code, sizeof(placeholder_code));
  if (!ok) printf("ZZ9000-MP3: DTCD hook chunk missing or altered\n");
  return ok;
}

static int expect_generator_rejects(const char *python, const char *generator,
                                    const char *label, const char *metadata,
                                    const char *code_dir)
{
  const char *source = "datatype_descriptor_test_code_source";
  const char *output = "datatype_descriptor_test_code_output";
  char path[256], output_path[256];
  int rejected;
  if (!make_clean_directory(source) || !make_clean_directory(output)) return 0;
  snprintf(path, sizeof(path), "%s/bad.dtid", source);
  snprintf(output_path, sizeof(output_path), "%s/bad", output);
  if (!write_file(path, metadata, strlen(metadata))) return 0;
  rejected = !run_generator(python, generator, source, output, code_dir);
  if (!rejected) printf("generator accepted %s\n", label);
  remove(output_path); remove(path);
  remove_directory(source); remove_directory(output);
  return rejected;
}

static int check_code_rules(const char *python, const char *generator,
                            const char *code_dir)
{
  static const char base[] =
      "FileName=Storage/DataTypes/bad\nVersion=42.5\nDTName=bad,zz9k-sound\n"
      "ID=soun,bad<nul>\nPattern=#?\nFlags=Binary,n,10\nInstall=inactive\n";
  static const char not_hunk[] = "not an executable";
  char metadata[512], path[256];
  int ok;
  snprintf(metadata, sizeof(metadata), "%sRecog=none\n", base);
  ok = expect_generator_rejects(python, generator,
                                "an empty mask without a hook", metadata,
                                code_dir);
  snprintf(metadata, sizeof(metadata), "%sRecog=none\nCode=hook\n", base);
  ok &= expect_generator_rejects(python, generator,
                                 "a hook without --code-dir", metadata, 0);
  snprintf(path, sizeof(path), "%s/hook", code_dir);
  if (!write_file(path, not_hunk, sizeof(not_hunk))) return 0;
  ok &= expect_generator_rejects(python, generator,
                                 "a non-hunk hook", metadata, code_dir);
  remove(path);
  return ok;
}

int main(int argc, char **argv)
{
  static const int jpeg_recognition[] = {0xff, 0xd8, 0xff};
  static const int png_recognition[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a};
  static const int webp_recognition[] = {
      0x52, 0x49, 0x46, 0x46, -1, -1, -1, -1, 0x57, 0x45, 0x42, 0x50
  };
  static const int flac_recognition[] = {0x66, 0x4c, 0x61, 0x43};
  static const unsigned char native_flac[] = {'f', 'L', 'a', 'C', 0x00, 0x00};
  static const unsigned char ogg_flac[] = {'O', 'g', 'g', 'S', 0x00, 0x02};
  const char *code_dir = "datatype_descriptor_test_code";
  char code_path[512];
  const char *output = "datatype_descriptor_test_output";
  char jpeg_path[512], png_path[512], webp_path[512], mp3_path[512],
      flac_path[512];
  struct byte_buffer jpeg, png, webp, mp3, flac, jpeg_icon, png_icon,
      webp_icon, mp3_icon, flac_icon;
  int ok;
  if (argc != 9) {
    printf("usage: %s <python> <generator> <descriptor-dir> <jpeg.info> <png.info> <webp.info> <mp3.info> <flac.info>\n",
           argv[0]);
    return 2;
  }
  snprintf(code_path, sizeof(code_path), "%s/zz9k-mp3-recog", code_dir);
  if (!make_clean_directory(output) || !make_clean_directory(code_dir) ||
      !write_file(code_path, placeholder_code, sizeof(placeholder_code)) ||
      !run_generator(argv[1], argv[2], argv[3], output, code_dir)) return 1;
  snprintf(jpeg_path, sizeof(jpeg_path), "%s/ZZ9000-JPEG", output);
  snprintf(png_path, sizeof(png_path), "%s/ZZ9000-PNG", output);
  snprintf(webp_path, sizeof(webp_path), "%s/ZZ9000-WebP", output);
  snprintf(mp3_path, sizeof(mp3_path), "%s/ZZ9000-MP3", output);
  snprintf(flac_path, sizeof(flac_path), "%s/ZZ9000-FLAC", output);
  jpeg = read_binary_file(jpeg_path);
  png = read_binary_file(png_path);
  webp = read_binary_file(webp_path);
  mp3 = read_binary_file(mp3_path);
  flac = read_binary_file(flac_path);
  jpeg_icon = read_binary_file(argv[4]);
  png_icon = read_binary_file(argv[5]);
  webp_icon = read_binary_file(argv[6]);
  mp3_icon = read_binary_file(argv[7]);
  flac_icon = read_binary_file(argv[8]);
  ok = validate_descriptor("ZZ9000-JPEG", &jpeg, "ZZ9000-JPEG", "jpeg",
                           "pict", "zz9k-picture",
                           jpeg_recognition, sizeof(jpeg_recognition) / sizeof(jpeg_recognition[0]), 106U);
  ok &= validate_descriptor("ZZ9000-PNG", &png, "ZZ9000-PNG", "png\0",
                            "pict", "zz9k-picture",
                            png_recognition, sizeof(png_recognition) / sizeof(png_recognition[0]), 110U);
  ok &= validate_descriptor("ZZ9000-WebP", &webp, "ZZ9000-WebP", "webp",
                            "pict", "zz9k-picture",
                            webp_recognition, sizeof(webp_recognition) / sizeof(webp_recognition[0]), 124U);
  ok &= validate_descriptor("ZZ9000-MP3", &mp3, "ZZ9000-MP3", "mp3 ",
                            "soun", "zz9k-sound", 0, 0U, 116U);
  ok &= validate_icon("ZZ9000-MP3.info", &mp3_icon);
  ok &= validate_descriptor("ZZ9000-FLAC", &flac, "ZZ9000-FLAC", "flac",
                            "soun", "zz9k-sound", flac_recognition,
                            sizeof(flac_recognition) / sizeof(flac_recognition[0]),
                            106U);
  ok &= validate_icon("ZZ9000-FLAC.info", &flac_icon);
  if (!recognition_matches(&flac, native_flac, sizeof(native_flac))) {
    printf("ZZ9000-FLAC: failed to match native FLAC header\n");
    ok = 0;
  }
  if (recognition_matches(&flac, ogg_flac, sizeof(ogg_flac))) {
    printf("ZZ9000-FLAC: unexpectedly matched Ogg-FLAC header\n");
    ok = 0;
  }
  ok &= check_mp3_hook(&mp3);
  ok &= check_code_rules(argv[1], argv[2], code_dir);
  ok &= validate_icon("ZZ9000-JPEG.info", &jpeg_icon);
  ok &= validate_icon("ZZ9000-PNG.info", &png_icon);
  ok &= validate_icon("ZZ9000-WebP.info", &webp_icon);
  ok &= check_malformed_metadata(argv[1], argv[2]);
  ok &= check_webp_wildcards(argv[1], argv[2]);
  free(jpeg.data); free(png.data); free(webp.data);
  free(mp3.data); free(mp3_icon.data);
  free(flac.data); free(flac_icon.data);
  remove(jpeg_path); remove(png_path); remove(webp_path); remove(mp3_path);
  remove(flac_path);
  remove(code_path);
  remove_directory(output);
  remove_directory(code_dir);
  return ok ? 0 : 1;
}

