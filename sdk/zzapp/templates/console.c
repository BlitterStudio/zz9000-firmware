/*
 * Shell console template: stdout, stderr, stdin and break events.
 * Prints a banner, then echoes each stdin line in upper case until EOF
 * (Ctrl-\ in a Shell window) and reports any Ctrl-D/E/F seen meanwhile.
 *
 * SPDX-License-Identifier: MIT
 */
#include <ctype.h>
#include <stdio.h>

#include "zzapp.h"

static void report_breaks(void)
{
    zzapp_event events[8];
    uint32_t count = 0;
    uint32_t i;

    if (zz_input_poll(events, 8u, &count) != ZZ_OK)
        return;
    for (i = 0; i < count; i++)
        if (events[i].type == ZZ_EV_BREAK)
            printf("break: Ctrl-%c\n", 'A' + events[i].code - 1);
}

int zz_main(int argc, char **argv)
{
    char line[256];
    unsigned lines = 0;
    int i;

    printf("console: %d argument(s)", argc - 1);
    for (i = 1; i < argc; i++)
        printf(" [%s]", argv[i]);
    printf("\n");
    fprintf(stderr, "console: stderr works\n");
    fflush(stdout);
    while (!zz_should_stop() && fgets(line, sizeof line, stdin)) {
        char *p;

        for (p = line; *p; p++)
            *p = (char)toupper((unsigned char)*p);
        fputs(line, stdout);
        fflush(stdout);
        lines++;
        report_breaks();
    }
    report_breaks();
    printf("console: EOF after %u line(s)\n", lines);
    return 0;
}
