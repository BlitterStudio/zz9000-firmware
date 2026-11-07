/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zzm_runtime.h"

typedef void (*zzm_ctor_fn)(void);

/* The firmware linker script supplies these zero-length ranges.  The host
 * harness uses equal start/end symbols so libzzm_rt follows the same entry
 * wrapper without introducing host CRT constructors into the module. */
zzm_ctor_fn zzm_host_empty_array[1];
__asm__(".globl __zzm_preinit_array_start\n"
        "__zzm_preinit_array_start = zzm_host_empty_array\n"
        ".globl __zzm_preinit_array_end\n"
        "__zzm_preinit_array_end = zzm_host_empty_array\n"
        ".globl __zzm_init_array_start\n"
        "__zzm_init_array_start = zzm_host_empty_array\n"
        ".globl __zzm_init_array_end\n"
        "__zzm_init_array_end = zzm_host_empty_array\n"
        ".globl __zzm_fini_array_start\n"
        "__zzm_fini_array_start = zzm_host_empty_array\n"
        ".globl __zzm_fini_array_end\n"
        "__zzm_fini_array_end = zzm_host_empty_array\n");
