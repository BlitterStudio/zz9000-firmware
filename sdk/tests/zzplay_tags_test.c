/* Behavioural tests for ID3 tags (zzplay-tags.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/zzplay-tags.h"

static int test_id3v22_parsing(void)
{
  uint8_t tag[128];
  size_t offset = 0;
  ZZPlayTags tags;

  memset(tag, 0, sizeof(tag));
  /* ID3v2.2 header */
  memcpy(tag, "ID3", 3);
  tag[3] = 2; /* major 2 */
  tag[4] = 0; /* rev 0 */
  tag[5] = 0; /* flags */
  /* Size syncsafe filled later */
  offset = 10;

  /* TT2: Title = "Song TwoTwo" */
  memcpy(tag + offset, "TT2", 3);
  {
    static const char title_str[] = "Song TwoTwo";
    size_t slen = strlen(title_str);
    tag[offset + 3] = 0;
    tag[offset + 4] = 0;
    tag[offset + 5] = (uint8_t)(1 + slen); /* 1 byte encoding + text */
    tag[offset + 6] = 0; /* ISO-8859-1 */
    memcpy(tag + offset + 7, title_str, slen);
    offset += 6 + 1 + slen;
  }

  /* TP1: Artist = "Artist 22" */
  memcpy(tag + offset, "TP1", 3);
  {
    static const char art_str[] = "Artist 22";
    size_t slen = strlen(art_str);
    tag[offset + 3] = 0;
    tag[offset + 4] = 0;
    tag[offset + 5] = (uint8_t)(1 + slen);
    tag[offset + 6] = 0;
    memcpy(tag + offset + 7, art_str, slen);
    offset += 6 + 1 + slen;
  }

  /* Syncsafe size */
  {
    uint32_t body = (uint32_t)(offset - 10);
    tag[6] = (uint8_t)((body >> 21) & 0x7F);
    tag[7] = (uint8_t)((body >> 14) & 0x7F);
    tag[8] = (uint8_t)((body >> 7) & 0x7F);
    tag[9] = (uint8_t)(body & 0x7F);
  }

  memset(&tags, 0, sizeof(tags));
  if (!zzplay_tags_parse_id3v2(tag, offset, &tags)) return 1;
  if (strcmp(tags.title, "Song TwoTwo") != 0) return 2;
  if (strcmp(tags.artist, "Artist 22") != 0) return 3;

  return 0;
}

static int test_id3v23_extended_and_utf16(void)
{
  uint8_t tag[256];
  size_t offset = 0;
  ZZPlayTags tags;

  memset(tag, 0, sizeof(tag));
  /* ID3v2.3 header with extended header flag (bit 6 = 0x40) */
  memcpy(tag, "ID3", 3);
  tag[3] = 3;
  tag[4] = 0;
  tag[5] = 0x40; /* extended header */
  offset = 10;

  /* Extended header: size = 6 (4 bytes length + 6 bytes data = 10) */
  tag[offset] = 0; tag[offset + 1] = 0; tag[offset + 2] = 0; tag[offset + 3] = 6;
  memset(tag + offset + 4, 0, 6);
  offset += 10;

  /* TIT2 with UTF-16 LE (BOM 0xFF 0xFE): "AB" + char 0x0100 (becomes '?') */
  memcpy(tag + offset, "TIT2", 4);
  /* frame size 4 bytes big endian */
  tag[offset + 4] = 0; tag[offset + 5] = 0; tag[offset + 6] = 0; tag[offset + 7] = 9;
  tag[offset + 8] = 0; tag[offset + 9] = 0; /* flags */
  tag[offset + 10] = 1; /* UTF-16 with BOM */
  tag[offset + 11] = 0xFF; tag[offset + 12] = 0xFE; /* BOM LE */
  tag[offset + 13] = 'A';  tag[offset + 14] = 0;
  tag[offset + 15] = 'B';  tag[offset + 16] = 0;
  tag[offset + 17] = 0x00; tag[offset + 18] = 0x01; /* U+0100 (> 0xFF) */
  offset += 19;

  /* Set syncsafe size */
  {
    uint32_t body = (uint32_t)(offset - 10);
    tag[6] = (uint8_t)((body >> 21) & 0x7F);
    tag[7] = (uint8_t)((body >> 14) & 0x7F);
    tag[8] = (uint8_t)((body >> 7) & 0x7F);
    tag[9] = (uint8_t)(body & 0x7F);
  }

  memset(&tags, 0, sizeof(tags));
  if (!zzplay_tags_parse_id3v2(tag, offset, &tags)) return 1;
  /* 'A', 'B', '?' */
  if (strcmp(tags.title, "AB?") != 0) return 2;

  return 0;
}

