/*
 * zz_msg_wait is the first appended slot of API 0.4. libzzapp reads it only
 * when the table covers it, so an app built against 0.4 still runs on 0.3
 * firmware and sees ZZ_ENOSYS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "zzapp.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static uint32_t seen_timeout;
static int calls;

static int mock_wait(uint32_t timeout_us)
{
    seen_timeout = timeout_us;
    calls++;
    return ZZ_ETIMEDOUT;
}

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

int main(void)
{
    static zzapp_api table;

    CHECK(ZZAPP_API_MINOR == 4u);
    CHECK(ZZAPP_API_VERSION == 4u);

    zzapp_api_ptr = NULL;
    CHECK(zz_msg_wait(10u) == ZZ_ENOSYS);

    /* A 0.3 table ends after heap_info; the slot is not covered. */
    memset(&table, 0, sizeof table);
    table.size = (uint32_t)offsetof(zzapp_api, msg_wait);
    table.version = 3u;
    table.msg_wait = mock_wait;
    zzapp_api_ptr = &table;
    CHECK(!ZZ_API_COVERS(&table, msg_wait));
    CHECK(zz_msg_wait(10u) == ZZ_ENOSYS);
    CHECK(calls == 0);

    /* A 0.4 table covers it and the timeout reaches the firmware. */
    table.size = (uint32_t)sizeof table;
    table.version = ZZAPP_API_VERSION;
    CHECK(ZZ_API_COVERS(&table, msg_wait));
    CHECK(zz_msg_wait(123456u) == ZZ_ETIMEDOUT);
    CHECK(calls == 1 && seen_timeout == 123456u);

    /* A covered NULL slot is still ZZ_ENOSYS. */
    table.msg_wait = NULL;
    CHECK(zz_msg_wait(10u) == ZZ_ENOSYS);
    printf("msg_wait_wrapper_test: ok\n");
    return 0;
}
