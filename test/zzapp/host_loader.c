/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_loader.h"

#include <setjmp.h>
#include <string.h>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

static jmp_buf exit_jump;
static int entered;
static int captured_exit;

static void host_exit(int code)
{
    captured_exit = code;
    if (entered)
        longjmp(exit_jump, 1);
}

int zzapp_host_enter(const zzapp_api *api, zzapp_host_main entry,
                     int argc, char **argv, int *exit_code)
{
    zzapp_api wrapped;
    int result;

    if (!api || !entry)
        return ZZ_EINVAL;
    wrapped = *api;
    wrapped.exit = host_exit;
    zzapp_api_ptr = &wrapped;
    entered = 1;
    if (setjmp(exit_jump) == 0) {
        result = entry(argc, argv);
        host_exit(result);
    }
    entered = 0;
    zzapp_api_ptr = 0;
    if (exit_code)
        *exit_code = captured_exit;
    return ZZ_OK;
}

int zzapp_host_map(const char *path, const zzapp_api *api,
                   int argc, char **argv, int *exit_code)
{
#if defined(_WIN32)
    (void)path;
    (void)api;
    (void)argc;
    (void)argv;
    (void)exit_code;
    return ZZ_ENOSYS;
#else
    void *handle;
    zzapp_host_main entry;
    int rc;

    if (!path)
        return ZZ_EINVAL;
    handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle)
        return ZZ_EIO;
    entry = (zzapp_host_main)dlsym(handle, "zz_main");
    if (!entry)
        return ZZ_ENOENT;
    rc = zzapp_host_enter(api, entry, argc, argv, exit_code);
    /* Keep the image mapped while the caller inspects app-owned pointers. */
    return rc;
#endif
}
