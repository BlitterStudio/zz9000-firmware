/* Streaming-input transport state for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_STREAM_H
#define ZZPLAY_STREAM_H

#include "zzplay-video.h"

#include <stdint.h>

typedef struct ZZPlayTransport {
  uint32_t accepted_total;
  uint32_t pending_offset;
  uint32_t pending_length;
  int eof;
  int eof_sent;
} ZZPlayTransport;

void zzplay_transport_init(ZZPlayTransport *transport);
void zzplay_transport_set_chunk(ZZPlayTransport *transport,
                                uint32_t length,
                                int eof);
int zzplay_transport_advance(ZZPlayTransport *transport,
                             uint32_t reported_total);
uint32_t zzplay_transport_write_flags(
    const ZZPlayTransport *transport);

int zzplay_advance_input(uint32_t *offset,
                         uint32_t *remaining,
                         uint32_t *accepted_total,
                         uint32_t reported_total);

#endif /* ZZPLAY_STREAM_H */
