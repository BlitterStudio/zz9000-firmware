/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Operating System (ZZ9000OS)
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef WATCHDOG_H
#define WATCHDOG_H

int watchdog_init(void);
void watchdog_kick(void);

#endif
