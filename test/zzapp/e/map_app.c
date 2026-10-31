/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zzapp.h"

int zz_main(int argc, char **argv)
{
    static const char text[] = "mapped";
    (void)argc;
    (void)argv;
    zz_log(text, sizeof text - 1u);
    return 3;
}
