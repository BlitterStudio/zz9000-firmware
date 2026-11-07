/*
 * File-in/file-out card-service template.  It writes the SHA-256 of an input
 * file, while exercising the launcher file proxy and app-memory service API.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"
#include "zzapp_note.h"

ZZAPP_NOTE(0, 0)

static int write_all(int fd, const uint8_t *data, uint32_t bytes)
{
    uint32_t done = 0;

    while (done < bytes) {
        uint32_t written = 0;
        int rc = zz_file_write(fd, data + done, bytes - done, &written);

        if (rc != ZZ_OK || written == 0u)
            return rc != ZZ_OK ? rc : ZZ_EIO;
        done += written;
    }
    return ZZ_OK;
}

int zz_main(int argc, char **argv)
{
    const void *input;
    uint32_t input_bytes;
    uint8_t digest[32];
    int out;
    int rc;

    if (argc != 3)
        return 5;
    rc = zz_map_file(argv[1], &input, &input_bytes);
    if (rc != ZZ_OK)
        return 10;
    rc = zz_sha256(input, input_bytes, digest);
    zz_unmap_file(input);
    if (rc != ZZ_OK)
        return 10;
    out = zz_file_open(argv[2], ZZ_FILE_WRITE | ZZ_FILE_CREATE);
    if (out < 0)
        return 20;
    rc = write_all(out, digest, sizeof digest);
    (void)zz_file_close(out);
    return rc == ZZ_OK ? 0 : 20;
}
