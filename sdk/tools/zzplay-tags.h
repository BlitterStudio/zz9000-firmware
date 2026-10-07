/* ID3 metadata for zzplay: enough to show "Artist - Title" and to know where
 * the audio starts and ends inside an MP3 file.
 *
 * Supports ID3v2.2/2.3/2.4 text frames (TT2/TP1/TAL, TIT2/TPE1/TALB) in
 * ISO-8859-1, UTF-16 with BOM, UTF-16BE and UTF-8, converted to Latin-1
 * (the Amiga's native charset; unmappable characters become '?'), plus the
 * ID3v1 trailer as a fallback. Unsynchronisation, compression and
 * encryption of individual frames are not decoded: such frames are skipped,
 * never misread.
 *
 * Host-testable. SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_TAGS_H
#define ZZPLAY_TAGS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define ZZPLAY_TAG_TEXT_MAX 64U

typedef struct ZZPlayTags {
  char title[ZZPLAY_TAG_TEXT_MAX];
  char artist[ZZPLAY_TAG_TEXT_MAX];
  char album[ZZPLAY_TAG_TEXT_MAX];
  /* Bytes of ID3v2 tag (header, footer and padding included) at the start
   * of the file: where the first audio frame can begin. 0 without a tag. */
  uint32_t audio_start;
  /* Bytes of ID3v1 trailer ("TAG", 128 bytes) at the end, 0 or 128. */
  uint32_t trailer_bytes;
} ZZPlayTags;

/* Size of an ID3v2 tag from its 10-byte header, or 0 when `header` is not
 * one. */
uint32_t zzplay_tags_id3v2_size(const uint8_t *header, size_t length);

/* Parse the text frames of a complete ID3v2 tag (starting at its header).
 * Fills only fields that are empty in `tags`. Returns 1 for a valid tag. */
int zzplay_tags_parse_id3v2(const uint8_t *data, size_t length,
                            ZZPlayTags *tags);

/* Parse a 128-byte ID3v1 trailer. Fills only empty fields (so ID3v2 wins)
 * and trims trailing spaces/NULs. Returns 1 for a valid trailer. */
int zzplay_tags_parse_id3v1(const uint8_t *trailer, size_t length,
                            ZZPlayTags *tags);

/* Read both tag kinds from an open file. Reads at most the first 64 KiB of
 * an ID3v2 tag for text frames (audio_start still reports the full size).
 * Restores the file position. Returns 1 when any field was found. */
int zzplay_tags_read(FILE *file, ZZPlayTags *tags);

/* "Artist - Title", "Title", or "" when neither is known. */
void zzplay_tags_display(const ZZPlayTags *tags, char *out, size_t capacity);

#endif /* ZZPLAY_TAGS_H */
