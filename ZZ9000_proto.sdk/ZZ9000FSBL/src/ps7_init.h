/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * fsbl.h includes "ps7_init.h". The PS init code vendored from the
 * Vivado build (util/refresh_ps7_init.sh) is the GPL-licensed variant
 * ps7_init_gpl.c/h — license-compatible with this GPLv3 repo and
 * API-identical (ps7_init, ps7_post_config, getPS7MessageInfo,
 * PS7_INIT_* codes). This shim maps the name fsbl.h expects.
 */
#ifndef ZZ9000_FSBL_PS7_INIT_SHIM_H
#define ZZ9000_FSBL_PS7_INIT_SHIM_H

#include "ps7_init_gpl.h"

#endif
