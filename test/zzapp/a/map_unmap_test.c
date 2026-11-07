/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "zzapp.h"
#include "zzapp_runtime.h"

int zzapp_host_mem_info(zzapp_mem_info *out);

static const uint8_t file_data[] = "mapped";
static int oversize_fixture;
static int stop_during_read;

int zzapp_svc_should_stop(void) { return stop_during_read; }

int zzapp_svc_file_stat(const char *path, zzapp_stat *out)
{
    if (!path || !out)
        return ZZ_ENOENT;
    if (strcmp(path, "RAM:oversize") == 0 && oversize_fixture) {
        out->size = (16u << 20) + 1u;
        out->mode = 1u;
        return ZZ_OK;
    }
    if (strcmp(path, "RAM:fixture") != 0)
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
    if (stop_during_read) {
        zzapp_host_tick_at(1u);
        zzapp_host_tick_at(1u + ZZAPP_STOP_GRACE_US);
        return ZZ_EFAULT;
    }
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
    oversize_fixture = 1;
    mapped = (const void *)(uintptr_t)1u;
    bytes = 1u;
    assert(api->map_file("RAM:oversize", &mapped, &bytes) == ZZ_ENOSPC);
    assert(mapped == NULL && bytes == 0u);
    oversize_fixture = 0;

    zzapp_test_mark_running();
    stop_during_read = 1;
    mapped = (const void *)(uintptr_t)1u;
    bytes = 1u;
    assert(api->map_file("RAM:fixture", &mapped, &bytes) == ZZ_EFAULT);
    assert(mapped == NULL && bytes == 0u);
    assert(zzapp_host_mem_info(&mem) == ZZ_OK && mem.host_visible_used == 0u);
    stop_during_read = 0;
    zzapp_loader_release();
    return 0;
}
