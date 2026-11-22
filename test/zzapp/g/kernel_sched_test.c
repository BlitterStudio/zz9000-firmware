/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "zzapp_kernel.h"
#include "zzapp_rec.h"

static unsigned irq_save_calls;
static unsigned irq_restore_calls;
static uint64_t clock_ms;
static int test_irqs_masked;
static void (*test_pending_irq_fn)(void);
static void (*test_clock_hook)(void);

static unsigned wake_events;

void zzapp_test_kernel_wake_event(void)
{
	wake_events++;
}

uint64_t zzapp_kernel_now_us(void)
{
	if (test_clock_hook)
		test_clock_hook();
	return clock_ms * 1000U;
}

uint32_t smp_local_irq_save(void)
{
	uint32_t prior = (uint32_t)test_irqs_masked;
	irq_save_calls++;
	test_irqs_masked = 1;
	return prior;
}

void smp_local_irq_restore(uint32_t state)
{
	irq_restore_calls++;
	test_irqs_masked = (int)state;
	if (!test_irqs_masked && test_pending_irq_fn) {
		void (*fn)(void) = test_pending_irq_fn;
		test_pending_irq_fn = NULL;
		fn();
	}
}

static void test_request_irq(void (*fn)(void))
{
	if (!test_irqs_masked) {
		fn();
	} else {
		test_pending_irq_fn = fn;
	}
}

static int failures;
static unsigned checks;

#define CHECK(x) do { \
	checks++; \
	if (!(x)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	} \
} while (0)

/*
 * The scheduler runs on process records: the thread is the first member of a
 * record and the per-record kernel words follow it. The old scenarios use
 * the first four records, as they used four fixed slots.
 */
#define TEST_RECS 4u
#define TEST_MAX_RECS 300u
static zzapp_rec_t g_recs[TEST_MAX_RECS];

/* The pids of the first four records, named as the old fixed slots were. */
enum {
	ZZAPP_KERNEL_APP0 = 0, ZZAPP_KERNEL_APP1, ZZAPP_KERNEL_APP2,
	ZZAPP_KERNEL_APP3
};

#define THREAD(k, pid) (*zzapp_kernel_thread(&(k), (pid)))
#define WORDS(pid) (g_recs[pid].kernel)

static void kinit_n(zzapp_kernel_t *kernel, uint32_t records)
{
	CHECK(zzapp_kernel_init(kernel, g_recs, sizeof(g_recs[0]), records) == 0);
}

static void kinit(zzapp_kernel_t *kernel)
{
	kinit_n(kernel, TEST_RECS);
}

#if ZZAPP_PERF_COUNTERS
/* Drop every pending perf report request, in the records and in the counts. */
static void clear_perf_requests(zzapp_kernel_t *kernel)
{
	uint32_t pid;

	for (pid = 0U; pid < zzapp_kernel_capacity(kernel); pid++) {
		WORDS(pid).perf_report_pending = 0U;
		WORDS(pid).perf_report_force = 0U;
	}
	kernel->perf.report_pending = 0U;
	kernel->perf.report_force = 0U;
}
#endif

static void make_runnable(zzapp_kernel_t *kernel, unsigned slot)
{
	CHECK(zzapp_kernel_start_app(kernel, slot) == 0);
}

static void test_first_context_and_dispatch(void)
{
	zzapp_kernel_t kernel;
	unsigned slot;

	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x12345678u,
				       0x23456789u, 3u, 0x34567890u,
				       0x45678000u) == 0);
	CHECK(THREAD(kernel, 0).context.frame.exception_lr == 0x12345678u);
	CHECK(THREAD(kernel, 0).context.frame.r[0] == 0x23456789u);
	CHECK(THREAD(kernel, 0).context.frame.r[1] == 3u);
	CHECK(THREAD(kernel, 0).context.frame.r[2] == 0x34567890u);
	CHECK(THREAD(kernel, 0).context.frame.user_sp == 0x45678000u);
	CHECK(THREAD(kernel, 0).context.frame.spsr == ZZAPP_KERNEL_APP_CPSR);
	CHECK(THREAD(kernel, 0).context.frame.fpscr == 0u);
	for (slot = 0; slot < TEST_RECS; slot++)
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

	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	CHECK(zzapp_kernel_prepare_app(&kernel, 1, 0x2000u, 2u, 0u, 0u,
				       0x9000u) == 0);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	frame = THREAD(kernel, 0).context.frame;
	frame.exception_lr = 0x1004u;
	zzapp_kernel_set_current_thread(0);
	clock_ms = 0;
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(next == &frame);
	CHECK(THREAD(kernel, 0).context.frame.exception_lr == 0x1000u);
	CHECK(THREAD(kernel, 0).context.frame.user_sp == 0x8000u);
	clock_ms = 5;
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(next == &frame);
	CHECK(zzapp_kernel_current_thread() == 1);
}

static void test_system_frame_runs_service_before_apps(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t system_frame = { 0 };
	zzapp_arm_frame_t *next;

	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	system_frame.exception_lr = 0x5000u;
	system_frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &system_frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(next == &system_frame);
	zzapp_kernel_service_due(&kernel);
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_DOORBELL, 0u);
	CHECK(next == &system_frame);
	CHECK(zzapp_kernel_current_thread() == -1);
}

static void test_round_robin_quantum(void)
{
	zzapp_kernel_t kernel;
	unsigned i;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	for (i = 0; i < 400; i++)
		(void)zzapp_kernel_schedule(&kernel, 5);
	CHECK(THREAD(kernel, 0).user_ticks == THREAD(kernel, 1).user_ticks);
	CHECK(THREAD(kernel, 0).user_ticks == 1000u);
}

/* Kit 66: every tick also makes service due, so it hands the core to the
 * system thread, which yields back to an app after its pass. With two
 * CPU-bound spin apps one of them got ~95% of the core. Both must get the
 * same share when every tick goes through the system thread. */
