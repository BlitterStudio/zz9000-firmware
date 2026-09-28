/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-stream.h"

#include <string.h>

void zzplay_transport_init(ZZPlayTransport *transport)
{
  if (transport) {
    memset(transport, 0, sizeof(*transport));
  }
}

void zzplay_transport_set_chunk(ZZPlayTransport *transport,
                                uint32_t length,
                                int eof)
{
  if (!transport) {
    return;
  }
  transport->pending_offset = 0U;
  transport->pending_length = length;
  transport->eof = eof ? 1 : 0;
}

int zzplay_advance_input(uint32_t *offset,
                         uint32_t *remaining,
                         uint32_t *accepted_total,
                         uint32_t reported_total)
{
  uint32_t accepted;

  if (!offset || !remaining || !accepted_total ||
      reported_total < *accepted_total) {
    return 0;
  }
  accepted = reported_total - *accepted_total;
  if (accepted > *remaining) {
    return 0;
  }
  *offset += accepted;
  *remaining -= accepted;
  *accepted_total = reported_total;
  return 1;
}

int zzplay_transport_advance(ZZPlayTransport *transport,
                             uint32_t reported_total)
{
  return transport &&
         zzplay_advance_input(&transport->pending_offset,
                              &transport->pending_length,
                              &transport->accepted_total,
                              reported_total);
}

uint32_t zzplay_transport_write_flags(
    const ZZPlayTransport *transport)
{
  if (transport && transport->eof &&
      transport->pending_length == 0U && !transport->eof_sent) {
    return ZZ9K_VIDEO_SESSION_WRITE_EOF;
  }
  return 0U;
}
