/*
 * Logs one line and returns. crt0 turns that into zz_exit.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"

static const char hello_text[] = "hello zzapp\n";
const char *zzapp_hello_msg = hello_text;

int zz_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    zz_log(zzapp_hello_msg, sizeof hello_text - 1u);
    return 0;
}