static void test_ticks_through_system_thread_share_fairly(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;
	unsigned runs[2] = { 0u, 0u };
	unsigned i;

	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	CHECK(zzapp_kernel_prepare_app(&kernel, 1, 0x2000u, 2u, 0u, 0u,
				       0x9000u) == 0);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	for (i = 0; i < 200; i++) {
		int app = zzapp_kernel_current_thread();

		CHECK(app == 0 || app == 1);
		if (app != 0 && app != 1)
			return;
		runs[app]++;
		/* The timer tick marks service due and lands in the app. The
		 * scheduler hands it to the system thread unless the 95% cap
		 * is spent; the system thread then finishes its pass and
		 * yields. */
		zzapp_kernel_service_due(&kernel);
		clock_ms += 5;
		next = zzapp_kernel_switch_frame(&kernel, next,
						 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
		if (zzapp_kernel_current_thread() == -1)
			next = zzapp_kernel_switch_frame(&kernel, next,
					ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	}
	CHECK(runs[0] + runs[1] == 200u);
	CHECK(runs[0] >= 95u && runs[1] >= 95u);
}

/* 4 ms of services then 1 ms of the app before the tick charges 4 and 1.
 * A voluntary handoff after 2 ms charges those 2 ms, not a full quantum. */
static void test_tick_charges_the_context_that_ran(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;

	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	THREAD(kernel, 0).state = ZZAPP_THREAD_BLOCKED;
	frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	clock_ms = 0;
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(zzapp_kernel_current_thread() == -1);
	THREAD(kernel, 0).state = ZZAPP_THREAD_RUNNABLE;
	clock_ms = 4;
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(kernel.service_ticks == 4u);
	CHECK(THREAD(kernel, 0).user_ticks == 0u);
	CHECK(zzapp_kernel_current_thread() == 0);
	clock_ms = 5;
	next->spsr = ZZAPP_KERNEL_APP_CPSR;
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(THREAD(kernel, 0).user_ticks == 1u);
	CHECK(kernel.service_ticks == 4u);
	CHECK(THREAD(kernel, 0).svc_ticks == 0u);

	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	clock_ms = 10;
	zzapp_kernel_set_current_thread(0);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	clock_ms = 12;
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(THREAD(kernel, 0).user_ticks == 2u);
	CHECK(kernel.service_ticks == 0u);
	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	frame.spsr = 0x1fu;
	clock_ms = 20;
	zzapp_kernel_set_current_thread(0);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	clock_ms = 23;
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(THREAD(kernel, 0).svc_ticks == 3u);
	CHECK(THREAD(kernel, 0).user_ticks == 0u);
	(void)next;
}

static void test_blocking_after_3ms_charges_3(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;

	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	clock_ms = 0;
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(zzapp_kernel_current_thread() == 0);
	clock_ms = 3;
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(THREAD(kernel, 0).user_ticks == 3u);
	(void)next;
}

static void test_wfe_idle_gap_charges_nothing(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;

	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	zzapp_kernel_set_current_thread(-1);
	clock_ms = 0;
	zzapp_kernel_idle_begin(&kernel);
	CHECK(kernel.idle == 1u);
	clock_ms = 50;
	zzapp_kernel_service_due(&kernel);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_DOORBELL, 0u);
	CHECK(kernel.idle == 0u);
	CHECK(kernel.idle_ticks == 50u);
	CHECK(kernel.service_ticks == 0u);
	CHECK(kernel.service_in_window == 0u);
	CHECK(kernel.overload_count == 0u);
	CHECK(zzapp_kernel_current_thread() == -1);
	(void)next;
}

static void test_recovery_gap_charges_nothing(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;

	kinit(&kernel);
	zzapp_kernel_set_current_thread(-1);
	clock_ms = 10;
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	clock_ms = 15;
	zzapp_kernel_reset_slice(&kernel);
	CHECK(kernel.slice_valid == 0u);
	clock_ms = 215;
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(kernel.service_ticks == 0u);
	CHECK(kernel.service_in_window == 0u);
	(void)next;
}

static zzapp_kernel_t *g_test_kernel;
static zzapp_arm_frame_t g_test_irq_frame;
static int g_test_irq_fired;
static int g_test_state_at_irq;
static uint64_t g_test_deadline_at_irq;

static void simulate_timer_irq(void)
{
	g_test_irq_fired++;
	g_test_state_at_irq = (int)THREAD(*g_test_kernel, 0).state;
	g_test_deadline_at_irq = THREAD(*g_test_kernel, 0).sleep_deadline;
	(void)zzapp_kernel_switch_frame(g_test_kernel, &g_test_irq_frame,
					ZZAPP_KERNEL_DISPATCH_TICK, 4u);
}

static void hook_trigger_sleep_irq(void)
{
	test_clock_hook = NULL;
	test_request_irq(simulate_timer_irq);
}

static void test_api_sleep_atomic_against_injected_irq(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;

	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	clock_ms = 0;
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(zzapp_kernel_current_thread() == 0);

	/* Arm the clock hook: inside zzapp_kernel_api_sleep(), the clock lookup
	 * occurs while IRQs are masked. The hook requests an IRQ. Because IRQs
	 * are masked, the IRQ is pended and does not fire until unmask after sleep
	 * deadline publication. */
	g_test_kernel = &kernel;
	g_test_irq_frame = *next;
	g_test_irq_fired = 0;
	g_test_state_at_irq = -1;
	g_test_deadline_at_irq = 0U;
	test_clock_hook = hook_trigger_sleep_irq;

	CHECK(zzapp_kernel_api_sleep(&kernel, 0, 10000u) == 0);
	/* Unmask delivered the pended IRQ, which ran switch_frame. */
	CHECK(g_test_irq_fired == 1);
	CHECK(g_test_state_at_irq == ZZAPP_THREAD_SLEEPING);
	CHECK(g_test_deadline_at_irq == 10u);
	CHECK(kernel.api_lock_owner == -1);
	CHECK(THREAD(kernel, 0).state == ZZAPP_THREAD_SLEEPING);
	CHECK(THREAD(kernel, 0).sleep_deadline == 10u);

	/* 10 ms elapse: the deadline arrives and the sleeper wakes up. */
	clock_ms = 10;
	zzapp_kernel_advance(&kernel, 10u);
	CHECK(THREAD(kernel, 0).state == ZZAPP_THREAD_RUNNABLE);
}

static void simulate_idle_timer_irq(void)
{
	g_test_irq_fired++;
	clock_ms = 20;
	(void)zzapp_kernel_switch_frame(g_test_kernel, &g_test_irq_frame,
					ZZAPP_KERNEL_DISPATCH_TICK, 4u);
}

static void hook_trigger_idle_irq(void)
{
	test_clock_hook = NULL;
	test_request_irq(simulate_idle_timer_irq);
}

static void test_idle_irq_no_double_charge(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };

	kinit(&kernel);
	zzapp_kernel_set_current_thread(-1);
	clock_ms = 0;

	/* Arm the clock hook: inside zzapp_kernel_idle_begin(), zzapp_kernel_now_ms()
	 * runs while IRQs are masked. The hook requests an IRQ.
	 * Because IRQs are masked, idle_begin sets idle = 1 before the IRQ fires
	 * at unmask. Switch_frame sees was_idle = 1, charges idle_ticks, and
	 * does not charge service_ticks. */
	g_test_kernel = &kernel;
	g_test_irq_frame = frame;
	g_test_irq_fired = 0;
	test_clock_hook = hook_trigger_idle_irq;

	zzapp_kernel_idle_begin(&kernel);
	/* smp_local_irq_restore at the end of idle_begin delivered the IRQ. */
	CHECK(g_test_irq_fired == 1);
	CHECK(kernel.idle == 0u);
	CHECK(kernel.idle_ticks == 20u);
	CHECK(kernel.service_ticks == 0u);
	CHECK(kernel.service_in_window == 0u);

	/* Returning to core 1 loop after IRQ executes idle_end at t = 25 ms.
	 * Since idle is 0, idle_end does not double-charge. */
	clock_ms = 25;
	zzapp_kernel_idle_end(&kernel);
	CHECK(kernel.idle == 0u);
	CHECK(kernel.idle_ticks == 20u);
	CHECK(kernel.service_ticks == 0u);
	CHECK(kernel.service_in_window == 0u);
}

static void test_service_precedes_app_and_preserves_remainder(void)
{
	zzapp_kernel_t kernel;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_schedule(&kernel, 2) == ZZAPP_KERNEL_APP0);
	zzapp_kernel_service_due(&kernel);
	CHECK(zzapp_kernel_schedule(&kernel, 1) == ZZAPP_KERNEL_SYSTEM);
	CHECK(zzapp_kernel_schedule(&kernel, 3) == ZZAPP_KERNEL_APP0);
	CHECK(THREAD(kernel, 0).quantum_left == 0u);
}

static void test_block_wake_and_idle(void)
{
	zzapp_kernel_t kernel;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 5) == ZZAPP_KERNEL_IDLE);
	CHECK(zzapp_kernel_wake(&kernel, 0, ZZAPP_WAKE_CANCELLED) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 5) == ZZAPP_KERNEL_APP0);
	CHECK(THREAD(kernel, 0).wake_reason == ZZAPP_WAKE_CANCELLED);
}

static void test_sleep_deadline_order(void)
{
	zzapp_kernel_t kernel;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	CHECK(zzapp_kernel_sleep(&kernel, 1, 10) == 0);
	CHECK(zzapp_kernel_sleep(&kernel, 0, 10) == 0);
	zzapp_kernel_advance(&kernel, 10);
	CHECK(THREAD(kernel, 1).state == ZZAPP_THREAD_RUNNABLE);
	CHECK(THREAD(kernel, 0).state == ZZAPP_THREAD_RUNNABLE);
	CHECK(kernel.wake_log_count == 2u);
	CHECK(kernel.wake_log[0] == 1u);
	CHECK(kernel.wake_log[1] == 0u);
	CHECK(zzapp_kernel_schedule(&kernel, 5) != ZZAPP_KERNEL_IDLE);
}

static void test_wait_completion_timeout_and_cancel(void)
{
	zzapp_kernel_t kernel;

	kinit(&kernel);
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

	kinit(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_schedule(&kernel, 2) == ZZAPP_KERNEL_APP0);
	zzapp_kernel_charge_svc(&kernel, 0, 3);
	CHECK(THREAD(kernel, 0).user_ticks + THREAD(kernel, 0).svc_ticks == 5u);
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	(void)zzapp_kernel_schedule(&kernel, 5);
	CHECK(THREAD(kernel, 0).user_ticks + THREAD(kernel, 0).svc_ticks == 5u);

	kinit(&kernel);
	make_runnable(&kernel, 0);
	for (i = 0; i < 200; i++) {
		zzapp_kernel_service_due(&kernel);
		(void)zzapp_kernel_schedule(&kernel, 5);
	}
	CHECK(THREAD(kernel, 0).user_ticks >= 10u);
	CHECK(kernel.overload_count != 0u);
	CHECK(kernel.service_ticks > THREAD(kernel, 0).user_ticks);
	CHECK(zzapp_kernel_schedule(&kernel, 0) != ZZAPP_KERNEL_IDLE);
}

static void test_new_generation_resets_cpu_ticks(void)
{
	zzapp_kernel_t kernel;

	kinit(&kernel);
	THREAD(kernel, 0).user_ticks = 17U;
	THREAD(kernel, 0).svc_ticks = 23U;
	zzapp_kernel_reset_cpu_ticks(&kernel, 0U);
	CHECK(THREAD(kernel, 0).user_ticks == 0U);
	CHECK(THREAD(kernel, 0).svc_ticks == 0U);
}

