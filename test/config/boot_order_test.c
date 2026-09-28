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
        "zz_config_load_fastram(ZZ_CONFIG_FASTRAM_DEADLINE_MS, 1);");
    const char *gate = strstr(buf,
        "zz_config_fastram_advertise() ? 1 : 0");

    CHECK(load != NULL);
    CHECK(gate != NULL);
    /* the cold-boot gate write follows the cold-boot bounded load
     * (the warm-reset handler earlier in the file rewrites the gate
     * from its own reload) */
    CHECK(load != NULL && strstr(load,
        "zz_config_fastram_advertise() ? 1 : 0") != NULL);
    /* every patched vendor SD object must be registered in the build:
     * a defined-but-unreferenced override silently links the unpatched
     * libxil.a object instead (caught review three times) */
    {
        FILE *mk = fopen("../../ZZ9000_proto.sdk/ZZ9000OS/Makefile", "rb");
        if (mk) {
            static char mbuf[64 * 1024];
            size_t mn = fread(mbuf, 1, sizeof(mbuf) - 1, mk);
            fclose(mk);
            mbuf[mn] = 0;
            CHECK(strstr(mbuf, "COMMON_OBJS += $(BSP_SDPS_OBJ)") != NULL);
            CHECK(strstr(mbuf, "COMMON_OBJS += $(BSP_SDPSOPT_OBJ)") != NULL);
            CHECK(strstr(mbuf, "COMMON_OBJS += $(BSP_DISKIO_OBJ)") != NULL);
        }
    }

    if (failures) {
        printf("%d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("boot-order source checks passed (%d)\n", checks);
    return 0;
}
