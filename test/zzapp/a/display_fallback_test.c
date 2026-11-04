#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "zzapp.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"
#include "zzapp_services.h"

int main(void)
{
    uint8_t window[ZZAPP_WINDOW_BYTES];
    uint8_t request[ZZAPP_BIND_REQ_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_len = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    uint32_t format = 0;
    uint32_t front = 0;
    uint32_t back = 0;

    memset(window, 0, sizeof(window));
    zzapp_boot_init();
    zzapp_services_test_reset();
    zzapp_services_test_set_window(window, sizeof(window));
    assert(zzapp_loader_begin() == ZZ_OK);

    zzapp_build_display_bind_request(request, 320U, 200U, 640U,
                                     ZZ_FMT_RGB565, 0x00200000U, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, request,
                                  sizeof(request), reply, &reply_len,
                                  1, 0U, 0U) == SDK_STATUS_OK);
    assert(zzapp_display_bound(&width, &height, &pitch, &format, &front,
                               &back) == ZZ_OK);
    assert(width == 320U && height == 200U && pitch == 640U);
    assert(format == ZZ_FMT_RGB565 && front == 0x00200000U);
    assert(back != 0U);

    zzapp_services_on_app_end();
    zzapp_build_display_bind_request(request, 320U, 200U, 640U,
                                     ZZ_FMT_RGB565, 0x00200000U, 0x00220000U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_DISPLAY_BIND, request,
                                  sizeof(request), reply, &reply_len,
                                  1, 0U, 0U) == SDK_STATUS_OK);
    assert(zzapp_display_flip_idle());
    assert(zzapp_svc_display_flip_async(1U) == ZZ_OK);
    assert(!zzapp_display_flip_idle());
    assert(zzapp_svc_display_flip_async(1U) == ZZ_EBUSY);
    zzapp_put_be32(window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLIP_ACK_OFF,
                   zzapp_get_be32(window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLIP_GEN_OFF));
    assert(zzapp_display_flip_idle());
    assert(zzapp_display_bound(NULL, NULL, NULL, NULL, &front, &back) == ZZ_OK);
    assert(front == 0x00220000U && back == 0x00200000U);
    zzapp_services_on_app_end();
    return 0;
}