static void test_blocking_api_lock_and_slot_resources(void)
{
	zzapp_kernel_t kernel;
	unsigned i;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) != 0);
	CHECK(zzapp_kernel_api_block(&kernel, 0) == 0);
	CHECK(kernel.api_lock_owner == -1);
	CHECK(THREAD(kernel, 0).state == ZZAPP_THREAD_BLOCKED);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) == 0);
	for (i = 0; i < 20; i++)
		(void)zzapp_kernel_schedule(&kernel, 5);
	CHECK(THREAD(kernel, 0).user_ticks == 0u);
	irq_save_calls = 0U;
	irq_restore_calls = 0U;
	CHECK(zzapp_kernel_api_unlock(&kernel, 1) == 0);
	CHECK(irq_save_calls == 1U);
	CHECK(irq_restore_calls == 1U);
	CHECK(THREAD(kernel, 0).wake_reason == ZZAPP_WAKE_READY);

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
	CHECK(WORDS(1).resources.surface_bytes == 0x1000u);
	for (i = 0; i < ZZAPP_KERNEL_SESSION_LIMIT; i++)
		CHECK(zzapp_kernel_session_open(&kernel, 1, 0) == 0);
	CHECK(zzapp_kernel_session_open(&kernel, 1, 0) != 0);
	for (i = 0; i < ZZAPP_KERNEL_SESSION_LIMIT; i++)
		CHECK(zzapp_kernel_session_open(&kernel, 1, 1) == 0);
	CHECK(zzapp_kernel_session_open(&kernel, 1, 1) != 0);
	zzapp_kernel_stop_app(&kernel, 1);
	CHECK(WORDS(1).resources.surface_bytes == 0u);
	CHECK(WORDS(1).resources.image_sessions == 0u);
	CHECK(WORDS(1).resources.audio_sessions == 0u);
}

static void test_waiter_isolation_timeout_and_stop(void)
{
	zzapp_kernel_t kernel;

	kinit(&kernel);
	make_runnable(&kernel, 1);
	make_runnable(&kernel, 2);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) == 0);
	CHECK(zzapp_kernel_api_block(&kernel, 1) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 5) == ZZAPP_KERNEL_APP2);
	CHECK(zzapp_kernel_wake(&kernel, 1, ZZAPP_WAKE_READY) == 0);
	CHECK(kernel.wake_log_count == 1u);
	CHECK(kernel.wake_log[0] == 1u);
	CHECK(THREAD(kernel, 2).state == ZZAPP_THREAD_RUNNABLE);

	CHECK(zzapp_kernel_sleep(&kernel, 1, kernel.now + 10u) == 0);
	zzapp_kernel_advance(&kernel, 10u);
	CHECK(zzapp_kernel_wait_status(&kernel, 1) == ZZAPP_KERNEL_WAIT_TIMEOUT);

	CHECK(zzapp_kernel_block(&kernel, 1) == 0);
	zzapp_kernel_stop_app(&kernel, 1);
	CHECK(zzapp_kernel_wait_status(&kernel, 1) == ZZAPP_KERNEL_WAIT_CANCELLED);
}

static void test_stop_wakes_api_lock_waiter(void)
{
	zzapp_kernel_t kernel;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) != 0);
	CHECK(zzapp_kernel_block(&kernel, 1) == 0);
	zzapp_kernel_stop_app(&kernel, 0);
	CHECK(THREAD(kernel, 1).state == ZZAPP_THREAD_RUNNABLE);
	CHECK(THREAD(kernel, 1).wake_reason == ZZAPP_WAKE_READY);
}

static void test_system_kernel_lock_handoffs(void)
{
	zzapp_kernel_t kernel;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	CHECK(kernel.api_lock_owner == ZZAPP_KERNEL_SYSTEM_OWNER);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) != 0);
	CHECK(zzapp_kernel_api_block(&kernel, 0) == 0);
	CHECK(THREAD(kernel, 0).state == ZZAPP_THREAD_BLOCKED);
	CHECK(zzapp_kernel_schedule(&kernel, 0) == ZZAPP_KERNEL_SYSTEM);
	CHECK(zzapp_kernel_system_unlock(&kernel) == 0);
	CHECK(THREAD(kernel, 0).state == ZZAPP_THREAD_RUNNABLE);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_api_unlock(&kernel, 0) == 0);

	kinit(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	zzapp_kernel_service_due(&kernel);
	CHECK(zzapp_kernel_schedule(&kernel, 0) == ZZAPP_KERNEL_APP0);
	CHECK(kernel.service_due != 0U);
	CHECK(zzapp_kernel_api_unlock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 0) == ZZAPP_KERNEL_SYSTEM);

	kinit(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_system_lock(&kernel) != 0);
	CHECK(zzapp_kernel_schedule(&kernel, 0) == ZZAPP_KERNEL_APP0);
	CHECK(zzapp_kernel_api_unlock(&kernel, 0) == 0);
}

/* SDK/audio doorbells can make the system thread release and immediately
 * retake the API lock several times within one millisecond.  An app that
 * blocked while entering an API call must receive that release before the
 * next service pass; round-robin never advances while all dispatches have
 * zero elapsed milliseconds. */
static void test_api_waiter_handoff_beats_continuous_service(void)
{
	zzapp_kernel_t kernel;
	unsigned calls = 0U;
	unsigned burst;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	make_runnable(&kernel, 2);
	make_runnable(&kernel, 3);

	for (burst = 0U; burst < 3U; ++burst) {
		CHECK(zzapp_kernel_system_lock(&kernel) == 0);
		CHECK(zzapp_kernel_api_lock(&kernel, 0) != 0);
		CHECK(zzapp_kernel_block(&kernel, 0) == 0);
		CHECK(zzapp_kernel_system_unlock(&kernel) == 0);
		/* The worker can loop immediately after an empty perf doorbell. */
		CHECK(zzapp_kernel_system_lock(&kernel) != 0);
		zzapp_kernel_service_due(&kernel);
		CHECK(zzapp_kernel_schedule(&kernel, 0U) == ZZAPP_KERNEL_APP0);
		CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
		calls++;
		CHECK(zzapp_kernel_api_unlock(&kernel, 0) == 0);
	}
	CHECK(calls == 3U);
}
/* A wait inside an API body releases the lock before blocking.  Its service
 * pass wakes the app while holding the system lock; that wake must reserve
 * the next lock handoff so the worker cannot immediately retake it. */
static void test_woken_api_waiter_beats_continuous_service(void)
{
	zzapp_kernel_t kernel;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_api_block(&kernel, 0) == 0);
	CHECK(THREAD(kernel, 0).state == ZZAPP_THREAD_BLOCKED);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	CHECK(zzapp_kernel_system_unlock(&kernel) == 0);
	CHECK(THREAD(kernel, 0).state == ZZAPP_THREAD_BLOCKED);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	CHECK(zzapp_kernel_wake(&kernel, 0, ZZAPP_WAKE_READY) == 0);
	CHECK(zzapp_kernel_system_unlock(&kernel) == 0);
	CHECK(zzapp_kernel_system_lock(&kernel) != 0);
	CHECK(zzapp_kernel_schedule(&kernel, 0U) == ZZAPP_KERNEL_APP0);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
}


static void test_api_lock_handoff_is_fifo(void)
{
	zzapp_kernel_t kernel;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	make_runnable(&kernel, 2);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 2) != 0);
	CHECK(zzapp_kernel_block(&kernel, 2) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) != 0);
	CHECK(zzapp_kernel_block(&kernel, 1) == 0);
	CHECK(zzapp_kernel_system_unlock(&kernel) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 0U) == ZZAPP_KERNEL_APP2);
	CHECK(zzapp_kernel_api_lock(&kernel, 2) == 0);
	CHECK(zzapp_kernel_api_unlock(&kernel, 2) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 0U) == ZZAPP_KERNEL_APP1);
}

static void test_pending_service_runs_between_api_handoffs(void)
{
	zzapp_kernel_t kernel;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	make_runnable(&kernel, 2);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) != 0);
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) != 0);
	CHECK(zzapp_kernel_block(&kernel, 1) == 0);
	CHECK(zzapp_kernel_system_unlock(&kernel) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 0U) == ZZAPP_KERNEL_APP0);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	zzapp_kernel_service_due(&kernel);
	CHECK(zzapp_kernel_api_unlock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 0U) == ZZAPP_KERNEL_SYSTEM);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	CHECK(zzapp_kernel_system_unlock(&kernel) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 0U) == ZZAPP_KERNEL_APP1);
}

static void test_service_budget_preserves_deferred_handoff(void)
{
	zzapp_kernel_t kernel;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	make_runnable(&kernel, 2);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) != 0);
	CHECK(zzapp_kernel_block(&kernel, 1) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 2) != 0);
	CHECK(zzapp_kernel_block(&kernel, 2) == 0);
	CHECK(zzapp_kernel_system_unlock(&kernel) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 0U) == ZZAPP_KERNEL_APP1);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) == 0);
	zzapp_kernel_service_due(&kernel);
	kernel.service_in_window = ZZAPP_KERNEL_SERVICE_LIMIT_MS;
	CHECK(zzapp_kernel_api_unlock(&kernel, 1) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 5U) == ZZAPP_KERNEL_APP2);
}

static void test_fault_releases_kernel_lock(void)
{
	zzapp_kernel_t kernel;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) != 0);
	CHECK(zzapp_kernel_api_block(&kernel, 1) == 0);
	CHECK(zzapp_kernel_api_fault_release(&kernel, 0) == 0);
	CHECK(kernel.api_lock_owner == -1);
	CHECK(THREAD(kernel, 1).state == ZZAPP_THREAD_RUNNABLE);
}

static void test_kernel_stack_covers_decoder_call_chain(void)
{
	/* MP3's decode scratch alone is 13,216 bytes; leave room for the
	 * exception frame and its callers without sharing a stack between apps. */
	CHECK(ZZAPP_KERNEL_STACK_BYTES >= 32768U);
}

