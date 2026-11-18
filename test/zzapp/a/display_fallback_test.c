/*
 * The display of an app whose client names no back buffer: the runtime
 * provides one, and a flip is complete when the client completes it (a client
 * that does not acknowledge scanout). The client is mock_client: the
 * DISPLAY_OPEN completion carries the board tokens of the client's buffers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "aar/app.h"
#include "mock_client.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"
#include "zzapp_services.h"

#define FRONT 0x00200000U
#define BACK  0x00220000U
#define TOKEN(phys) ((uint32_t)(phys) - MOCK_HEAP_TOKEN_ADJUST)

static mock_client client;

static void client_yield(void)
{
    (void)mock_client_serve(&client);
    zzapp_services_poll();
}

/* The display open, with its status reported when it fails. */
static void open_ok(aar_surface *surface)
{
    int rc = zzapp_svc_display_open(320U, 200U, AAR_FMT_RGB565, 0U, surface);

    if (rc != AAR_OK)
        fprintf(stderr, "display open: %d\n", rc);
    assert(rc == AAR_OK);
}

int main(void)
{
    zzapp_xport_env_t env;
    zzapp_xport_buf_t pair;
    aar_surface surface;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    uint32_t format = 0;
    uint32_t front = 0;
    uint32_t back = 0;
    uint32_t pid;

    zzapp_boot_init();
    zzapp_services_test_reset();
    mock_heap_reset();
    env = mock_heap_env();
    zzapp_xport_init(&env);
    pid = zzapp_test_admit();
    assert(pid != ZZAPP_PID_NONE);
    (void)zzapp_as_create(&zzapp_rec_by_pid(pid)->proc.as, pid);
    assert(zzapp_xport_block_get(zzapp_rec_by_pid(pid), ZZAPP_XPORT_BLOCK_PAIR,
                                 &pair) == 1);
    mock_client_init(&client);
    assert(mock_client_attach(&client, pair.token) == AAR_OK);
    zzapp_services_test_set_yield(client_yield);
    assert(zzapp_loader_begin() == AAR_OK);

    /* No back buffer: the client names the front only. */
    client.display_front = TOKEN(FRONT);
    client.display_back = 0U;
    memset(&surface, 0, sizeof(surface));
    open_ok(&surface);
    assert(zzapp_display_bound(&width, &height, &pitch, &format, &front,
                               &back) == AAR_OK);
    assert(width == 320U && height == 200U && pitch == 640U);
    assert(format == AAR_FMT_RGB565 && front == FRONT);
    assert(back != 0U);

    zzapp_services_on_app_end();
    assert(pid == zzapp_services_current_pid());

    /* The same display with both buffers named by the client. */
    zzapp_boot_init();
    zzapp_services_test_reset();
    mock_heap_reset();
    env = mock_heap_env();
    zzapp_xport_init(&env);
    pid = zzapp_test_admit();
    assert(pid != ZZAPP_PID_NONE);
    (void)zzapp_as_create(&zzapp_rec_by_pid(pid)->proc.as, pid);
    assert(zzapp_xport_block_get(zzapp_rec_by_pid(pid), ZZAPP_XPORT_BLOCK_PAIR,
                                 &pair) == 1);
    mock_client_init(&client);
    assert(mock_client_attach(&client, pair.token) == AAR_OK);
    zzapp_services_test_set_yield(client_yield);
    assert(zzapp_loader_begin() == AAR_OK);
    client.display_front = TOKEN(FRONT);
    client.display_back = TOKEN(BACK);
    memset(&surface, 0, sizeof(surface));
    open_ok(&surface);
    assert(zzapp_display_flip_idle());
    /* The client holds the flip: it is pending, and a second one is busy. */
    client.delay[AAR_OP_DISPLAY_FLIP] = 1000U;
    assert(zzapp_svc_display_flip_async(1U) == AAR_OK);
    assert(!zzapp_display_flip_idle());
    assert(zzapp_svc_display_flip_async(1U) == AAR_E_BUSY);
    /* Its completion, with no SUBMITTED flag, is the acknowledgement. */
    client.delay[AAR_OP_DISPLAY_FLIP] = 0U;
    client_yield();
    assert(zzapp_display_flip_idle());
    assert(zzapp_display_bound(NULL, NULL, NULL, NULL, &front, &back) == AAR_OK);
    assert(front == BACK && back == FRONT);
    zzapp_services_on_app_end();
    zzapp_services_test_set_yield(NULL);
    return 0;
}
