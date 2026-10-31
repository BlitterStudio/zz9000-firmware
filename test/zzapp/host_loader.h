/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZAPP_HOST_LOADER_H
#define ZZAPP_HOST_LOADER_H

#include "zzapp.h"

/* Host crt0: installs api, invokes main, and captures zz_exit's code. */
typedef int (*zzapp_host_main)(int argc, char **argv);
int zzapp_host_enter(const zzapp_api *api, zzapp_host_main entry,
                     int argc, char **argv, int *exit_code);

/* Maps a host-built shared object exporting zz_main against libzzapp. */
int zzapp_host_map(const char *path, const zzapp_api *api,
                   int argc, char **argv, int *exit_code);

#endif
