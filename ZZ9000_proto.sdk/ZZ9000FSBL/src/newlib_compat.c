/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal newlib compatibility shim, same pattern as the ZZ9000OS one:
 * libxil.a (standalone_v6_8 xil_printf) references __ctype_ptr__,
 * which current newlib no longer exports.
 */

#include <ctype.h>

char *__ctype_ptr__ __attribute__((weak)) = (char *)_ctype_;