static int test_id3v24_utf8_and_skip(void)
{
  uint8_t tag[256];
  size_t offset = 0;
  ZZPlayTags tags;

  memset(tag, 0, sizeof(tag));
  /* ID3v2.4 header with footer flag (bit 4 = 0x10) */
  memcpy(tag, "ID3", 3);
  tag[3] = 4;
  tag[4] = 0;
  tag[5] = 0x10; /* footer flag */
  offset = 10;

  /* Frame 1: Encrypted frame (must be skipped) */
  memcpy(tag + offset, "TIT2", 4);
  tag[offset + 4] = 0; tag[offset + 5] = 0; tag[offset + 6] = 0; tag[offset + 7] = 5;
  tag[offset + 8] = 0;
  tag[offset + 9] = 0x04; /* encryption flag in v2.4 (bit 2) */
  memcpy(tag + offset + 10, "\0JUNK", 5);
  offset += 15;

  /* Frame 2: Valid TIT2 with UTF-8 (encoding 3)
   * "Café" (é is U+00E9, in UTF-8: 0xC3 0xA9) and a non-Latin1 character U+20AC (€, in UTF-8: 0xE2 0x82 0xAC) */
  memcpy(tag + offset, "TIT2", 4);
  {
    static const uint8_t text_bytes[] = {
      3, /* encoding UTF-8 */
      'C', 'a', 'f', 0xC3, 0xA9, ' ', 0xE2, 0x82, 0xAC
    };
    size_t fsz = sizeof(text_bytes);
    tag[offset + 4] = (uint8_t)((fsz >> 21) & 0x7F);
    tag[offset + 5] = (uint8_t)((fsz >> 14) & 0x7F);
    tag[offset + 6] = (uint8_t)((fsz >> 7) & 0x7F);
    tag[offset + 7] = (uint8_t)(fsz & 0x7F);
    tag[offset + 8] = 0; tag[offset + 9] = 0;
    memcpy(tag + offset + 10, text_bytes, fsz);
    offset += 10 + fsz;
  }

  /* Set syncsafe size */
  {
    uint32_t body = (uint32_t)(offset - 10);
    tag[6] = (uint8_t)((body >> 21) & 0x7F);
    tag[7] = (uint8_t)((body >> 14) & 0x7F);
    tag[8] = (uint8_t)((body >> 7) & 0x7F);
    tag[9] = (uint8_t)(body & 0x7F);
  }

  /* Footer adds 10 bytes to total size */
  if (zzplay_tags_id3v2_size(tag, offset) != (uint32_t)(offset + 10)) return 1;

  memset(&tags, 0, sizeof(tags));
  if (!zzplay_tags_parse_id3v2(tag, offset, &tags)) return 2;
  /* "Caf\xE9 ?" */
  if (tags.title[0] != 'C' || tags.title[1] != 'a' || tags.title[2] != 'f' ||
      (unsigned char)tags.title[3] != 0xE9 || tags.title[4] != ' ' ||
      tags.title[5] != '?' || tags.title[6] != '\0')
    return 3;

  return 0;
}

