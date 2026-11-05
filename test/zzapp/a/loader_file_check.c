/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "zzapp.h"
#include "zzapp_runtime.h"

#ifndef ZZAPP_PROBE_ELF
#error "ZZAPP_PROBE_ELF must name the app ELF"
#endif

int main(void)
{
    FILE *file;
    long size;
    uint8_t *image;
    uint32_t offset;

    file = fopen(ZZAPP_PROBE_ELF, "rb");
    assert(file != NULL);
    assert(fseek(file, 0L, SEEK_END) == 0);
    size = ftell(file);
    assert(size > 0L);
    assert(fseek(file, 0L, SEEK_SET) == 0);
    image = malloc((size_t)size);
    assert(image != NULL);
    assert(fread(image, 1u, (size_t)size, file) == (size_t)size);
    assert(fclose(file) == 0);

    zzapp_boot_init();
    assert(zzapp_loader_begin() == ZZ_OK);
    for (offset = 0u; offset < (uint32_t)size;) {
        uint32_t chunk = (uint32_t)size - offset;
        if (chunk > 65536u)
            chunk = 65536u;
        assert(zzapp_loader_chunk(offset, image + offset, chunk) == ZZ_OK);
        offset += chunk;
    }
    assert(zzapp_loader_end() == ZZ_OK);
    zzapp_loader_release();
    free(image);
    return 0;
}
