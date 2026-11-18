/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZAPP_HOST_LOADER_H
#define ZZAPP_HOST_LOADER_H

#include "aar/app.h"

/* Host crt0: installs api, invokes main, and captures aar_exit's code. */
typedef int (*zzapp_host_main)(int argc, char **argv);
int zzapp_host_enter(const aar_api *api, zzapp_host_main entry,
                     int argc, char **argv, int *exit_code);

/* Maps a host-built shared object exporting aar_main against libaar. */
int zzapp_host_map(const char *path, const aar_api *api,
                   int argc, char **argv, int *exit_code);

#endif
