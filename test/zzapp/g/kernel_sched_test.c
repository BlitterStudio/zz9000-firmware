/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>

#include "zzapp_kernel.h"

static int failures;
static unsigned checks;

#define CHECK(x) do { \
	checks++; \
	if (!(x)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	} \
} while (0)

static void make_runnable(zzapp_kernel_t *kernel, unsigned slot)
{
	CHECK(zzapp_kernel_start_app(kernel, slot) == 0);
}

static void test_first_context_and_dispatch(void)
{
	zzapp_kernel_t kernel;
	unsigned slot;

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x12345678u,
				       0x23456789u, 3u, 0x34567890u,
				       0x45678000u) == 0);
	CHECK(kernel.threads[0].context.r[15] == 0x12345678u);
	CHECK(kernel.threads[0].context.r[0] == 0x23456789u);
	CHECK(kernel.threads[0].context.r[1] == 3u);
	CHECK(kernel.threads[0].context.r[2] == 0x34567890u);
	CHECK(kernel.threads[0].context.r[13] == 0x45678000u);
	CHECK(kernel.threads[0].context.cpsr == ZZAPP_KERNEL_APP_CPSR);
	CHECK(kernel.threads[0].context.fpscr == 0u);
	for (slot = 0; slot < ZZAPP_KERNEL_APP_SLOTS; slot++)
		make_runnable(&kernel, slot);
	CHECK(zzapp_kernel_dispatch(&kernel, ZZAPP_KERNEL_DISPATCH_TICK, 5u) ==
	      ZZAPP_KERNEL_APP0);
	CHECK(zzapp_kernel_dispatch(&kernel, ZZAPP_KERNEL_DISPATCH_TICK, 5u) ==
	      ZZAPP_KERNEL_APP1);
	CHECK(zzapp_kernel_dispatch(&kernel, ZZAPP_KERNEL_DISPATCH_TICK, 5u) ==
	      ZZAPP_KERNEL_APP2);
	CHECK(zzapp_kernel_dispatch(&kernel, ZZAPP_KERNEL_DISPATCH_TICK, 5u) ==
	      ZZAPP_KERNEL_APP3);
	zzapp_kernel_service_due(&kernel);
	CHECK(zzapp_kernel_dispatch(&kernel, ZZAPP_KERNEL_DISPATCH_DOORBELL, 0u) ==
	      ZZAPP_KERNEL_SYSTEM);
	CHECK(zzapp_kernel_dispatch(&kernel, ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u) ==
	      ZZAPP_KERNEL_APP0);
}

static void test_frame_switch(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame;
	zzapp_arm_frame_t *next;

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	CHECK(zzapp_kernel_prepare_app(&kernel, 1, 0x2000u, 2u, 0u, 0u,
				       0x9000u) == 0);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	frame = kernel.threads[0].context.frame;
	frame.exception_lr = 0x1004u;
	zzapp_kernel_set_current_thread(0);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(next == &kernel.threads[0].context.frame);
	CHECK(kernel.threads[0].context.r[15] == 0x1000u);
	CHECK(kernel.threads[0].context.r[13] == 0x8000u);
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(next == &kernel.threads[1].context.frame);
	CHECK(zzapp_kernel_current_thread() == 1);
}

static void test_system_frame_runs_service_before_apps(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t system_frame = { 0 };
	zzapp_arm_frame_t *next;

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	system_frame.exception_lr = 0x5000u;
	system_frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &system_frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(next == &kernel.threads[0].context.frame);
	zzapp_kernel_service_due(&kernel);
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_DOORBELL, 0u);
	CHECK(next == &kernel.system_context.frame);
	CHECK(zzapp_kernel_current_thread() == -1);
}

static void test_round_robin_quantum(void)
{
	zzapp_kernel_t kernel;
	unsigned i;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	for (i = 0; i < 400; i++)
		(void)zzapp_kernel_schedule(&kernel, 5);
	CHECK(kernel.threads[0].user_ticks == kernel.threads[1].user_ticks);
	CHECK(kernel.threads[0].user_ticks == 1000u);
}

