/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "aar/app.h"
#include "zzapp_runtime.h"

static uint64_t shared_used_now(const aar_api *api)
{
    aar_info info;

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    assert(api->info(&info) == AAR_OK);
    return info.shared_used;
}

static const uint8_t file_data[] = "mapped";
static int oversize_fixture;
static int stop_during_read;

int zzapp_svc_should_stop(void) { return stop_during_read; }

int zzapp_svc_file_stat(const char *path, aar_stat *out)
{
    if (!path || !out)
        return AAR_E_NOENT;
    if (strcmp(path, "RAM:oversize") == 0 && oversize_fixture) {
        out->size = (16u << 20) + 1u;
        out->mode = 1u;
        return AAR_OK;
    }
    if (strcmp(path, "RAM:fixture") != 0)
        return AAR_E_NOENT;
    out->size = sizeof(file_data) - 1u;
    out->mode = 1u;
    return AAR_OK;
}

int zzapp_svc_file_open(const char *path, uint32_t flags)
{
    return path && strcmp(path, "RAM:fixture") == 0 &&
           flags == AAR_FILE_READ ? 1 : AAR_E_NOENT;
}

int zzapp_svc_file_read(int fd, void *dst, uint32_t bytes, uint32_t *done)
{
    assert(fd == 1 && bytes >= sizeof(file_data) - 1u);
    if (stop_during_read) {
        zzapp_host_tick_at(1u);
        zzapp_host_tick_at(1u + ZZAPP_STOP_GRACE_US);
        return AAR_E_FAULT;
    }
    memcpy(dst, file_data, sizeof(file_data) - 1u);
    *done = sizeof(file_data) - 1u;
    return AAR_OK;
}

int zzapp_svc_file_close(int fd)
{
    assert(fd == 1);
    return AAR_OK;
}

int main(void)
{
    const aar_api *api;
    const void *mapped;
    uint64_t bytes;

    zzapp_boot_init();
    assert(zzapp_loader_begin() == AAR_OK);
    api = zzapp_api_table();
    assert(api->map_file("RAM:fixture", &mapped, &bytes) == AAR_OK);
    assert(bytes == sizeof(file_data) - 1u);
    /* App-visible host memory is backed by whole pages, so no page is
     * shared between owners; usage counts that backing. */
    assert(shared_used_now(api) == ((bytes + 4095u) & ~4095u));
    api->unmap_file(mapped);
    assert(shared_used_now(api) == 0u);
    assert(api->map_file("RAM:fixture", &mapped, &bytes) == AAR_OK);
    api->unmap_file(mapped);
    assert(shared_used_now(api) == 0u);
    oversize_fixture = 1;
    mapped = (const void *)(uintptr_t)1u;
    bytes = 1u;
    assert(api->map_file("RAM:oversize", &mapped, &bytes) == AAR_E_NOSPC);
    assert(mapped == NULL && bytes == 0u);
    oversize_fixture = 0;

    zzapp_test_mark_running();
    stop_during_read = 1;
    mapped = (const void *)(uintptr_t)1u;
    bytes = 1u;
    assert(api->map_file("RAM:fixture", &mapped, &bytes) == AAR_E_FAULT);
    assert(mapped == NULL && bytes == 0u);
    assert(shared_used_now(api) == 0u);
    stop_during_read = 0;
    zzapp_loader_release();
    return 0;
}
