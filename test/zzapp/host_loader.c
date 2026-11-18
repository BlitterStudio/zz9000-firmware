/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_loader.h"
#include "aar_internal.h"

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

int zzapp_host_enter(const aar_api *api, zzapp_host_main entry,
                     int argc, char **argv, int *exit_code)
{
    aar_api wrapped;
    int result;

    if (!api || !entry)
        return AAR_E_INVAL;
    wrapped = *api;
    wrapped.exit = host_exit;
    aar_crt0_bind(&wrapped);
    entered = 1;
    if (setjmp(exit_jump) == 0) {
        result = entry(argc, argv);
        host_exit(result);
    }
    entered = 0;
    aar_crt0_bind(0);
    if (exit_code)
        *exit_code = captured_exit;
    return AAR_OK;
}

int zzapp_host_map(const char *path, const aar_api *api,
                   int argc, char **argv, int *exit_code)
{
#if defined(_WIN32)
    (void)path;
    (void)api;
    (void)argc;
    (void)argv;
    (void)exit_code;
    return AAR_E_NOSYS;
#else
    void *handle;
    zzapp_host_main entry;
    int rc;

    if (!path)
        return AAR_E_INVAL;
    handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle)
        return AAR_E_IO;
    entry = (zzapp_host_main)dlsym(handle, "aar_main");
    if (!entry)
        return AAR_E_NOENT;
    rc = zzapp_host_enter(api, entry, argc, argv, exit_code);
    /* Keep the image mapped while the caller inspects app-owned pointers. */
    return rc;
#endif
}
