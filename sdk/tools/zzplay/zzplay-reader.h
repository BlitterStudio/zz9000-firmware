/* Read-ahead input for zzplay's streaming players.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_READER_H
#define ZZPLAY_READER_H

#include <stdint.h>

/* A child process reads the file in large blocks into a fast-RAM ring while
 * the player waits on the card, so disk time overlaps decoding instead of
 * adding to every frame. One reader exists at a time. */
typedef struct ZZPlayReader ZZPlayReader;

/* Opens path and starts reading; NULL when the file, the memory or the
 * process cannot be had. */
ZZPlayReader *zzplay_reader_open(const char *path);

/* The unread bytes of the oldest filled block. Returns 0 when the read
 * failed and nothing is left to deliver. Otherwise *length is 0 while the
 * next block is still being read, and *eof is set when no data follows
 * the bytes returned. */
int zzplay_reader_peek(ZZPlayReader *reader, const uint8_t **data,
                       uint32_t *length, int *eof);

/* Marks bytes from the last peek as taken. */
void zzplay_reader_consume(ZZPlayReader *reader, uint32_t bytes);

/* Stops the process, waits for it and frees everything. NULL is ignored. */
void zzplay_reader_close(ZZPlayReader *reader);

#endif /* ZZPLAY_READER_H */
