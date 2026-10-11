/* Read-ahead input for zzplay's streaming players.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_READER_H
#define ZZPLAY_READER_H

#include <stdint.h>

/* A child process reads the file in large blocks and copies them into
 * slots of the card-visible input buffer while the player waits on the
 * card, so neither the disk nor the copy to the card adds to a frame. One
 * reader exists at a time. */
typedef struct ZZPlayReader ZZPlayReader;

/* Opens path and starts filling card (card_bytes, at least two slots'
 * worth) from it. The reader writes card until it is closed, so close it
 * before the buffer goes. NULL when the file, the memory or the process
 * cannot be had. */
ZZPlayReader *zzplay_reader_open(const char *path, uint8_t *card,
                                 uint32_t card_bytes);

/* The unwritten bytes of the oldest filled slot, as an offset into the
 * card buffer. Returns 0 when the read failed and nothing is left to
 * deliver. Otherwise *length is 0 while the next slot is still being
 * filled, and *eof is set when no data follows the bytes returned. */
int zzplay_reader_peek(ZZPlayReader *reader, uint32_t *offset,
                       uint32_t *length, int *eof);

/* Marks bytes from the last peek as taken by the card; a fully taken slot
 * goes back to the reader. */
void zzplay_reader_consume(ZZPlayReader *reader, uint32_t bytes);

/* Stops the process, waits for it and frees everything. NULL is ignored. */
void zzplay_reader_close(ZZPlayReader *reader);

#endif /* ZZPLAY_READER_H */