static void test_service_precedes_app_and_preserves_remainder(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_schedule(&kernel, 2) == ZZAPP_KERNEL_APP0);
	zzapp_kernel_service_due(&kernel);
	CHECK(zzapp_kernel_schedule(&kernel, 1) == ZZAPP_KERNEL_SYSTEM);
	CHECK(zzapp_kernel_schedule(&kernel, 3) == ZZAPP_KERNEL_APP0);
	CHECK(kernel.threads[0].quantum_left == 0u);
}

static void test_block_wake_and_idle(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 5) == ZZAPP_KERNEL_IDLE);
	CHECK(zzapp_kernel_wake(&kernel, 0, ZZAPP_WAKE_CANCELLED) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 5) == ZZAPP_KERNEL_APP0);
	CHECK(kernel.threads[0].wake_reason == ZZAPP_WAKE_CANCELLED);
}

static void test_sleep_deadline_order(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	CHECK(zzapp_kernel_sleep(&kernel, 1, 10) == 0);
	CHECK(zzapp_kernel_sleep(&kernel, 0, 10) == 0);
	zzapp_kernel_advance(&kernel, 10);
	CHECK(kernel.threads[1].state == ZZAPP_THREAD_RUNNABLE);
	CHECK(kernel.threads[0].state == ZZAPP_THREAD_RUNNABLE);
	CHECK(kernel.wake_log_count == 2u);
	CHECK(kernel.wake_log[0] == 1u);
	CHECK(kernel.wake_log[1] == 0u);
	CHECK(zzapp_kernel_schedule(&kernel, 5) != ZZAPP_KERNEL_IDLE);
}

static void test_wait_completion_timeout_and_cancel(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	CHECK(zzapp_kernel_wake(&kernel, 0, ZZAPP_WAKE_READY) == 0);
	CHECK(zzapp_kernel_wait_status(&kernel, 0) == 0);
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	CHECK(zzapp_kernel_wake(&kernel, 0, ZZAPP_WAKE_TIMEOUT) == 0);
	CHECK(zzapp_kernel_wait_status(&kernel, 0) ==
	      ZZAPP_KERNEL_WAIT_TIMEOUT);
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	CHECK(zzapp_kernel_wake(&kernel, 0, ZZAPP_WAKE_CANCELLED) == 0);
	CHECK(zzapp_kernel_wait_status(&kernel, 0) ==
	      ZZAPP_KERNEL_WAIT_CANCELLED);
}

static void test_accounting_and_throttle(void)
{
	zzapp_kernel_t kernel;
	unsigned i;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_schedule(&kernel, 2) == ZZAPP_KERNEL_APP0);
	zzapp_kernel_charge_svc(&kernel, 0, 3);
	CHECK(kernel.threads[0].user_ticks + kernel.threads[0].svc_ticks == 5u);
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	(void)zzapp_kernel_schedule(&kernel, 5);
	CHECK(kernel.threads[0].user_ticks + kernel.threads[0].svc_ticks == 5u);

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	for (i = 0; i < 200; i++) {
		zzapp_kernel_service_due(&kernel);
		(void)zzapp_kernel_schedule(&kernel, 5);
	}
	CHECK(kernel.threads[0].user_ticks >= 10u);
	CHECK(kernel.overload_count != 0u);
	CHECK(kernel.service_ticks > kernel.threads[0].user_ticks);
	CHECK(zzapp_kernel_schedule(&kernel, 0) != ZZAPP_KERNEL_IDLE);
}