static int test_id3v1_fallback_and_display(void)
{
  uint8_t v1[128];
  ZZPlayTags tags;
  char display[128];

  memset(v1, ' ', sizeof(v1));
  memcpy(v1, "TAG", 3);
  memcpy(v1 + 3, "Song Title                    ", 30);
  memcpy(v1 + 33, "Band Name                     ", 30);
  memcpy(v1 + 63, "The Album                     ", 30);

  memset(&tags, 0, sizeof(tags));
  if (!zzplay_tags_parse_id3v1(v1, sizeof(v1), &tags)) return 1;

  if (strcmp(tags.title, "Song Title") != 0) return 2;
  if (strcmp(tags.artist, "Band Name") != 0) return 3;
  if (strcmp(tags.album, "The Album") != 0) return 4;

  /* Display formatting */
  zzplay_tags_display(&tags, display, sizeof(display));
  if (strcmp(display, "Band Name - Song Title") != 0) return 5;

  /* Only title */
  tags.artist[0] = '\0';
  zzplay_tags_display(&tags, display, sizeof(display));
  if (strcmp(display, "Song Title") != 0) return 6;

  /* Neither */
  tags.title[0] = '\0';
  zzplay_tags_display(&tags, display, sizeof(display));
  if (strcmp(display, "") != 0) return 7;

  return 0;
}

static int test_tags_read_from_file(void)
{
  const char *tmp_path = "T_zzplay_test_tags.tmp";
  FILE *f;
  uint8_t tag_hdr[10];
  uint8_t v1[128];
  ZZPlayTags tags;
  long pos_before;

  /* Create file with ID3v2 at start and ID3v1 at end */
  f = fopen(tmp_path, "wb");
  if (!f) return 1;

  /* v2 header with 0 body size */
  memset(tag_hdr, 0, sizeof(tag_hdr));
  memcpy(tag_hdr, "ID3", 3);
  tag_hdr[3] = 3;
  fwrite(tag_hdr, 1, 10, f);

  /* Some dummy data in middle */
  fwrite("DUMMY AUDIO DATA", 1, 16, f);

  /* ID3v1 trailer at end */
  memset(v1, ' ', sizeof(v1));
  memcpy(v1, "TAG", 3);
  memcpy(v1 + 3, "V1 Track                      ", 30);
  fwrite(v1, 1, 128, f);

  fclose(f);

  /* Now read with zzplay_tags_read */
  f = fopen(tmp_path, "rb");
  if (!f) {
    remove(tmp_path);
    return 2;
  }
  fseek(f, 5, SEEK_SET); /* Set non-zero position */
  pos_before = ftell(f);

  if (!zzplay_tags_read(f, &tags)) {
    fclose(f);
    remove(tmp_path);
    return 3;
  }

  /* File position must be restored */
  if (ftell(f) != pos_before) {
    fclose(f);
    remove(tmp_path);
    return 4;
  }
  fclose(f);
  remove(tmp_path);

  if (tags.audio_start != 10U) return 5;
  if (tags.trailer_bytes != 128U) return 6;
  if (strcmp(tags.title, "V1 Track") != 0) return 7;

  return 0;
}

static int test_truncated_input(void)
{
  uint8_t short_data[8];
  ZZPlayTags tags;

  memset(short_data, 0, sizeof(short_data));
  memcpy(short_data, "ID3", 3);

  if (zzplay_tags_id3v2_size(short_data, 8U) != 0U) return 1;
  if (zzplay_tags_parse_id3v2(short_data, 8U, &tags) != 0) return 2;
  if (zzplay_tags_parse_id3v1(short_data, 8U, &tags) != 0) return 3;

  return 0;
}

int main(void)
{
  int rc;

  rc = test_id3v22_parsing();
  if (rc) {
    fprintf(stderr, "test_id3v22_parsing failed: %d\n", rc);
    return 1;
  }
  rc = test_id3v23_extended_and_utf16();
  if (rc) {
    fprintf(stderr, "test_id3v23_extended_and_utf16 failed: %d\n", rc);
    return 2;
  }
  rc = test_id3v24_utf8_and_skip();
  if (rc) {
    fprintf(stderr, "test_id3v24_utf8_and_skip failed: %d\n", rc);
    return 3;
  }
  rc = test_id3v1_fallback_and_display();
  if (rc) {
    fprintf(stderr, "test_id3v1_fallback_and_display failed: %d\n", rc);
    return 4;
  }
  rc = test_tags_read_from_file();
  if (rc) {
    fprintf(stderr, "test_tags_read_from_file failed: %d\n", rc);
    return 5;
  }
  rc = test_truncated_input();
  if (rc) {
    fprintf(stderr, "test_truncated_input failed: %d\n", rc);
    return 6;
  }

  printf("zzplay_tags_test: all tests passed\n");
  return 0;
}
