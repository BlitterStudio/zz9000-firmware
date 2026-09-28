/*
 * Source-order test for the boot-time Fast-RAM advertisement gate
 * (plan fast-ram-cfg U2). The gate write in main() must come after
 * the bounded CFG load and be driven by the fail-closed decision;
 * the old unconditional early write must not return.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define MAIN_SRC "../../ZZ9000_proto.sdk/ZZ9000OS/src/main.c"

int main(void) {
    static char buf[192 * 1024];
    FILE *f = fopen(MAIN_SRC, "rb");
    size_t n;

    if (!f) {
        printf("FAIL cannot open " MAIN_SRC "\n");
        return 1;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;

    const char *load = strstr(buf,
        "zz_config_load_fastram(ZZ_CONFIG_FASTRAM_DEADLINE_MS);");
    const char *gate = strstr(buf,
        "zz_config_fastram_advertise() ? 1 : 0");

    CHECK(load != NULL);
    CHECK(gate != NULL);
    /* the decision is made (bounded load) before the gate is written */
    CHECK(load != NULL && gate != NULL && load < gate);
    /* the unconditional early write (issue #25's original form) is gone */
    CHECK(strstr(buf, "MNTZORRO_REG6, 1);") == NULL);

    if (failures) {
        printf("%d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("boot-order source checks passed (%d)\n", checks);
    return 0;
}