static void test_non_timer_interrupt_does_not_advance_time(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };

	kinit(&kernel);
	(void)zzapp_kernel_switch_frame(&kernel, &frame,
		ZZAPP_KERNEL_DISPATCH_INTERRUPT, 4U);
	CHECK(kernel.now == 0U);
}

static void test_system_ticks_wake_sleepers(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t system_frame = { 0 };
	zzapp_arm_frame_t *next;

	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_sleep(&kernel, 0, 10u) == 0);
	zzapp_kernel_set_current_thread(-1);
	clock_ms = 0;
	next = zzapp_kernel_switch_frame(&kernel, &system_frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	clock_ms = 5;
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(next == &system_frame);
	CHECK(kernel.now == 5u);
	CHECK(THREAD(kernel, 0).state == ZZAPP_THREAD_SLEEPING);
	clock_ms = 10;
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(kernel.now == 10u);
	CHECK(next == &system_frame);
	CHECK(THREAD(kernel, 0).state == ZZAPP_THREAD_RUNNABLE);
}
static void test_restart_releases_kernel_lock_waiters(void)
{
	zzapp_kernel_t kernel;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) != 0);
	CHECK(zzapp_kernel_api_block(&kernel, 0) == 0);
	zzapp_kernel_reset_lock(&kernel);
	CHECK(kernel.api_lock_owner == -1);
	CHECK(kernel.api_lock_depth == 0U);
	CHECK(WORDS(0).api_waiting == 0U);
	CHECK(THREAD(kernel, 0).state == ZZAPP_THREAD_RUNNABLE);
	CHECK(THREAD(kernel, 0).wake_reason == ZZAPP_WAKE_READY);

	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) != 0);
	CHECK(zzapp_kernel_api_block(&kernel, 1) == 0);
	zzapp_kernel_reset_lock(&kernel);
	CHECK(kernel.api_lock_owner == -1);
	CHECK(kernel.api_lock_depth == 0U);
	CHECK(WORDS(1).api_waiting == 0U);
	CHECK(THREAD(kernel, 1).state == ZZAPP_THREAD_RUNNABLE);
	CHECK(THREAD(kernel, 1).wake_reason == ZZAPP_WAKE_READY);
}

static void test_system_lock_inheritance_is_throttled(void)
{
	zzapp_kernel_t kernel;
	unsigned i;

	kinit(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) != 0);
	CHECK(zzapp_kernel_api_block(&kernel, 0) == 0);
	for (i = 0U; i < 19U; ++i)
		CHECK(zzapp_kernel_schedule(&kernel, 5U) == ZZAPP_KERNEL_SYSTEM);
	CHECK(kernel.service_ticks == 95U);
	CHECK(kernel.service_in_window == 95U);
	CHECK(zzapp_kernel_schedule(&kernel, 5U) == ZZAPP_KERNEL_APP1);
	CHECK(THREAD(kernel, 1).user_ticks == 5U);
	CHECK(kernel.overload_count == 1U);
}

/* Model the assembly order: entry, exit, another entry, then C consumes
 * the completed pair. */
#if ZZAPP_PERF_COUNTERS
static void test_performance_counters(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;
	zzapp_kernel_perf_snapshot_t snapshot;
	zzapp_kernel_perf_metric_t overflow = { 0 };

	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	CHECK(zzapp_kernel_prepare_app(&kernel, 1, 0x2000u, 2u, 0u, 0u,
				       0x9000u) == 0);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	zzapp_kernel_set_current_thread(0);
	zzapp_kernel_switch_entry_stamp = 100u;
	zzapp_kernel_switch_exit_stamp = 160u;
	zzapp_kernel_switch_completed_entry_stamp = 100u;
	zzapp_kernel_switch_complete = 1u;
	zzapp_kernel_switch_entry_stamp = 200u;
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	zzapp_kernel_switch_exit_stamp = 280u;
	zzapp_kernel_switch_completed_entry_stamp = 200u;
	zzapp_kernel_switch_complete = 1u;
	zzapp_kernel_switch_entry_stamp = 300u;
	(void)zzapp_kernel_switch_frame(&kernel, next,
					ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	zzapp_kernel_perf_sgi_raised(&kernel, 200u);
	zzapp_kernel_perf_sgi_started(&kernel, 260u);
	zzapp_kernel_perf_sgi_started(&kernel, 300u);
	zzapp_kernel_perf_sgi_raised(&kernel, 400u);
	zzapp_kernel_perf_sgi_started(&kernel, 460u);
	zzapp_kernel_perf_work_published(&kernel, 4u);
	zzapp_kernel_perf_sdk_started(&kernel, 240u, 280u);
	zzapp_kernel_perf_service_begin(&kernel, ZZAPP_PERF_SERVICE_AUDIO, 300u);
	zzapp_kernel_perf_service_end(&kernel, ZZAPP_PERF_SERVICE_AUDIO, 380u);
	zzapp_kernel_perf_audio(&kernel, 1u, 3u, 4u, 2u);
	zzapp_kernel_perf_service_begin(&kernel, ZZAPP_PERF_SERVICE_SDK, 400u);
	zzapp_kernel_perf_service_end_detail(&kernel, ZZAPP_PERF_SERVICE_SDK,
					     480u, 0x1234u, 0u);
	zzapp_kernel_perf_service_begin(&kernel, ZZAPP_PERF_SERVICE_LAUNCHER,
					500u);
	zzapp_kernel_perf_service_end_detail(&kernel,
					     ZZAPP_PERF_SERVICE_LAUNCHER,
					     700u, 0x103u, 192u * 1024u);
	zzapp_kernel_perf_service_begin(&kernel, ZZAPP_PERF_SERVICE_LAUNCHER,
					710u);
	zzapp_kernel_perf_service_end_detail(&kernel,
					     ZZAPP_PERF_SERVICE_LAUNCHER,
					     810u, 0x101u, 32u * 1024u);
	zzapp_kernel_perf_service_begin(&kernel, ZZAPP_PERF_SERVICE_LAUNCHER,
					820u);
	zzapp_kernel_perf_service_end_detail(&kernel,
					     ZZAPP_PERF_SERVICE_LAUNCHER,
					     930u, 0x102u, 48u * 1024u);
	zzapp_kernel_perf_service_begin(&kernel, ZZAPP_PERF_SERVICE_LAUNCHER,
					940u);
	zzapp_kernel_perf_service_end_detail(&kernel,
					     ZZAPP_PERF_SERVICE_LAUNCHER,
					     1060u, 0x104u, 512u * 16u);
	zzapp_kernel_perf_service_begin(&kernel, ZZAPP_PERF_SERVICE_LAUNCHER,
					1070u);
	zzapp_kernel_perf_service_end_detail(&kernel,
					     ZZAPP_PERF_SERVICE_LAUNCHER,
					     1200u, 0x105u, 256u * 16u);
	zzapp_kernel_perf_service_begin(&kernel, ZZAPP_PERF_SERVICE_LAUNCHER,
					1210u);
	zzapp_kernel_perf_service_end_detail(&kernel,
					     ZZAPP_PERF_SERVICE_LAUNCHER,
					     1350u, 0x106u, 4u << 20);
	zzapp_kernel_perf_service_begin(&kernel, ZZAPP_PERF_SERVICE_LAUNCHER,
					1360u);
	zzapp_kernel_perf_service_end_detail(&kernel,
					     ZZAPP_PERF_SERVICE_LAUNCHER,
					     1510u, 1u, 0u);
	/* A smaller later sample must not overwrite the per-kind maximum. */
	zzapp_kernel_perf_service_begin(&kernel, ZZAPP_PERF_SERVICE_LAUNCHER,
					1520u);
	zzapp_kernel_perf_service_end_detail(&kernel,
					     ZZAPP_PERF_SERVICE_LAUNCHER,
					     1570u, 0x101u, 16u * 1024u);
	zzapp_kernel_perf_snapshot(&kernel, 1u, &snapshot);
	CHECK(snapshot.switch_cycles.samples == 2u);
	CHECK(snapshot.switch_cycles.max == 80u);
	CHECK(snapshot.sgi_to_service.samples == 2u);
	CHECK(snapshot.sgi_to_service.max == 60u);
	CHECK(snapshot.sdk_enqueue_to_start.samples == 1u);
	CHECK(snapshot.sdk_enqueue_to_start.max == 40u);
	CHECK(snapshot.service[ZZAPP_PERF_SERVICE_AUDIO].max == 80u);
	CHECK(snapshot.service_max_detail[ZZAPP_PERF_SERVICE_SDK] == 0x1234u);
	CHECK(snapshot.service_max_detail[ZZAPP_PERF_SERVICE_LAUNCHER] == 0x103u);
	CHECK(snapshot.service_max_bytes[ZZAPP_PERF_SERVICE_LAUNCHER] ==
	      (192u * 1024u));
	CHECK(snapshot.launcher_step_max[ZZAPP_PERF_LAUNCHER_ZERO] == 100u);
	CHECK(snapshot.launcher_step_max[ZZAPP_PERF_LAUNCHER_COPY] == 110u);
	CHECK(snapshot.launcher_step_max[ZZAPP_PERF_LAUNCHER_MAP] == 200u);
	CHECK(snapshot.launcher_step_max[ZZAPP_PERF_LAUNCHER_RELOC] == 120u);
	CHECK(snapshot.launcher_step_max[ZZAPP_PERF_LAUNCHER_APPLY] == 130u);
	CHECK(snapshot.launcher_step_max[ZZAPP_PERF_LAUNCHER_ALLOC] == 140u);
	CHECK(snapshot.launcher_step_max[ZZAPP_PERF_LAUNCHER_OTHER] == 150u);
	CHECK(snapshot.sgis == 2u);
	CHECK(snapshot.work_publications == 4u);
	CHECK(snapshot.audio_low_water == 3u);
	CHECK(snapshot.audio_starved == 4u);
	CHECK(snapshot.audio_underruns == 2u);

	clear_perf_requests(&kernel);

	zzapp_kernel_perf_request_report(&kernel, 1u, 0);
	CHECK(WORDS(1).perf_report_pending != 0U);
	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 1);
	CHECK(strstr(kernel.perf.report_text,
		     "sdk_max_op=0x1234 launcher_max_step=loader-map:196608") !=
	      NULL);
	CHECK(strstr(kernel.perf.report_text,
		     "launcher_steps=zero:100/copy:110/map:200/reloc:120/"
		     "apply:130/alloc:140/other:150") != NULL);
	zzapp_kernel_perf_print_reports(&kernel);
	CHECK(kernel.perf.report_pending == 0U && WORDS(1).perf_report_pending == 0U);
	zzapp_kernel_perf_request_report(&kernel, 1u, 0);
	CHECK(kernel.perf.report_pending == 0U && WORDS(1).perf_report_pending == 0U);
	kernel.now += 10000u;
	zzapp_kernel_perf_request_report(&kernel, 1u, 0);
	zzapp_kernel_perf_request_report(&kernel, 2u, 0);
	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 1);
	CHECK(WORDS(2).perf_report_pending != 0U);
	zzapp_kernel_perf_print_reports(&kernel);
	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 0);
	zzapp_kernel_perf_request_report(&kernel, 3u, 1);
	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 1);
	CHECK(strstr(kernel.perf.report_text, "forced=1") != NULL);
	zzapp_kernel_perf_print_reports(&kernel);
	WORDS(1).audio_low_water = 9u;
	WORDS(1).audio_starved = 7u;
	WORDS(1).audio_underruns = 8u;
	WORDS(1).tlb_instruction_refills = 7u;
	WORDS(1).tlb_data_refills = 6u;
	CHECK(zzapp_kernel_prepare_app(&kernel, 1, 0x3000u, 3u, 0u, 0u,
				       0xa000u) == 0);
	zzapp_kernel_perf_snapshot(&kernel, 1u, &snapshot);
	CHECK(snapshot.audio_low_water == 0u);
	CHECK(snapshot.audio_starved == 0u);
	CHECK(snapshot.audio_underruns == 0u);
	CHECK(snapshot.tlb_instruction_refills == 0u);
	CHECK(snapshot.tlb_data_refills == 0u);

	irq_save_calls = 0U;
	irq_restore_calls = 0U;
	zzapp_kernel_switch_completed_entry_stamp = 500u;
	zzapp_kernel_switch_exit_stamp = 550u;
	zzapp_kernel_switch_complete = 1u;
	zzapp_kernel_perf_switch_finished(&kernel);
	CHECK(irq_save_calls == 1U);
	CHECK(irq_restore_calls == 1U);

	irq_save_calls = 0U;
	irq_restore_calls = 0U;
	kernel.perf.tlb_pid = 0U;
	kernel.perf.tlb_instruction_stamp = 10U;
	kernel.perf.tlb_data_stamp = 20U;
	zzapp_kernel_perf_finish_slot(&kernel, 0U);
	CHECK(irq_save_calls == 1U);
	CHECK(irq_restore_calls == 1U);

	overflow.samples = 43383508u;
	overflow.histogram[0] = 1u;
	overflow.histogram[31] = overflow.samples - 1u;
	CHECK(zzapp_kernel_perf_test_percentile(&overflow, 99u) == (1U << 31));
}
static void test_tlb_samples_survive_system_dispatch(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;
	zzapp_kernel_perf_snapshot_t snapshot;
	unsigned i;

	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	clock_ms = 0U;
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0U);
	CHECK(zzapp_kernel_current_thread() == 0);
	for (i = 0U; i < 10U; i++) {
		zzapp_kernel_service_due(&kernel);
		clock_ms++;
		next = zzapp_kernel_switch_frame(&kernel, next,
						 ZZAPP_KERNEL_DISPATCH_TICK, 4U);
		CHECK(zzapp_kernel_current_thread() == -1);
		clock_ms++;
		next = zzapp_kernel_switch_frame(&kernel, next,
						 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0U);
		CHECK(zzapp_kernel_current_thread() == 0);
	}
	zzapp_kernel_perf_snapshot(&kernel, 0U, &snapshot);
	CHECK(snapshot.tlb_instruction_refills == 9U);
	CHECK(snapshot.tlb_data_refills == 9U);
}
static void test_tlb_window_expiry_drops_system_baseline(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;
	zzapp_kernel_perf_snapshot_t snapshot;

	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	clock_ms = 0U;
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0U);
	zzapp_kernel_service_due(&kernel);
	clock_ms = 1U;
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4U);
	CHECK(zzapp_kernel_current_thread() == -1);
	CHECK(kernel.perf.tlb_stamps_valid != 0U);

	kernel.now = 8000U;
	clock_ms = 12000U;
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0U);
	CHECK(zzapp_kernel_current_thread() == 0);
	CHECK(kernel.perf.tlb_stamps_valid == 0U);

	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 1);
	zzapp_kernel_perf_request_report(&kernel, 0U, 1);
	zzapp_kernel_service_due(&kernel);
	clock_ms++;
	(void)zzapp_kernel_switch_frame(&kernel, next,
					ZZAPP_KERNEL_DISPATCH_TICK, 4U);
	zzapp_kernel_perf_snapshot(&kernel, 0U, &snapshot);
	CHECK(snapshot.tlb_instruction_refills == 0U);
	CHECK(snapshot.tlb_data_refills == 0U);
}


