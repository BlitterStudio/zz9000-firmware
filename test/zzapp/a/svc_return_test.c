/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zzapp_kernel.h"
#include "zzapp_runtime.h"

zzapp_kernel_t *zzapp_runtime_kernel(void);
zzapp_arm_frame_t *zzapp_kernel_arch_dispatch(zzapp_arm_frame_t *frame,
                                               uint32_t source,
                                               uint32_t return_adjust);

static void test_banked_exception_stacks_keep_app_context(void)
{
    zzapp_kernel_t *kernel;
    zzapp_arm_frame_t svc_stack = { 0 };
    zzapp_arm_frame_t irq_stack = { 0 };
    zzapp_arm_frame_t *svc_return;
    zzapp_arm_frame_t *app_return;

    zzapp_boot_init();
    kernel = zzapp_runtime_kernel();
    assert(zzapp_kernel_prepare_app(kernel, 0u, 0x8000u, 0xa1u, 0u, 0u,
                                    0xc0100000u) == 0);
    assert(zzapp_kernel_start_app(kernel, 0u) == 0);
    zzapp_kernel_set_current_thread(ZZAPP_KERNEL_SYSTEM);

    svc_return = zzapp_kernel_arch_dispatch(
        &svc_stack, ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
    assert(svc_return == &svc_stack);
    assert(svc_return->r[0] == 0xa1u);

    irq_stack.r[0] = 0xa1u;
    irq_stack.exception_lr = 0x8004u;
    (void)zzapp_kernel_arch_dispatch(&irq_stack,
                                     ZZAPP_KERNEL_DISPATCH_DOORBELL, 4u);
    assert(zzapp_kernel_current_thread() == ZZAPP_KERNEL_SYSTEM);

    svc_return->r[0] = 0x5a5au;
    app_return = zzapp_kernel_arch_dispatch(
        svc_return, ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
    assert(app_return == svc_return);
    assert(app_return->r[0] == 0xa1u);
}


/* An ordinary API call must return to the same app with its frame intact
 * and its quantum unspent; a call that leaves the app blocked hands the
 * core to the system thread. */
int main(void)
{
    zzapp_kernel_t *kernel;
    zzapp_arm_frame_t frame;
    zzapp_arm_frame_t saved;
    zzapp_arm_frame_t *restored;
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
    kernel->system_context.frame.r[0] = 99u;
    kernel->system_context_valid = 1u;
    kernel->threads[0].state = ZZAPP_THREAD_BLOCKED;
    restored = zzapp_kernel_svc_return(&frame);
    assert(restored == &frame);
    assert(restored->r[0] == 99u);
    /* The blocked app's registers were saved for its later resume. */
    assert(kernel->threads[0].context.frame.r[0] == 42u);
    test_banked_exception_stacks_keep_app_context();

    printf("svc_return_test: ok\n");
    return 0;
}
