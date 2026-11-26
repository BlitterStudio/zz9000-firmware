/*
 * libaar reads the aar_msg_wait slot only when the table's size covers
 * it, so a table cut short before it gives
 * the app AAR_E_NOSYS, and so does a covered slot that is NULL.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "aar/app.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static uint32_t seen_timeout;
static int calls;

static int mock_wait(uint32_t timeout_us)
{
    seen_timeout = timeout_us;
    calls++;
    return AAR_E_TIMEDOUT;
}

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

int main(void)
{
    static aar_api table;

    aar_api_ptr = NULL;
    CHECK(aar_msg_wait(10u) == AAR_E_NOSYS);

    /* A table that ends before msg_wait does not cover the slot. */
    memset(&table, 0, sizeof table);
    table.size = (uint32_t)offsetof(aar_api, msg_wait);
    table.version = AAR_API_VERSION;
    table.msg_wait = mock_wait;
    aar_api_ptr = &table;
    CHECK(!AAR_API_COVERS(&table, msg_wait));
    CHECK(aar_msg_wait(10u) == AAR_E_NOSYS);
    CHECK(calls == 0);

    /* A table that covers it passes the timeout to the firmware. */
    table.size = (uint32_t)sizeof table;
    table.version = AAR_API_VERSION;
    CHECK(AAR_API_COVERS(&table, msg_wait));
    CHECK(aar_msg_wait(123456u) == AAR_E_TIMEDOUT);
    CHECK(calls == 1 && seen_timeout == 123456u);

    /* A covered NULL slot is still AAR_E_NOSYS. */
    table.msg_wait = NULL;
    CHECK(aar_msg_wait(10u) == AAR_E_NOSYS);
    printf("msg_wait_wrapper_test: ok\n");
    return 0;
}
