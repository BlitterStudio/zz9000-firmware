/*
 * Small public wrapper around the embedded LHa for UNIX decoder subset.
 *
 * The decoder sources are from jca02266/lha 1.14i-ac20220213 and retain
 * their original redistribution terms.
 */

#define HAVE_CONFIG_H 1
#include "lha.h"
#include "zz9k_lha_unix.h"

extern int zz9k_lha_unix_error;

int
zz9k_lha_unix_decode_accept(int had_error,
                            uint32_t read_size,
                            uint32_t packed_size,
                            int check_crc,
                            uint16_t actual_crc,
                            uint16_t expected_crc)
{
    if (had_error) {
        return 0;
    }
    if (check_crc) {
        /* CRC-16 over the output is authoritative: accept regardless of how
         * many packed bytes the decoder consumed. */
        return actual_crc == expected_crc ? 1 : 0;
    }
    /* No CRC to lean on: fall back to the read-size sanity check. Bug #39:
     * actual_crc/expected_crc are NOT consulted here, so a caller must not
     * report this outcome with CRC wording -- an "actual == expected"
     * coincidence (e.g. both zero) must never be read as a CRC match when
     * check_crc is off. Callers should report the read-size/decode failure
     * without a "crc=... expected=..." line in that case (see
     * zz9k_archive_lha_decode_method_to_file). */
    return read_size == packed_size ? 1 : 0;
}

int
zz9k_lha_unix_decode_method(FILE *input,
                            FILE *output,
                            uint32_t original_size,
                            uint32_t packed_size,
                            uint16_t expected_crc,
                            int check_crc,
                            uint16_t *actual_crc,
                            int method)
{
    off_t read_size = 0;
    unsigned int crc;

    if (!input) {
        return 0;
    }
    zz9k_lha_unix_error = 0;
    make_crctable();
    quiet = TRUE;
    quiet_mode = 2;
    verify_mode = output == NULL ? TRUE : FALSE;
    text_mode = FALSE;
    output_to_stdout = FALSE;
    dump_lzss = FALSE;
    extract_broken_archive = FALSE;
    crc = decode_lzhuf(input, output,
                       (off_t)original_size,
                       (off_t)packed_size,
                       "zz9k-archive",
                       method,
                       &read_size);
    if (actual_crc) {
        *actual_crc = (uint16_t)crc;
    }
    return zz9k_lha_unix_decode_accept(zz9k_lha_unix_error,
                                       (uint32_t)read_size, packed_size,
                                       check_crc, (uint16_t)crc, expected_crc);
}

int
zz9k_lha_unix_decode_lh5(FILE *input,
                         FILE *output,
                         uint32_t original_size,
                         uint32_t packed_size,
                         uint16_t expected_crc,
                         int check_crc,
                         uint16_t *actual_crc)
{
    return zz9k_lha_unix_decode_method(input, output,
                                       original_size, packed_size,
                                       expected_crc, check_crc, actual_crc,
                                       LZHUFF5_METHOD_NUM);
}