static void test_blocking_api_lock_and_slot_resources(void)
{
	zzapp_kernel_t kernel;
	unsigned i;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) != 0);
	CHECK(zzapp_kernel_api_block(&kernel, 0) == 0);
	CHECK(kernel.api_lock_owner == -1);
	CHECK(kernel.threads[0].state == ZZAPP_THREAD_BLOCKED);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) == 0);
	for (i = 0; i < 20; i++)
		(void)zzapp_kernel_schedule(&kernel, 5);
	CHECK(kernel.threads[0].user_ticks == 0u);
	CHECK(zzapp_kernel_wake(&kernel, 0, ZZAPP_WAKE_READY) == 0);
	CHECK(zzapp_kernel_api_unlock(&kernel, 1) == 0);
	CHECK(kernel.threads[0].wake_reason == ZZAPP_WAKE_READY);

	CHECK(zzapp_kernel_surface_bind(&kernel, 0, 11u, 0x200000u,
					 0x2000u) == 0);
	CHECK(zzapp_kernel_surface_bind(&kernel, 1, 12u, 0x201000u,
					 0x1000u) != 0);
	CHECK(zzapp_kernel_surface_bind(&kernel, 1, 12u, 0x202000u,
					 0x1000u) == 0);
	CHECK(zzapp_kernel_audio_open(&kernel, 0, 1) == 0);
	CHECK(zzapp_kernel_audio_open(&kernel, 1, 1) == 0);
	CHECK(zzapp_kernel_audio_open(&kernel, 2, 1) != 0);
	zzapp_kernel_audio_close(&kernel, 1);
	CHECK(zzapp_kernel_audio_open(&kernel, 2, 1) == 0);

	CHECK(zzapp_kernel_gl_bind(&kernel, 0, 11u) == 0);
	CHECK(zzapp_kernel_gl_bind(&kernel, 1, 12u) != 0);
	zzapp_kernel_stop_app(&kernel, 0);
	CHECK(zzapp_kernel_gl_bind(&kernel, 1, 12u) == 0);
	CHECK(kernel.resources[1].surface_bytes == 0x1000u);
	for (i = 0; i < ZZAPP_KERNEL_SESSION_LIMIT; i++)
		CHECK(zzapp_kernel_session_open(&kernel, 1, 0) == 0);
	CHECK(zzapp_kernel_session_open(&kernel, 1, 0) != 0);
	for (i = 0; i < ZZAPP_KERNEL_SESSION_LIMIT; i++)
		CHECK(zzapp_kernel_session_open(&kernel, 1, 1) == 0);
	CHECK(zzapp_kernel_session_open(&kernel, 1, 1) != 0);
	zzapp_kernel_stop_app(&kernel, 1);
	CHECK(kernel.resources[1].surface_bytes == 0u);
	CHECK(kernel.resources[1].image_sessions == 0u);
	CHECK(kernel.resources[1].audio_sessions == 0u);
}

static void test_waiter_isolation_timeout_and_stop(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 1);
	make_runnable(&kernel, 2);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) == 0);
	CHECK(zzapp_kernel_api_block(&kernel, 1) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 5) == ZZAPP_KERNEL_APP2);
	CHECK(zzapp_kernel_wake(&kernel, 1, ZZAPP_WAKE_READY) == 0);
	CHECK(kernel.wake_log_count == 1u);
	CHECK(kernel.wake_log[0] == 1u);
	CHECK(kernel.threads[2].state == ZZAPP_THREAD_RUNNABLE);

	CHECK(zzapp_kernel_sleep(&kernel, 1, kernel.now + 10u) == 0);
	zzapp_kernel_advance(&kernel, 10u);
	CHECK(zzapp_kernel_wait_status(&kernel, 1) == ZZAPP_KERNEL_WAIT_TIMEOUT);

	CHECK(zzapp_kernel_block(&kernel, 1) == 0);
	zzapp_kernel_stop_app(&kernel, 1);
	CHECK(zzapp_kernel_wait_status(&kernel, 1) == ZZAPP_KERNEL_WAIT_CANCELLED);
}

static void test_system_ticks_wake_sleepers(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t system_frame = { 0 };
	zzapp_arm_frame_t *next;

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_sleep(&kernel, 0, 10u) == 0);
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &system_frame,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(next == &kernel.system_context.frame);
	CHECK(kernel.now == 5u);
	CHECK(kernel.threads[0].state == ZZAPP_THREAD_SLEEPING);
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(kernel.now == 10u);
	CHECK(next == &kernel.threads[0].context.frame);
	CHECK(kernel.threads[0].state == ZZAPP_THREAD_RUNNABLE);
}

int main(void)
{
    test_first_context_and_dispatch();
    test_frame_switch();
    test_system_frame_runs_service_before_apps();
    test_round_robin_quantum();
    test_service_precedes_app_and_preserves_remainder();
    test_block_wake_and_idle();
    test_sleep_deadline_order();
    test_wait_completion_timeout_and_cancel();
    test_accounting_and_throttle();
    test_blocking_api_lock_and_slot_resources();
    test_waiter_isolation_timeout_and_stop();
    test_system_ticks_wake_sleepers();
    printf("kernel_sched_test: %u checks, %d failures\n", checks, failures);
    return failures != 0;
}
