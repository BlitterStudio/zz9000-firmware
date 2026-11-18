/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "aar/app.h"

int aar_main(int argc, char **argv)
{
    static const char text[] = "mapped";
    (void)argc;
    (void)argv;
    aar_log(text, sizeof text - 1u);
    return 3;
}