#endif


static void test_wake_hands_back_to_runnable_app(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame;
	zzapp_arm_frame_t *next;

	kinit(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	frame = THREAD(kernel, 0).context.frame;
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(zzapp_kernel_current_thread() == 0);
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	zzapp_kernel_service_due(&kernel);
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_DOORBELL, 0u);
	CHECK(zzapp_kernel_current_thread() == -1);
	wake_events = 0U;
	CHECK(zzapp_kernel_wake(&kernel, 0, ZZAPP_WAKE_READY) == 0);
	CHECK(wake_events == 1U);
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(zzapp_kernel_current_thread() == 0);
}


/* ---- Many records: the scheduler no longer knows a count of four ----- */

static void start_range(zzapp_kernel_t *kernel, unsigned first, unsigned count)
{
	unsigned pid;

	for (pid = first; pid < first + count; pid++) {
		CHECK(zzapp_kernel_prepare_app(kernel, pid, 0x1000u + pid, 1u, 0u,
					       0u, 0x8000u) == 0);
		CHECK(zzapp_kernel_start_app(kernel, pid) == 0);
	}
}

static void test_init_rejects_bad_areas(void)
{
	zzapp_kernel_t kernel;

	CHECK(zzapp_kernel_init(&kernel, NULL, sizeof(g_recs[0]), 4u) == -1);
	CHECK(zzapp_kernel_init(&kernel, g_recs, sizeof(g_recs[0]), 0u) == -1);
	CHECK(zzapp_kernel_init(&kernel, g_recs, sizeof(g_recs[0]),
				ZZAPP_KERNEL_MAX_APPS + 1u) == -1);
	/* The stride must hold a whole record, not just a thread. */
	CHECK(zzapp_kernel_init(&kernel, g_recs, sizeof(zzapp_thread_t), 4u) == -1);
	CHECK(zzapp_kernel_init(&kernel, (uint8_t *)g_recs + 1, sizeof(g_recs[0]),
				4u) == -1);
	CHECK(zzapp_kernel_init(&kernel, g_recs, sizeof(g_recs[0]), 4u) == 0);
	CHECK(zzapp_kernel_capacity(&kernel) == 4u);
	CHECK(zzapp_kernel_thread(&kernel, 3u) == &g_recs[3].thread);
	CHECK(zzapp_kernel_thread(&kernel, 4u) == NULL);
	CHECK(zzapp_kernel_start_app(&kernel, 4u) == -1);
	CHECK(zzapp_kernel_block(&kernel, 4u) == -1);
	CHECK(zzapp_kernel_wake(&kernel, 4u, ZZAPP_WAKE_READY) == -1);
	CHECK(zzapp_kernel_api_lock(&kernel, 4u) == -1);
	CHECK(kernel.active == ZZAPP_PID_NONE);
	CHECK(kernel.api_handoff == ZZAPP_PID_NONE);
}

/* A card that booted without a record arena has no records, but the system
 * thread still takes the kernel lock. A zeroed kernel (the static before any
 * init) cannot: zero reads as record 0 owning the API lock. */
