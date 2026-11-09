/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zzapp_kernel.h"
#include "zzapp_runtime.h"

zzapp_kernel_t *zzapp_runtime_kernel(void);

/* An ordinary API call must return to the same app with its frame intact
 * and its quantum unspent; a call that leaves the app blocked hands the
 * core to the system thread. */
int main(void)
{
    zzapp_kernel_t *kernel;
    zzapp_arm_frame_t frame;
    zzapp_arm_frame_t saved;
    uint32_t quantum;

    zzapp_boot_init();
    kernel = zzapp_runtime_kernel();
    assert(kernel != NULL);
    assert(zzapp_kernel_prepare_app(kernel, 0u, 0x8000u, 0x9000u, 0u, 0u,
                                    0xc0100000u) == 0);
    zzapp_kernel_set_current_thread(0);
    kernel->threads[0].state = ZZAPP_THREAD_RUNNABLE;
    quantum = kernel->threads[0].quantum_left = 7u;

    memset(&frame, 0x5a, sizeof(frame));
    frame.r[0] = 42u;
    saved = frame;
    assert(zzapp_kernel_svc_return(&frame) == &frame);
    assert(memcmp(&frame, &saved, sizeof(frame)) == 0);
    assert(zzapp_kernel_current_thread() == 0);
    assert(kernel->threads[0].quantum_left == quantum);

    /* On the card the system thread has always parked a context by now. */
    memset(&kernel->system_context, 0, sizeof(kernel->system_context));
    kernel->system_context.r[0] = 99u;
    kernel->system_context_valid = 1u;
    kernel->threads[0].state = ZZAPP_THREAD_BLOCKED;
    assert(zzapp_kernel_svc_return(&frame) == &frame);
    assert(zzapp_kernel_current_thread() == ZZAPP_KERNEL_SYSTEM);
    assert(frame.r[0] == 99u);
    /* The blocked app's registers were saved for its later resume. */
    assert(kernel->threads[0].context.r[0] == 42u);

    printf("svc_return_test: ok\n");
    return 0;
}
