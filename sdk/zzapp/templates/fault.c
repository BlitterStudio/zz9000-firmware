/*
 * Deliberately faults after proving the app log path.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"

int zz_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    zz_log("fault zzapp: executing UDF\n",
           sizeof "fault zzapp: executing UDF\n" - 1u);
    /* UDF is architecturally undefined on ARMv7-A, independent of the
     * firmware's writable mapping at address zero. */
    __asm__ volatile (".word 0xe7f000f0" ::: "memory");
    return 1;
}