static void test_system_only_kernel(void)
{
	zzapp_kernel_t kernel;

	memset(&kernel, 0, sizeof(kernel));
	CHECK(zzapp_kernel_system_lock(&kernel) == -1);
	zzapp_kernel_init_system_only(&kernel);
	CHECK(zzapp_kernel_capacity(&kernel) == 0u);
	CHECK(zzapp_kernel_thread(&kernel, 0u) == NULL);
	CHECK(zzapp_kernel_run_count(&kernel) == 0u);
	CHECK(kernel.active == ZZAPP_PID_NONE && kernel.api_lock_owner == -1);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	CHECK(zzapp_kernel_system_unlock(&kernel) == 0);
	CHECK(zzapp_kernel_system_unlock(&kernel) == 0);
	CHECK(kernel.api_lock_owner == -1);
	zzapp_kernel_service_due(&kernel);
	CHECK(zzapp_kernel_start_app(&kernel, 0u) == -1);
	CHECK(zzapp_kernel_api_lock(&kernel, 0u) == -1);
	zzapp_kernel_init_system_only(NULL);
}

/* The records outlive a scheduler restart: a thread left RUNNABLE by the
 * previous run must not stay on a run list that no longer knows it. */
static void test_init_clears_threads_left_by_a_previous_run(void)
{
	zzapp_kernel_t kernel;

	kinit_n(&kernel, 8u);
	start_range(&kernel, 3u, 1u);
	CHECK(zzapp_kernel_surface_bind(&kernel, 3u, 1u, 0x200000u, 0x1000u) == 0);
	CHECK(THREAD(kernel, 3).state == ZZAPP_THREAD_RUNNABLE);
	kinit_n(&kernel, 8u);
	CHECK(THREAD(kernel, 3).state == ZZAPP_THREAD_STOPPED);
	CHECK(THREAD(kernel, 3).in_run == 0u);
	CHECK(zzapp_kernel_run_count(&kernel) == 0u);
	CHECK(WORDS(3).resources.surface_bytes == 0u);
	CHECK(zzapp_kernel_apps_runnable(&kernel) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 5u) == ZZAPP_KERNEL_IDLE);
}

static void test_run_list_follows_start_and_stop(void)
{
	zzapp_kernel_t kernel;

	kinit_n(&kernel, 64u);
	start_range(&kernel, 9u, 1u);
	start_range(&kernel, 2u, 1u);
	start_range(&kernel, 5u, 1u);
	/* Ascending whatever the start order, and no entry twice. */
	CHECK(zzapp_kernel_start_app(&kernel, 5u) == 0);
	CHECK(zzapp_kernel_run_count(&kernel) == 3u);
	CHECK(zzapp_kernel_run_pid(&kernel, 0u) == 2u);
	CHECK(zzapp_kernel_run_pid(&kernel, 1u) == 5u);
	CHECK(zzapp_kernel_run_pid(&kernel, 2u) == 9u);
	CHECK(zzapp_kernel_run_pid(&kernel, 3u) == ZZAPP_PID_NONE);
	CHECK(THREAD(kernel, 5).in_run == 1u && THREAD(kernel, 4).in_run == 0u);
	/* A blocked thread is still on the list; a stopped one is not. */
	CHECK(zzapp_kernel_block(&kernel, 5u) == 0);
	CHECK(zzapp_kernel_run_count(&kernel) == 3u);
	zzapp_kernel_stop_app(&kernel, 2u);
	CHECK(zzapp_kernel_run_count(&kernel) == 2u);
	CHECK(zzapp_kernel_run_pid(&kernel, 0u) == 5u);
	CHECK(THREAD(kernel, 2).in_run == 0u);
	CHECK(zzapp_kernel_apps_runnable(&kernel) == 1);
	zzapp_kernel_stop_app(&kernel, 9u);
	CHECK(zzapp_kernel_apps_runnable(&kernel) == 0);
	zzapp_kernel_stop_app(&kernel, 5u);
	CHECK(zzapp_kernel_run_count(&kernel) == 0u);
}

/* 64 started threads take turns in pid order, two laps, and every thread is
 * charged the same. */
static void test_64_threads_round_robin_through_run_list(void)
{
	zzapp_kernel_t kernel;
	unsigned lap;
	unsigned pid;

	kinit_n(&kernel, 64u);
	start_range(&kernel, 0u, 64u);
	CHECK(zzapp_kernel_run_count(&kernel) == 64u);
	for (pid = 0u; pid < 64u; pid++)
		CHECK(zzapp_kernel_run_pid(&kernel, pid) == pid);
	for (lap = 0u; lap < 2u; lap++)
		for (pid = 0u; pid < 64u; pid++)
			CHECK(zzapp_kernel_schedule(&kernel, 5u) == (int)pid);
	for (pid = 0u; pid < 64u; pid++)
		CHECK(THREAD(kernel, pid).user_ticks == 10u);

	/* A stopped thread leaves the rotation, and joins it again at its
	 * place: it starts at the least virtual run time of the others, which
	 * is behind every thread that already ran this lap, so it runs next
	 * after the lap. */
	zzapp_kernel_stop_app(&kernel, 20u);
	CHECK(zzapp_kernel_run_count(&kernel) == 63u);
	for (pid = 0u; pid < 20u; pid++)
		CHECK(zzapp_kernel_schedule(&kernel, 5u) == (int)pid);
	CHECK(zzapp_kernel_schedule(&kernel, 5u) == 21);
	start_range(&kernel, 20u, 1u);
	CHECK(zzapp_kernel_schedule(&kernel, 5u) == 22);
	for (pid = 23u; pid < 64u; pid++)
		CHECK(zzapp_kernel_schedule(&kernel, 5u) == (int)pid);
	CHECK(zzapp_kernel_schedule(&kernel, 5u) == 20);
	CHECK(zzapp_kernel_schedule(&kernel, 5u) == 21);
}

/* Pids above 31 and above 255, with gaps: the order is still ascending and
 * wraps from the highest to the lowest. */
static void test_sparse_large_pids_round_robin(void)
{
	static const unsigned pids[8] = { 3u, 31u, 32u, 63u, 64u, 255u, 256u, 299u };
	zzapp_kernel_t kernel;
	unsigned lap;
	unsigned i;

	kinit_n(&kernel, 300u);
	for (i = 8u; i-- > 0u;)
		start_range(&kernel, pids[i], 1u);
	CHECK(zzapp_kernel_run_count(&kernel) == 8u);
	for (lap = 0u; lap < 3u; lap++)
		for (i = 0u; i < 8u; i++)
			CHECK(zzapp_kernel_schedule(&kernel, 5u) == (int)pids[i]);
	CHECK(THREAD(kernel, 299).user_ticks == 15u);
	CHECK(THREAD(kernel, 256).user_ticks == 15u);
	CHECK(THREAD(kernel, 4).user_ticks == 0u);
}

static void test_yield_skips_the_yielder(void)
{
	zzapp_kernel_t kernel;

	kinit_n(&kernel, 64u);
	start_range(&kernel, 10u, 1u);
	start_range(&kernel, 40u, 2u);
	zzapp_kernel_set_current_thread(40);
	zzapp_kernel_yield(&kernel);
	CHECK(THREAD(kernel, 40).yield_pending == 1u);
	/* The next pick after the yield is a peer: the first of equal virtual
	 * run time in pid order. */
	kernel.active = ZZAPP_PID_NONE;
	CHECK(zzapp_kernel_dispatch(&kernel, ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u) == 10);
	CHECK(THREAD(kernel, 40).yield_pending == 0u);
	/* Nothing else runnable: no yield. */
	zzapp_kernel_stop_app(&kernel, 10u);
	zzapp_kernel_stop_app(&kernel, 41u);
	zzapp_kernel_yield(&kernel);
	CHECK(THREAD(kernel, 40).yield_pending == 0u);
	zzapp_kernel_set_current_thread(-1);
}

/* 64 sleepers with distinct deadlines wake one per millisecond in deadline
 * order, and the wake log keeps the latest wakes whatever the count. */
static void test_64_sleepers_wake_in_deadline_order(void)
{
	zzapp_kernel_t kernel;
	unsigned pid;
	unsigned step;
	unsigned woken;
	unsigned at_deadline[65];

	kinit_n(&kernel, 64u);
	start_range(&kernel, 0u, 64u);
	/* 37 is odd, so pid * 37 mod 64 visits 0..63 once: deadlines 1..64,
	 * in no pid order. */
	for (pid = 0u; pid < 64u; pid++) {
		unsigned deadline = 1u + (pid * 37u) % 64u;

		at_deadline[deadline] = pid;
		CHECK(zzapp_kernel_sleep(&kernel, pid, deadline) == 0);
	}
	for (step = 1u; step <= 64u; step++) {
		zzapp_kernel_advance(&kernel, 1u);
		woken = 0u;
		for (pid = 0u; pid < 64u; pid++)
			woken += THREAD(kernel, pid).state == ZZAPP_THREAD_RUNNABLE;
		CHECK(woken == step);
		CHECK(THREAD(kernel, at_deadline[step]).state ==
		      ZZAPP_THREAD_RUNNABLE);
		CHECK(THREAD(kernel, at_deadline[step]).wake_reason ==
		      ZZAPP_WAKE_TIMEOUT);
	}
	CHECK(kernel.wake_log_count == 64u);
	for (step = 48u; step < 64u; step++)
		CHECK(kernel.wake_log[step % ZZAPP_KERNEL_WAKE_LOG] ==
		      at_deadline[step + 1u]);

	/* The same sleepers, all due in one advance: the earliest deadline
	 * still goes first, not the lowest or highest pid. */
	kinit_n(&kernel, 64u);
	start_range(&kernel, 0u, 64u);
	for (pid = 0u; pid < 64u; pid++)
		CHECK(zzapp_kernel_sleep(&kernel, pid, 1u + (pid * 37u) % 64u) == 0);
	zzapp_kernel_advance(&kernel, 64u);
	CHECK(kernel.wake_log_count == 64u);
	for (step = 48u; step < 64u; step++)
		CHECK(kernel.wake_log[step % ZZAPP_KERNEL_WAKE_LOG] ==
		      at_deadline[step + 1u]);
}

