/*
 * Deliberately faults after proving the app log path.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"

int zz_main(int argc, char **argv)
{
    volatile unsigned int *const null_word = (volatile unsigned int *)0;

    (void)argc;
    (void)argv;
    zz_log("fault zzapp: storing to address zero\n",
           sizeof "fault zzapp: storing to address zero\n" - 1u);
    *null_word = 0u;
    return 1;
}
