/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "zzapp.h"
#include "zzapp_runtime.h"

int zzapp_host_mem_info(zzapp_mem_info *out);

static const uint8_t file_data[] = "mapped";

int zzapp_svc_file_stat(const char *path, zzapp_stat *out)
{
    if (!path || strcmp(path, "RAM:fixture") != 0 || !out)
        return ZZ_ENOENT;
    out->size = sizeof(file_data) - 1u;
    out->mode = 1u;
    return ZZ_OK;
}

int zzapp_svc_file_open(const char *path, uint32_t flags)
{
    return path && strcmp(path, "RAM:fixture") == 0 &&
           flags == ZZ_FILE_READ ? 1 : ZZ_ENOENT;
}

int zzapp_svc_file_read(int fd, void *dst, uint32_t bytes, uint32_t *done)
{
    assert(fd == 1 && bytes >= sizeof(file_data) - 1u);
    memcpy(dst, file_data, sizeof(file_data) - 1u);
    *done = sizeof(file_data) - 1u;
    return ZZ_OK;
}

int zzapp_svc_file_close(int fd)
{
    assert(fd == 1);
    return ZZ_OK;
}

int main(void)
{
    const zzapp_api *api;
    zzapp_mem_info mem;
    const void *mapped;
    uint32_t bytes;

    zzapp_boot_init();
    assert(zzapp_loader_begin() == ZZ_OK);
    api = zzapp_api_table();
    assert(api->map_file("RAM:fixture", &mapped, &bytes) == ZZ_OK);
    assert(bytes == sizeof(file_data) - 1u);
    assert(api->mem_info(&mem) == ZZ_OK);
    assert(mem.host_visible_used == bytes);
    api->unmap_file(mapped);
    assert(api->mem_info(&mem) == ZZ_OK);
    assert(mem.host_visible_used == 0u);
    assert(api->map_file("RAM:fixture", &mapped, &bytes) == ZZ_OK);
    api->unmap_file(mapped);
    assert(api->mem_info(&mem) == ZZ_OK && mem.host_visible_used == 0u);
    zzapp_loader_release();
    return 0;
}