/* The wake log holds pids, not bytes or slot numbers. */
static void test_wake_log_keeps_pids_above_255(void)
{
	zzapp_kernel_t kernel;

	kinit_n(&kernel, 300u);
	start_range(&kernel, 260u, 1u);
	start_range(&kernel, 299u, 1u);
	CHECK(zzapp_kernel_block(&kernel, 260u) == 0);
	CHECK(zzapp_kernel_block(&kernel, 299u) == 0);
	CHECK(zzapp_kernel_wake(&kernel, 299u, ZZAPP_WAKE_READY) == 0);
	CHECK(zzapp_kernel_wake(&kernel, 260u, ZZAPP_WAKE_READY) == 0);
	CHECK(kernel.wake_log_count == 2u);
	CHECK(kernel.wake_log[0] == 299u);
	CHECK(kernel.wake_log[1] == 260u);
}

/* All due at once with one deadline: they wake in the order they went to
 * sleep, newest sleeper last. */
static void test_64_sleepers_with_equal_deadlines_wake_in_sleep_order(void)
{
	zzapp_kernel_t kernel;
	unsigned i;

	kinit_n(&kernel, 64u);
	start_range(&kernel, 0u, 64u);
	for (i = 0u; i < 64u; i++)
		CHECK(zzapp_kernel_sleep(&kernel, 63u - i, 10u) == 0);
	zzapp_kernel_advance(&kernel, 10u);
	CHECK(kernel.wake_log_count == 64u);
	for (i = 48u; i < 64u; i++)
		CHECK(kernel.wake_log[i % ZZAPP_KERNEL_WAKE_LOG] == 63u - i);
	for (i = 0u; i < 64u; i++)
		CHECK(THREAD(kernel, i).state == ZZAPP_THREAD_RUNNABLE);
}

/* The API lock queue is FIFO by the order of asking, for pids that do not
 * fit a byte or a 32-bit mask. */
static void test_api_lock_handoff_with_pids_above_31_and_255(void)
{
	static const unsigned asking[3] = { 299u, 40u, 260u };
	zzapp_kernel_t kernel;
	unsigned i;

	kinit_n(&kernel, 300u);
	for (i = 0u; i < 3u; i++)
		start_range(&kernel, asking[i], 1u);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	for (i = 0u; i < 3u; i++) {
		CHECK(zzapp_kernel_api_lock(&kernel, asking[i]) != 0);
		CHECK(zzapp_kernel_block(&kernel, asking[i]) == 0);
		CHECK(WORDS(asking[i]).api_waiting == 1u);
	}
	/* No other record's word was touched (a byte slot would alias 260
	 * to 4 and 299 to 43). */
	CHECK(WORDS(4).api_waiting == 0u && WORDS(43).api_waiting == 0u);
	CHECK(zzapp_kernel_system_unlock(&kernel) == 0);
	for (i = 0u; i < 3u; i++) {
		CHECK(zzapp_kernel_schedule(&kernel, 0u) == (int)asking[i]);
		CHECK(kernel.api_handoff == asking[i]);
		CHECK(zzapp_kernel_api_lock(&kernel, asking[i]) == 0);
		CHECK(kernel.api_lock_owner == (int)asking[i]);
		CHECK(zzapp_kernel_api_unlock(&kernel, asking[i]) == 0);
	}
	CHECK(kernel.api_handoff == ZZAPP_PID_NONE);
	CHECK(kernel.api_lock_owner == -1);

	/* Stopping the owner hands the lock on. */
	CHECK(zzapp_kernel_api_lock(&kernel, 260u) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 40u) != 0);
	CHECK(zzapp_kernel_block(&kernel, 40u) == 0);
	zzapp_kernel_stop_app(&kernel, 260u);
	CHECK(THREAD(kernel, 40).state == ZZAPP_THREAD_RUNNABLE);
	CHECK(kernel.api_handoff == 40u);
}

/* Surfaces, audio, GL and sessions are per record, for any pid. */
static void test_per_record_resources_do_not_cross(void)
{
	zzapp_kernel_t kernel;
	unsigned i;

	kinit_n(&kernel, 300u);
	CHECK(zzapp_kernel_surface_bind(&kernel, 100u, 1u, 0x200000u, 0x2000u) == 0);
	CHECK(zzapp_kernel_surface_bind(&kernel, 200u, 2u, 0x201000u, 0x1000u) != 0);
	CHECK(zzapp_kernel_surface_bind(&kernel, 200u, 2u, 0x202000u, 0x1000u) == 0);
	CHECK(WORDS(100).resources.surface_base == 0x200000u);
	CHECK(WORDS(200).resources.surface_base == 0x202000u);
	for (i = 0u; i < ZZAPP_KERNEL_SESSION_LIMIT; i++)
		CHECK(zzapp_kernel_session_open(&kernel, 270u, 0) == 0);
	CHECK(zzapp_kernel_session_open(&kernel, 270u, 0) != 0);
	CHECK(zzapp_kernel_session_open(&kernel, 271u, 0) == 0);
	CHECK(WORDS(270).resources.image_sessions == ZZAPP_KERNEL_SESSION_LIMIT);
	CHECK(WORDS(271).resources.image_sessions == 1u);
	CHECK(zzapp_kernel_gl_bind(&kernel, 270u, 5u) == 0);
	CHECK(kernel.gl_owner == 270);
	CHECK(zzapp_kernel_gl_bind(&kernel, 271u, 6u) != 0);
	CHECK(zzapp_kernel_audio_open(&kernel, 299u, 1) == 0);
	zzapp_kernel_stop_app(&kernel, 100u);
	CHECK(WORDS(100).resources.surface_bytes == 0u);
	CHECK(WORDS(200).resources.surface_bytes == 0x1000u);
	CHECK(WORDS(270).resources.image_sessions == ZZAPP_KERNEL_SESSION_LIMIT);
	zzapp_kernel_stop_app(&kernel, 270u);
	CHECK(kernel.gl_owner == -1);
	CHECK(zzapp_kernel_gl_bind(&kernel, 271u, 6u) == 0);
	zzapp_kernel_stop_app(&kernel, 299u);
	CHECK(kernel.direct_audio_count == 0u);
}

/* The kernel stack of a thread is the record's: its top is what the SVC
 * vector builds frames below, its painted depth is the high water mark. */
static uint8_t g_stacks[3][ZZAPP_KERNEL_STACK_BYTES] __attribute__((aligned(8)));

