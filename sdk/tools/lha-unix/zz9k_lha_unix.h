/*
 * Small public wrapper around the embedded LHa for UNIX decoder subset.
 */

#ifndef ZZ9K_LHA_UNIX_H
#define ZZ9K_LHA_UNIX_H

#include <stdint.h>
#include <stdio.h>

int zz9k_lha_unix_decode_lh5(FILE *input,
                             FILE *output,
                             uint32_t original_size,
                             uint32_t packed_size,
                             uint16_t expected_crc,
                             int check_crc,
                             uint16_t *actual_crc);
int zz9k_lha_unix_decode_method(FILE *input,
                                FILE *output,
                                uint32_t original_size,
                                uint32_t packed_size,
                                uint16_t expected_crc,
                                int check_crc,
                                uint16_t *actual_crc,
                                int method);

/*
 * Decide whether a decode result should be accepted. The CRC-16 over the
 * decoded output is LHA's authoritative integrity check, so when a CRC is
 * present a matching CRC accepts the decode even if the decoder did not consume
 * every packed byte -- read_size can legitimately be a little short of
 * packed_size (see "usually read size is interface->packed" in slide.c). Only
 * when no CRC is available does read_size == packed_size serve as the fallback
 * sanity check. A decoder error always rejects. Returns 1 to accept, 0 to
 * reject. Exposed for unit testing.
 */
int zz9k_lha_unix_decode_accept(int had_error,
                                uint32_t read_size,
                                uint32_t packed_size,
                                int check_crc,
                                uint16_t actual_crc,
                                uint16_t expected_crc);

#endif