static void test_stack_top_and_high_water_come_from_the_record(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	unsigned i;

	kinit_n(&kernel, 3u);
	memset(g_stacks, 0, sizeof(g_stacks));
	for (i = 0u; i < 3u; i++) {
		g_recs[i].hdr.kstack_base = (uint32_t)(uintptr_t)g_stacks[i];
		g_recs[i].hdr.kstack_top = g_recs[i].hdr.kstack_base +
					   ZZAPP_KERNEL_STACK_BYTES;
	}
	/* prepare_app paints its own record's stack and nothing else. */
	CHECK(zzapp_kernel_prepare_app(&kernel, 1, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	CHECK(g_stacks[1][0] == 0xa5u);
	CHECK(g_stacks[1][ZZAPP_KERNEL_STACK_BYTES - 1u] == 0xa5u);
	CHECK(g_stacks[0][0] == 0u && g_stacks[2][0] == 0u);
	CHECK(zzapp_kernel_stack_high_water(&kernel, 1u) == 0u);
	/* The app used the top 1234 bytes. */
	memset(g_stacks[1] + ZZAPP_KERNEL_STACK_BYTES - 1234u, 0x11, 1234u);
	CHECK(zzapp_kernel_stack_high_water(&kernel, 1u) == 1234u);
	/* An unpainted stack reads as fully used; the figure means something
	 * only after prepare_app painted the record's stack. */
	CHECK(zzapp_kernel_stack_high_water(&kernel, 0u) == ZZAPP_KERNEL_STACK_BYTES);
	CHECK(zzapp_kernel_stack_high_water(&kernel, 3u) == 0u);

	/* The scheduler publishes the record's top when it dispatches. */
	CHECK(zzapp_kernel_start_app(&kernel, 1u) == 0);
	frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	zzapp_kernel_set_current_thread(-1);
	clock_ms = 0;
	(void)zzapp_kernel_switch_frame(&kernel, &frame,
					ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(zzapp_kernel_current_thread() == 1);
	CHECK(zzapp_kernel_svc_stack_top == (uintptr_t)g_recs[1].hdr.kstack_top);
	zzapp_kernel_set_current_thread(-1);
	CHECK(zzapp_kernel_svc_stack_top == 0u);
	/* A new run paints it again. */
	CHECK(zzapp_kernel_prepare_app(&kernel, 1, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	CHECK(zzapp_kernel_stack_high_water(&kernel, 1u) == 0u);
	for (i = 0u; i < 3u; i++)
		g_recs[i].hdr.kstack_base = g_recs[i].hdr.kstack_top = 0u;
}

/* C mirror of the vector's counter update: it stores through
 * zzapp_svc_counter_block, so the pointer must follow the running thread. */
static void test_svc_counter_block_follows_current_thread(void)
{
#if ZZAPP_PERF_COUNTERS
	static const int pids[8] = { 0, 1, 7, 31, 32, 255, 256, 299 };
	zzapp_kernel_t kernel;
	void *idle;
	unsigned i;

	kinit_n(&kernel, 300u);
	zzapp_kernel_set_current_thread(-1);
	idle = zzapp_svc_counter_block;
	CHECK(idle != NULL);
	for (i = 0u; i < 8u; i++) {
		zzapp_kernel_set_current_thread(pids[i]);
		CHECK(zzapp_svc_counter_block ==
		      (void *)((uint8_t *)&g_recs[pids[i]] +
			       offsetof(zzapp_rec_t, svc)));
		CHECK(zzapp_svc_counter_block == (void *)g_recs[pids[i]].svc.bytes);
		/* The vector's first store: the entry stamp, offset 0. */
		*(volatile uint32_t *)zzapp_svc_counter_block =
			0xc0de0000u + (uint32_t)pids[i];
	}
	for (i = 0u; i < 8u; i++)
		CHECK(*(const uint32_t *)(const void *)g_recs[pids[i]].svc.bytes ==
		      0xc0de0000u + (uint32_t)pids[i]);
	/* No other record was touched. */
	CHECK(*(const uint32_t *)(const void *)g_recs[2].svc.bytes == 0u);
	CHECK(*(const uint32_t *)(const void *)g_recs[298].svc.bytes == 0u);

	/* System, idle and a pid past the capacity all count into the scratch
	 * block; none is a record's. */
	zzapp_kernel_set_current_thread(-2);
	CHECK(zzapp_svc_counter_block == idle);
	zzapp_kernel_set_current_thread(300);
	CHECK(zzapp_svc_counter_block == idle);
	CHECK(zzapp_kernel_svc_stack_top == 0u);

	/* The scheduler switches it with the thread. */
	{
		zzapp_arm_frame_t frame = { 0 };
		zzapp_arm_frame_t *next;

		kinit_n(&kernel, 300u);
		start_range(&kernel, 40u, 1u);
		start_range(&kernel, 260u, 1u);
		frame.spsr = ZZAPP_KERNEL_APP_CPSR;
		zzapp_kernel_set_current_thread(-1);
		clock_ms = 0;
		next = zzapp_kernel_switch_frame(&kernel, &frame,
						 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
		CHECK(zzapp_kernel_current_thread() == 40);
		CHECK(zzapp_svc_counter_block == (void *)g_recs[40].svc.bytes);
		clock_ms = 5;
		next = zzapp_kernel_switch_frame(&kernel, next,
						 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
		CHECK(zzapp_kernel_current_thread() == 260);
		CHECK(zzapp_svc_counter_block == (void *)g_recs[260].svc.bytes);
		zzapp_kernel_service_due(&kernel);
		next = zzapp_kernel_switch_frame(&kernel, next,
						 ZZAPP_KERNEL_DISPATCH_DOORBELL, 0u);
		(void)next;
	}
	zzapp_kernel_set_current_thread(-1);
	CHECK(ZZAPP_SVC_COUNTER_BLOCK_BYTES == 184u);
#endif
}

#if ZZAPP_PERF_COUNTERS
/* Report requests are per-record flags: pid 40 (and 255) work, where a
 * 32-bit mask bit would have shifted out of range or aliased another pid. */
static void test_perf_report_requests_for_large_pids(void)
{
	zzapp_kernel_t kernel;

	kinit_n(&kernel, 300u);
	zzapp_kernel_perf_request_report(&kernel, 40u, 1);
	CHECK(WORDS(40).perf_report_pending == 1u);
	CHECK(WORDS(40).perf_report_force == 1u);
	CHECK(WORDS(8).perf_report_pending == 0u);
	CHECK(kernel.perf.report_pending == 1u && kernel.perf.report_force == 1u);
	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 1);
	CHECK(strstr(kernel.perf.report_text, "slot=40 forced=1") != NULL);
	zzapp_kernel_perf_print_reports(&kernel);
	CHECK(WORDS(40).perf_report_pending == 0u && WORDS(40).perf_report_force == 0u);
	CHECK(kernel.perf.report_pending == 0u && kernel.perf.report_force == 0u);
	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 0);

	/* Two plain requests: the lowest pid goes first, the other stays. */
	kernel.now += 10000u;
	zzapp_kernel_perf_request_report(&kernel, 255u, 0);
	zzapp_kernel_perf_request_report(&kernel, 40u, 0);
	CHECK(kernel.perf.report_pending == 2u);
	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 1);
	CHECK(strstr(kernel.perf.report_text, "slot=40 forced=0") != NULL);
	zzapp_kernel_perf_print_reports(&kernel);
	CHECK(WORDS(255).perf_report_pending == 1u && kernel.perf.report_pending == 1u);
	kernel.now += 10000u;
	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 1);
	CHECK(strstr(kernel.perf.report_text, "slot=255 forced=0") != NULL);
	zzapp_kernel_perf_print_reports(&kernel);

	/* A forced report of a stopped record still goes out. */
	zzapp_kernel_perf_request_report(&kernel, 299u, 1);
	CHECK(THREAD(kernel, 299).state == ZZAPP_THREAD_STOPPED);
	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 1);
	CHECK(strstr(kernel.perf.report_text, "slot=299 forced=1") != NULL);
	zzapp_kernel_perf_print_reports(&kernel);

	/* A count whose record was cleared since is dropped, not stuck. */
	kernel.now += 10000u;
	kernel.perf.report_pending = 1u;
	kernel.perf.report_force = 1u;
	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 0);
	CHECK(kernel.perf.report_pending == 0u && kernel.perf.report_force == 0u);
}
#endif

int main(void)
{
    test_first_context_and_dispatch();
    test_frame_switch();
    test_system_frame_runs_service_before_apps();
    test_round_robin_quantum();
    test_ticks_through_system_thread_share_fairly();
    test_tick_charges_the_context_that_ran();
    test_blocking_after_3ms_charges_3();
    test_wfe_idle_gap_charges_nothing();
    test_recovery_gap_charges_nothing();
    test_api_sleep_atomic_against_injected_irq();
    test_idle_irq_no_double_charge();
    test_service_precedes_app_and_preserves_remainder();
    test_block_wake_and_idle();
    test_sleep_deadline_order();
    test_wait_completion_timeout_and_cancel();
    test_accounting_and_throttle();
    test_new_generation_resets_cpu_ticks();
    test_blocking_api_lock_and_slot_resources();
    test_waiter_isolation_timeout_and_stop();
    test_stop_wakes_api_lock_waiter();
    test_system_kernel_lock_handoffs();
    test_woken_api_waiter_beats_continuous_service();
    test_api_waiter_handoff_beats_continuous_service();
    test_api_lock_handoff_is_fifo();
    test_pending_service_runs_between_api_handoffs();
    test_service_budget_preserves_deferred_handoff();
    test_fault_releases_kernel_lock();
    test_kernel_stack_covers_decoder_call_chain();
    test_non_timer_interrupt_does_not_advance_time();
    test_system_ticks_wake_sleepers();
    test_restart_releases_kernel_lock_waiters();
    test_system_lock_inheritance_is_throttled();
    test_wake_hands_back_to_runnable_app();
    test_init_rejects_bad_areas();
    test_system_only_kernel();
    test_init_clears_threads_left_by_a_previous_run();
    test_run_list_follows_start_and_stop();
    test_64_threads_round_robin_through_run_list();
    test_sparse_large_pids_round_robin();
    test_yield_skips_the_yielder();
    test_64_sleepers_wake_in_deadline_order();
    test_64_sleepers_with_equal_deadlines_wake_in_sleep_order();
    test_wake_log_keeps_pids_above_255();
    test_api_lock_handoff_with_pids_above_31_and_255();
    test_per_record_resources_do_not_cross();
    test_stack_top_and_high_water_come_from_the_record();
    test_svc_counter_block_follows_current_thread();
#if ZZAPP_PERF_COUNTERS
    test_performance_counters();
    test_tlb_samples_survive_system_dispatch();
    test_tlb_window_expiry_drops_system_baseline();
    test_perf_report_requests_for_large_pids();
#endif
    printf("kernel_sched_test: %u checks, %d failures\n", checks, failures);
    return failures != 0;
}
