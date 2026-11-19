/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Weighted fair share of the core-1 scheduler: the runnable app with the
 * smallest virtual run time runs next, and every charged interval adds its
 * run time over the app's weight. The scenarios drive zzapp_kernel_switch_frame
 * the way the timer vector does, over an injected microsecond clock, with CPU-
 * bound apps that use whatever slice they are given.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zzapp_kernel.h"
#include "zzapp_proto.h"
#include "zzapp_rec.h"

static uint64_t clock_us;

uint64_t zzapp_kernel_now_us(void)
{
	return clock_us;
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

#define TEST_RECS 12u
#define TICK_US 1000u
#define QUANTUM_US (ZZAPP_KERNEL_QUANTUM_MS * 1000U)

static zzapp_rec_t g_recs[TEST_RECS];

#define THREAD(k, pid) (*zzapp_kernel_thread(&(k), (pid)))

typedef struct {
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame;
	/* Microseconds each pid held the core, from the model's side. */
	uint64_t ran_us[TEST_RECS];
	uint64_t last_floor;
	/* The app that last held the core, and the changes of app since:
	 * all of them, and those that took the core from an app that was
	 * still runnable. */
	int last_app;
	uint64_t switches;
	uint64_t preemptions;
} sim_t;

static sim_t g_sim;

/* A scheduler over `count` records; apps 0..count-1 get the weights and
 * start. The first dispatch is made the way the system thread hands the core
 * to the first app. */
static void sim_boot(sim_t *sim, uint32_t records)
{
	memset(sim, 0, sizeof(*sim));
	sim->last_app = -1;
	clock_us = 0U;
	CHECK(zzapp_kernel_init(&sim->kernel, g_recs, sizeof(g_recs[0]),
				records) == 0);
	sim->frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	zzapp_kernel_set_current_thread(-1);
}

static void sim_add(sim_t *sim, uint32_t pid, uint32_t weight)
{
	CHECK(zzapp_kernel_set_weight(&sim->kernel, pid, weight) == 0);
	CHECK(zzapp_kernel_start_app(&sim->kernel, pid) == 0);
}

static void sim_note_switch(sim_t *sim)
{
	int cur = zzapp_kernel_current_thread();

	if (cur < 0)
		return;
	if (sim->last_app >= 0 && sim->last_app != cur) {
		sim->switches++;
		if (THREAD(sim->kernel, (uint32_t)sim->last_app).state ==
		    ZZAPP_THREAD_RUNNABLE)
			sim->preemptions++;
	}
	sim->last_app = cur;
}

static void sim_first_dispatch(sim_t *sim)
{
	(void)zzapp_kernel_switch_frame(&sim->kernel, &sim->frame,
					ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0U);
	sim_note_switch(sim);
}

static void sim_start(sim_t *sim, const uint32_t *weights, uint32_t count)
{
	uint32_t i;

	sim_boot(sim, TEST_RECS);
	for (i = 0U; i < count; i++)
		sim_add(sim, i, weights[i]);
	sim_first_dispatch(sim);
}

/* `us` microseconds pass in the current context, then `source` switches. */
static void sim_step(sim_t *sim, uint64_t us, zzapp_kernel_dispatch_t source)
{
	int cur = zzapp_kernel_current_thread();

	if (cur >= 0)
		sim->ran_us[cur] += us;
	clock_us += us;
	/* The vector restores the next context into the frame; what ran just
	 * now ran in user mode. */
	sim->frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	(void)zzapp_kernel_switch_frame(&sim->kernel, &sim->frame, source, 4U);
	sim_note_switch(sim);
	/* The floor never goes back. */
	CHECK(sim->kernel.vfloor >= sim->last_floor);
	sim->last_floor = sim->kernel.vfloor;
}

static void sim_run(sim_t *sim, uint64_t total_us, uint32_t tick_us)
{
	uint64_t done;

	for (done = 0U; done < total_us; done += tick_us)
		sim_step(sim, tick_us, ZZAPP_KERNEL_DISPATCH_TICK);
}

static void sim_clear(sim_t *sim)
{
	memset(sim->ran_us, 0, sizeof(sim->ran_us));
}

static uint64_t abs_diff(uint64_t a, uint64_t b)
{
	return a > b ? a - b : b - a;
}

/* Every app got its weight's share of `total_us` within 2 percent of that
 * share. */
static void check_proportional(const sim_t *sim, const uint32_t *weights,
			       uint32_t count, uint64_t total_us)
{
	uint64_t sum = 0U;
	uint64_t ran = 0U;
	uint32_t i;

	for (i = 0U; i < count; i++)
		sum += weights[i];
	for (i = 0U; i < count; i++) {
		uint64_t expect = total_us * weights[i] / sum;

		ran += sim->ran_us[i];
		if (abs_diff(sim->ran_us[i], expect) * 50U > expect) {
			printf("app %u weight %u: ran %llu us, expected %llu\n",
			       (unsigned)i, (unsigned)weights[i],
			       (unsigned long long)sim->ran_us[i],
			       (unsigned long long)expect);
			CHECK(0);
		}
	}
	CHECK(ran == total_us);
}

/* Within `slack_us` of the weight's share of `total_us`. */
static void check_share_within(const sim_t *sim, uint32_t pid, uint32_t weight,
			       uint32_t weight_sum, uint64_t total_us,
			       uint64_t slack_us)
{
	uint64_t expect = total_us * weight / weight_sum;

	if (abs_diff(sim->ran_us[pid], expect) > slack_us) {
		printf("app %u: ran %llu us, expected %llu +- %llu\n",
		       (unsigned)pid, (unsigned long long)sim->ran_us[pid],
		       (unsigned long long)expect, (unsigned long long)slack_us);
		CHECK(0);
	}
}

/* (a) CPU-bound apps converge to their weights. */
static void test_shares_follow_weights(void)
{
	static const uint32_t two[] = { 1U, 3U };
	static const uint32_t three[] = { 1U, 2U, 4U };
	static const uint32_t ten[] = { 1U, 1U, 2U, 2U, 3U, 4U, 5U, 8U, 16U, 32U };
	const uint64_t total = 120ULL * 1000000U;

	sim_start(&g_sim, two, 2U);
	sim_run(&g_sim, total, TICK_US);
	check_proportional(&g_sim, two, 2U, total);

	sim_start(&g_sim, three, 3U);
	sim_run(&g_sim, total, TICK_US);
	check_proportional(&g_sim, three, 3U, total);

	sim_start(&g_sim, ten, 10U);
	sim_run(&g_sim, total, TICK_US);
	check_proportional(&g_sim, ten, 10U, total);
}

/* The widest spread of weights the claim allows. */
static void test_weight_bounds(void)
{
	static const uint32_t wide[] = { 1U, ZZAPP_WEIGHT_MAX };
	const uint64_t total = 300ULL * 1000000U;
	zzapp_kernel_t kernel;

	sim_start(&g_sim, wide, 2U);
	sim_run(&g_sim, total, TICK_US);
	check_proportional(&g_sim, wide, 2U, total);

	/* 0 is the default of 1; nothing above the bound, no record past the
	 * capacity. */
	CHECK(zzapp_kernel_init(&kernel, g_recs, sizeof(g_recs[0]), 4U) == 0);
	CHECK(zzapp_kernel_set_weight(&kernel, 0U, 0U) == 0);
	CHECK(THREAD(kernel, 0).weight == 0U);
	CHECK(zzapp_kernel_set_weight(&kernel, 0U, ZZAPP_WEIGHT_MAX) == 0);
	CHECK(THREAD(kernel, 0).weight == ZZAPP_WEIGHT_MAX);
	CHECK(zzapp_kernel_set_weight(&kernel, 0U, ZZAPP_WEIGHT_MAX + 1U) == -1);
	CHECK(THREAD(kernel, 0).weight == ZZAPP_WEIGHT_MAX);
	CHECK(zzapp_kernel_set_weight(&kernel, 4U, 1U) == -1);
}

/* An app never given a weight and an app with weight 1 share equally. */
static void test_unset_weight_counts_as_one(void)
{
	const uint64_t total = 20ULL * 1000000U;

	sim_boot(&g_sim, TEST_RECS);
	CHECK(zzapp_kernel_start_app(&g_sim.kernel, 0U) == 0);
	sim_add(&g_sim, 1U, 1U);
	sim_first_dispatch(&g_sim);
	sim_run(&g_sim, total, TICK_US);
	check_share_within(&g_sim, 0U, 1U, 2U, total, 2U * QUANTUM_US);
	check_share_within(&g_sim, 1U, 1U, 2U, total, 2U * QUANTUM_US);
}

/* Equal virtual run times take turns in pid order. */
static void test_equal_weights_rotate_in_pid_order(void)
{
	static const uint32_t same[] = { 1U, 1U, 1U };
	uint32_t round;
	uint32_t pid;

	sim_start(&g_sim, same, 3U);
	CHECK(zzapp_kernel_current_thread() == 0);
	for (round = 0U; round < 20U; round++) {
		for (pid = 0U; pid < 3U; pid++) {
			CHECK(zzapp_kernel_current_thread() == (int)pid);
			sim_step(&g_sim, QUANTUM_US, ZZAPP_KERNEL_DISPATCH_TICK);
		}
	}
}

static uint32_t weight_sum(const uint32_t *weights, uint32_t count)
{
	uint32_t sum = 0U;
	uint32_t i;

	for (i = 0U; i < count; i++)
		sum += weights[i];
	return sum;
}

/* Runs `window_us` and checks each app of `weights` is within `slack_us` of
 * its share. */
static void check_window(sim_t *sim, const uint32_t *weights, uint32_t count,
			 uint64_t window_us, uint64_t slack_us)
{
	uint32_t i;

	sim_clear(sim);
	sim_run(sim, window_us, TICK_US);
	for (i = 0U; i < count; i++)
		check_share_within(sim, i, weights[i], weight_sum(weights, count),
				   window_us, slack_us);
}

/* The window after a wake or a join, in which each other app has to run. */
static void check_others_run_early(sim_t *sim, uint32_t newcomer,
				   const uint32_t *weights, uint32_t count,
				   uint64_t early_us)
{
	uint32_t i;
	uint32_t sum = weight_sum(weights, count);

	sim_clear(sim);
	sim_run(sim, early_us, TICK_US);
	for (i = 0U; i < count; i++) {
		uint64_t wanted;

		if (i == newcomer)
			continue;
		/* Half of the share is enough to show the app was not
		 * starved; the newcomer's credit and the rotation cost the
		 * rest. */
		wanted = early_us * weights[i] / sum / 2U;
		if (sim->ran_us[i] < wanted) {
			printf("app %u starved after the newcomer: ran %llu us, "
			       "wanted %llu\n", (unsigned)i,
			       (unsigned long long)sim->ran_us[i],
			       (unsigned long long)wanted);
			CHECK(0);
		}
	}
}

/* (b) An app that was blocked for a minute comes back to its share, no more,
 * and the others keep theirs. */
static void test_wake_after_long_block(void)
{
	static const uint32_t equal[] = { 1U, 1U, 1U };
	static const uint32_t heavy[] = { 1U, 1U, 4U };
	const uint64_t window = 3ULL * 1000000U;
	const uint64_t slack = 3U * QUANTUM_US;

	sim_start(&g_sim, equal, 3U);
	sim_run(&g_sim, 1000000U, TICK_US);
	CHECK(zzapp_kernel_block(&g_sim.kernel, 2U) == 0);
	sim_run(&g_sim, 60ULL * 1000000U, TICK_US);
	CHECK(THREAD(g_sim.kernel, 2U).state == ZZAPP_THREAD_BLOCKED);
	CHECK(zzapp_kernel_wake(&g_sim.kernel, 2U, ZZAPP_WAKE_READY) == 0);
	check_others_run_early(&g_sim, 2U, equal, 3U, 60000U);
	check_window(&g_sim, equal, 3U, window, slack);

	/* A heavier waker is held to its own share the same way. */
	sim_start(&g_sim, heavy, 3U);
	sim_run(&g_sim, 1000000U, TICK_US);
	CHECK(zzapp_kernel_block(&g_sim.kernel, 2U) == 0);
	sim_run(&g_sim, 60ULL * 1000000U, TICK_US);
	CHECK(zzapp_kernel_wake(&g_sim.kernel, 2U, ZZAPP_WAKE_READY) == 0);
	check_others_run_early(&g_sim, 2U, heavy, 3U, 60000U);
	check_window(&g_sim, heavy, 3U, window, slack);
}

/* The same for a sleeper the timer wakes at its deadline. */
static void test_wake_after_long_sleep(void)
{
	static const uint32_t equal[] = { 1U, 1U, 1U };
	const uint64_t window = 3ULL * 1000000U;
	uint64_t guard;

	sim_start(&g_sim, equal, 3U);
	sim_run(&g_sim, 1000000U, TICK_US);
	CHECK(zzapp_kernel_sleep(&g_sim.kernel, 2U,
				 zzapp_kernel_now(&g_sim.kernel) + 60000U) == 0);
	for (guard = 0U; THREAD(g_sim.kernel, 2U).state == ZZAPP_THREAD_SLEEPING &&
	     guard < 100000U; guard++)
		sim_step(&g_sim, TICK_US, ZZAPP_KERNEL_DISPATCH_TICK);
	CHECK(THREAD(g_sim.kernel, 2U).state == ZZAPP_THREAD_RUNNABLE);
	CHECK(THREAD(g_sim.kernel, 2U).wake_reason == ZZAPP_WAKE_TIMEOUT);
	check_others_run_early(&g_sim, 2U, equal, 3U, 60000U);
	check_window(&g_sim, equal, 3U, window, 3U * QUANTUM_US);
}

/* A wake does not hand out more than one quantum of credit at the app's
 * weight, however long the app was away. */
static void test_wake_credit_is_one_quantum(void)
{
	static const uint32_t pair[] = { 1U, 4U };
	uint64_t floor_vr;

	sim_start(&g_sim, pair, 2U);
	sim_run(&g_sim, 1000000U, TICK_US);
	CHECK(zzapp_kernel_block(&g_sim.kernel, 1U) == 0);
	sim_run(&g_sim, 30ULL * 1000000U, TICK_US);
	floor_vr = THREAD(g_sim.kernel, 0U).vruntime;
	CHECK(zzapp_kernel_wake(&g_sim.kernel, 1U, ZZAPP_WAKE_READY) == 0);
	/* The waker's weight is 4: a quantum of it is a quarter of the charge
	 * of a quantum at weight 1, below the floor of the apps that ran. */
	CHECK(THREAD(g_sim.kernel, 1U).vruntime ==
	      floor_vr - QUANTUM_US * ZZAPP_KERNEL_VRUNTIME_SCALE / 4U);
}

/* A short sleeper keeps what it earned: waking brings it back no further
 * than where it left. */
static void test_wake_keeps_a_larger_virtual_time(void)
{
	static const uint32_t pair[] = { 1U, 1U };
	uint64_t before;

	sim_start(&g_sim, pair, 2U);
	sim_run(&g_sim, 100000U, TICK_US);
	CHECK(zzapp_kernel_block(&g_sim.kernel, 1U) == 0);
	THREAD(g_sim.kernel, 1U).vruntime += 500000ULL * ZZAPP_KERNEL_VRUNTIME_SCALE;
	before = THREAD(g_sim.kernel, 1U).vruntime;
	CHECK(zzapp_kernel_wake(&g_sim.kernel, 1U, ZZAPP_WAKE_READY) == 0);
	CHECK(THREAD(g_sim.kernel, 1U).vruntime == before);
}

/* (c) Parked and blocked apps are skipped: they run nothing and are charged
 * nothing. */
static void test_parked_apps_get_nothing(void)
{
	static const uint32_t three[] = { 1U, 1U, 1U };
	const uint64_t total = 10ULL * 1000000U;
	uint64_t blocked_vr;
	uint64_t sleeping_vr;

	sim_boot(&g_sim, TEST_RECS);
	sim_add(&g_sim, 0U, three[0]);
	sim_add(&g_sim, 1U, three[1]);
	sim_add(&g_sim, 2U, three[2]);
	CHECK(zzapp_kernel_block(&g_sim.kernel, 1U) == 0);
	CHECK(zzapp_kernel_sleep(&g_sim.kernel, 2U,
				 zzapp_kernel_now(&g_sim.kernel) + 1000000U) == 0);
	blocked_vr = THREAD(g_sim.kernel, 1U).vruntime;
	sleeping_vr = THREAD(g_sim.kernel, 2U).vruntime;
	sim_first_dispatch(&g_sim);
	sim_run(&g_sim, total, TICK_US);
	CHECK(g_sim.ran_us[0] == total);
	CHECK(g_sim.ran_us[1] == 0U && g_sim.ran_us[2] == 0U);
	CHECK(THREAD(g_sim.kernel, 1U).user_ticks == 0U);
	CHECK(THREAD(g_sim.kernel, 2U).user_ticks == 0U);
	CHECK(THREAD(g_sim.kernel, 1U).vruntime == blocked_vr);
	CHECK(THREAD(g_sim.kernel, 2U).vruntime == sleeping_vr);
	/* A record that was never started is not on the run list. */
	CHECK(THREAD(g_sim.kernel, 3U).state == ZZAPP_THREAD_STOPPED);
	CHECK(THREAD(g_sim.kernel, 3U).user_ticks == 0U);
	CHECK(zzapp_kernel_run_count(&g_sim.kernel) == 3U);

	/* With every app parked the core idles. */
	CHECK(zzapp_kernel_block(&g_sim.kernel, 0U) == 0);
	sim_step(&g_sim, TICK_US, ZZAPP_KERNEL_DISPATCH_TICK);
	CHECK(zzapp_kernel_apps_runnable(&g_sim.kernel) == 0);
}

/* (d) An app that starts late joins at the others' virtual time: it neither
 * waits for the others to catch up with an empty history nor runs alone to
 * make up for the time it was not there. */
static void test_late_join(void)
{
	static const uint32_t pair[] = { 1U, 1U };
	static const uint32_t trio[] = { 1U, 1U, 1U };
	static const uint32_t heavy[] = { 1U, 1U, 2U };
	const uint64_t window = 3ULL * 1000000U;
	const uint64_t slack = 3U * QUANTUM_US;

	sim_start(&g_sim, pair, 2U);
	sim_run(&g_sim, 10ULL * 1000000U, TICK_US);
	sim_add(&g_sim, 2U, 1U);
	check_others_run_early(&g_sim, 2U, trio, 3U, 60000U);
	check_window(&g_sim, trio, 3U, window, slack);

	sim_start(&g_sim, pair, 2U);
	sim_run(&g_sim, 10ULL * 1000000U, TICK_US);
	sim_add(&g_sim, 2U, 2U);
	check_others_run_early(&g_sim, 2U, heavy, 3U, 60000U);
	check_window(&g_sim, heavy, 3U, window, slack);
}

/* With nothing runnable a new app starts at the floor the scheduler kept, and
 * not at zero. */
static void test_join_into_an_empty_run_set_starts_at_the_floor(void)
{
	static const uint32_t pair[] = { 1U, 1U };
	uint64_t floor_vr;

	sim_start(&g_sim, pair, 2U);
	sim_run(&g_sim, 2ULL * 1000000U, TICK_US);
	CHECK(zzapp_kernel_block(&g_sim.kernel, 0U) == 0);
	CHECK(zzapp_kernel_block(&g_sim.kernel, 1U) == 0);
	sim_step(&g_sim, TICK_US, ZZAPP_KERNEL_DISPATCH_TICK);
	CHECK(zzapp_kernel_apps_runnable(&g_sim.kernel) == 0);
	floor_vr = g_sim.kernel.vfloor;
	CHECK(floor_vr > 0U);
	sim_add(&g_sim, 2U, 1U);
	CHECK(THREAD(g_sim.kernel, 2U).vruntime == floor_vr);
}

/* (e) A yield sends the app behind its runnable peers for the next pick, even
 * when its virtual run time is the smallest. */
static void test_yield_goes_behind_peers(void)
{
	static const uint32_t pair[] = { 1U, 1U };
	int i;

	sim_start(&g_sim, pair, 2U);
	/* The peer is well ahead, so the yielder stays the smallest. */
	THREAD(g_sim.kernel, 1U).vruntime = 1000000ULL * ZZAPP_KERNEL_VRUNTIME_SCALE;
	CHECK(zzapp_kernel_current_thread() == 0);
	zzapp_kernel_yield(&g_sim.kernel);
	CHECK(THREAD(g_sim.kernel, 0U).yield_pending == 1U);
	sim_step(&g_sim, 0U, ZZAPP_KERNEL_DISPATCH_VOLUNTARY);
	CHECK(zzapp_kernel_current_thread() == 1);
	CHECK(THREAD(g_sim.kernel, 0U).yield_pending == 0U);
	/* Without a yield the smallest runs again. */
	sim_step(&g_sim, 0U, ZZAPP_KERNEL_DISPATCH_VOLUNTARY);
	CHECK(zzapp_kernel_current_thread() == 0);
	/* Yielders in a row alternate, and neither spins on the core. */
	for (i = 0; i < 10; i++) {
		int cur = zzapp_kernel_current_thread();

		zzapp_kernel_yield(&g_sim.kernel);
		sim_step(&g_sim, 0U, ZZAPP_KERNEL_DISPATCH_VOLUNTARY);
		CHECK(zzapp_kernel_current_thread() == 1 - cur);
	}
	/* A yield with no peer runnable keeps the core. */
	CHECK(zzapp_kernel_block(&g_sim.kernel, 1U) == 0);
	sim_step(&g_sim, 0U, ZZAPP_KERNEL_DISPATCH_VOLUNTARY);
	CHECK(zzapp_kernel_current_thread() == 0);
	zzapp_kernel_yield(&g_sim.kernel);
	CHECK(THREAD(g_sim.kernel, 0U).yield_pending == 0U);
	sim_step(&g_sim, 0U, ZZAPP_KERNEL_DISPATCH_VOLUNTARY);
	CHECK(zzapp_kernel_current_thread() == 0);
}

/* An app that yields after every millisecond goes behind its peer each time,
 * so it runs less than the peer, but it keeps running and the peer is not
 * starved either. */
static void test_yielding_peer_does_not_starve_the_other(void)
{
	static const uint32_t pair[] = { 1U, 1U };
	const uint64_t total = 10ULL * 1000000U;
	uint64_t done;

	sim_start(&g_sim, pair, 2U);
	for (done = 0U; done < total; done += TICK_US) {
		/* App 0 gives the core back after every millisecond, app 1
		 * runs its quantum. */
		if (zzapp_kernel_current_thread() == 0) {
			zzapp_kernel_yield(&g_sim.kernel);
			sim_step(&g_sim, TICK_US, ZZAPP_KERNEL_DISPATCH_VOLUNTARY);
		} else {
			sim_step(&g_sim, TICK_US, ZZAPP_KERNEL_DISPATCH_TICK);
		}
	}
	CHECK(g_sim.ran_us[0] >= total / 10U);
	CHECK(g_sim.ran_us[1] >= total / 2U);
	CHECK(g_sim.ran_us[0] + g_sim.ran_us[1] == total);
}

/* (f) A slice shorter than a millisecond is charged. */
static void test_sub_millisecond_slices_are_charged(void)
{
	static const uint32_t pair[] = { 1U, 3U };
	const uint64_t total = 60ULL * 1000000U;
	uint64_t third;

	sim_start(&g_sim, pair, 2U);
	CHECK(zzapp_kernel_current_thread() == 0);
	sim_step(&g_sim, 400U, ZZAPP_KERNEL_DISPATCH_VOLUNTARY);
	CHECK(THREAD(g_sim.kernel, 0U).vruntime == 400ULL * ZZAPP_KERNEL_VRUNTIME_SCALE);
	/* No whole millisecond went by: nothing for the tick counters. */
	CHECK(THREAD(g_sim.kernel, 0U).user_ticks == 0U);
	CHECK(zzapp_kernel_current_thread() == 1);
	sim_step(&g_sim, 250U, ZZAPP_KERNEL_DISPATCH_VOLUNTARY);
	third = 250ULL * ZZAPP_KERNEL_VRUNTIME_SCALE / 3U;
	CHECK(THREAD(g_sim.kernel, 1U).vruntime == third);
	CHECK(THREAD(g_sim.kernel, 1U).user_ticks == 0U);

	/* Many slices of a quarter millisecond, as a pair of apps that call the
	 * API all the time make: the shares still follow the weights. */
	sim_start(&g_sim, pair, 2U);
	for (clock_us = 0U; clock_us < total;)
		sim_step(&g_sim, 250U, ZZAPP_KERNEL_DISPATCH_VOLUNTARY);
	CHECK(g_sim.ran_us[0] + g_sim.ran_us[1] == clock_us);
	check_proportional(&g_sim, pair, 2U, clock_us);

	/* The tick counters still count whole milliseconds across slices. */
	CHECK(THREAD(g_sim.kernel, 0U).user_ticks +
	      THREAD(g_sim.kernel, 1U).user_ticks == clock_us / 1000U);
}

/* (g) The system thread still comes first and the apps keep their shares with
 * service due at every tick. */
static void test_system_thread_first_and_shares_hold(void)
{
	static const uint32_t pair[] = { 1U, 3U };
	const uint64_t total = 60ULL * 1000000U;
	uint64_t done;
	unsigned system_turns = 0U;

	sim_start(&g_sim, pair, 2U);
	for (done = 0U; done < total; done += 5000U) {
		zzapp_kernel_service_due(&g_sim.kernel);
		sim_step(&g_sim, 5000U, ZZAPP_KERNEL_DISPATCH_TICK);
		CHECK(zzapp_kernel_current_thread() == -1);
		if (zzapp_kernel_current_thread() != -1)
			return;
		system_turns++;
		sim_step(&g_sim, 0U, ZZAPP_KERNEL_DISPATCH_VOLUNTARY);
		CHECK(zzapp_kernel_current_thread() >= 0);
	}
	CHECK(system_turns == total / 5000U);
	check_proportional(&g_sim, pair, 2U, total);
}

/* The card's timer tick, and the wake granularity as a run time. The kernel's
 * own constant is checked where it exists. */
#define CARD_TICK_US 5000U
#define GRAN_US 1000U
#ifdef ZZAPP_KERNEL_WAKE_GRAN_US
_Static_assert(ZZAPP_KERNEL_WAKE_GRAN_US == GRAN_US, "test granularity");
#endif

/* A service pass: a tick or the doorbell puts the system thread on the core,
 * it wakes `wake` (a pid, or -1) the way completion service does, and it
 * hands the core back, which is where the next pick is made. */
static void sim_service(sim_t *sim, int wake)
{
	zzapp_kernel_service_due(&sim->kernel);
	sim_step(sim, 0U, ZZAPP_KERNEL_DISPATCH_TICK);
	CHECK(zzapp_kernel_current_thread() == -1);
	if (wake >= 0)
		CHECK(zzapp_kernel_wake(&sim->kernel, (uint32_t)wake,
					ZZAPP_WAKE_READY) == 0);
	sim_step(sim, 0U, ZZAPP_KERNEL_DISPATCH_VOLUNTARY);
}

/* `us` microseconds pass in the current context; the switch comes later. */
static void sim_run_for(sim_t *sim, uint64_t us)
{
	int cur = zzapp_kernel_current_thread();

	if (cur >= 0)
		sim->ran_us[cur] += us;
	clock_us += us;
}

static uint64_t g_ia_max_delay;
static uint64_t g_ia_wakes;

/* App 0 is CPU-bound at weight 1. App 1, of weight `i_weight`, is the
 * interactive one: it runs `run_us`, blocks on a client, and is woken `gap_us`
 * later by a service pass, over and over. The time from each wake to the
 * app's next run is kept in g_ia_max_delay, the wakes in g_ia_wakes. Time
 * moves in 100 us steps, with a tick every CARD_TICK_US. */
static void run_interactive(sim_t *sim, uint32_t i_weight, uint32_t run_us,
			    uint32_t gap_us, uint64_t total_us)
{
	const uint32_t dt = 100U;
	uint32_t weights[2];
	uint32_t left = run_us;
	uint64_t wake_at = 0U;
	uint64_t woke_at = 0U;
	uint64_t done;
	int blocked = 0;
	int waiting = 0;

	weights[0] = 1U;
	weights[1] = i_weight;
	sim_start(sim, weights, 2U);
	g_ia_max_delay = 0U;
	g_ia_wakes = 0U;
	for (done = 0U; done < total_us; done += dt) {
		if (waiting && zzapp_kernel_current_thread() == 1) {
			if (clock_us - woke_at > g_ia_max_delay)
				g_ia_max_delay = clock_us - woke_at;
			waiting = 0;
		}
		sim_run_for(sim, dt);
		if (zzapp_kernel_current_thread() == 1) {
			left -= left < dt ? left : dt;
			if (left == 0U) {
				CHECK(zzapp_kernel_block(&sim->kernel, 1U) == 0);
				blocked = 1;
				wake_at = clock_us + gap_us;
				left = run_us;
				/* The blocking call hands the core on. */
				sim_step(sim, 0U, ZZAPP_KERNEL_DISPATCH_VOLUNTARY);
			}
		}
		if (blocked && clock_us >= wake_at) {
			blocked = 0;
			sim_service(sim, 1);
			waiting = 1;
			woke_at = clock_us;
			g_ia_wakes++;
		} else if (clock_us % CARD_TICK_US == 0U) {
			sim_service(sim, -1);
		}
	}
}

/* What the interactive app asks for is its run time per cycle; what it can
 * have is its weight's share. It gets at least nine tenths of the smaller,
 * and its wake-to-run delay stays within `delay_us`. */
static void check_interactive(uint32_t i_weight, uint32_t run_us,
			      uint32_t gap_us, uint64_t delay_us)
{
	const uint64_t total = 2000000U;
	uint64_t ideal;
	uint64_t share;
	uint64_t demand;

	run_interactive(&g_sim, i_weight, run_us, gap_us, total);
	ideal = total * run_us / (run_us + gap_us);
	share = total * i_weight / (i_weight + 1U);
	demand = ideal < share ? ideal : share;
	if (g_sim.ran_us[1] * 10U < demand * 9U || g_ia_max_delay > delay_us)
		printf("weight %u run %u gap %u: ran %llu us of %llu, delay %llu us, "
		       "preemptions %llu\n", (unsigned)i_weight, (unsigned)run_us,
		       (unsigned)gap_us, (unsigned long long)g_sim.ran_us[1],
		       (unsigned long long)demand,
		       (unsigned long long)g_ia_max_delay,
		       (unsigned long long)g_sim.preemptions);
	CHECK(g_sim.ran_us[1] * 10U >= demand * 9U);
	CHECK(g_ia_max_delay <= delay_us);
	CHECK(g_sim.ran_us[0] + g_sim.ran_us[1] == total);
	/* The spinner keeps what the interactive app cannot have. */
	if (ideal >= share)
		CHECK(g_sim.ran_us[0] * 10U >= (total - share) * 9U);
}

/* An app that blocks on its client several times a frame gets the core back
 * when its wake is served, not after the rest of the spinner's quantum. */
static void test_interactive_app_runs_when_woken(void)
{
	/* Wants 91 percent of the core, can have half of it. */
	check_interactive(1U, 1000U, 100U, CARD_TICK_US);
	/* Wants 91 percent, can have eight ninths of it. */
	check_interactive(8U, 1000U, 100U, CARD_TICK_US);
	/* Wants a sixth of the core: all of it, and at once. */
	check_interactive(1U, 200U, 1000U, 100U);
	check_interactive(8U, 200U, 1000U, 100U);
}

/* Runnable peers do not swap faster than the granularity: with a service pass
 * every 100 us, two CPU-bound apps change places at most once per millisecond
 * of run time, and keep their shares. */
static void test_runnable_peers_swap_once_per_granularity(void)
{
	static const uint32_t equal[] = { 1U, 1U };
	static const uint32_t unequal[] = { 1U, 8U };
	const uint64_t total = 4ULL * 1000000U;
	uint64_t done;

	sim_start(&g_sim, equal, 2U);
	for (done = 0U; done < total; done += 100U) {
		sim_run_for(&g_sim, 100U);
		sim_service(&g_sim, -1);
	}
	CHECK(g_sim.preemptions <= total / GRAN_US + 1U);
	check_proportional(&g_sim, equal, 2U, total);

	sim_start(&g_sim, unequal, 2U);
	for (done = 0U; done < total; done += 100U) {
		sim_run_for(&g_sim, 100U);
		sim_service(&g_sim, -1);
	}
	CHECK(g_sim.preemptions <= total / GRAN_US + 1U);
	check_proportional(&g_sim, unequal, 2U, total);
}

/* Two apps that wake each other: each runs 100 us, blocks and has the other
 * woken by the service pass. Every change of app is the blocking one: the
 * wake takes the core from nobody, so there is no extra switch. */
static void test_mutual_wake_does_not_ping_pong(void)
{
	const uint32_t cycles = 5000U;
	uint32_t i;

	sim_boot(&g_sim, TEST_RECS);
	sim_add(&g_sim, 0U, 1U);
	sim_add(&g_sim, 1U, 1U);
	CHECK(zzapp_kernel_block(&g_sim.kernel, 1U) == 0);
	sim_first_dispatch(&g_sim);
	CHECK(zzapp_kernel_current_thread() == 0);
	for (i = 0U; i < cycles; i++) {
		int cur = zzapp_kernel_current_thread();
		int other = 1 - cur;

		CHECK(cur == (int)(i & 1U));
		sim_run_for(&g_sim, 100U);
		CHECK(zzapp_kernel_block(&g_sim.kernel, (uint32_t)cur) == 0);
		sim_step(&g_sim, 0U, ZZAPP_KERNEL_DISPATCH_VOLUNTARY);
		sim_service(&g_sim, other);
	}
	CHECK(g_sim.preemptions == 0U);
	CHECK(g_sim.switches == cycles);
	CHECK(g_sim.ran_us[0] + g_sim.ran_us[1] == (uint64_t)cycles * 100U);
}

/* A chatty app next to a spinner takes the core at its wakes and nowhere
 * else: it never causes more switches than it has wakes. */
static void test_chatty_app_preempts_once_per_wake(void)
{
	run_interactive(&g_sim, 1U, 100U, 100U, 2000000U);
	CHECK(g_ia_wakes > 1000U);
	CHECK(g_sim.preemptions <= g_ia_wakes + 1U);
	CHECK(g_sim.switches <= 2U * g_ia_wakes + 2U);
	CHECK(g_ia_max_delay <= CARD_TICK_US);
}

static void run_case(const char *name, void (*fn)(void), int argc, char **argv)
{
	unsigned before = (unsigned)failures;

	if (argc > 1 && strcmp(argv[1], name) != 0)
		return;
	fn();
	printf("%s: %s\n", name, (unsigned)failures == before ? "ok" : "FAILED");
}

int main(int argc, char **argv)
{
	run_case("weights", test_shares_follow_weights, argc, argv);
	run_case("bounds", test_weight_bounds, argc, argv);
	run_case("unset", test_unset_weight_counts_as_one, argc, argv);
	run_case("rotate", test_equal_weights_rotate_in_pid_order, argc, argv);
	run_case("wake_block", test_wake_after_long_block, argc, argv);
	run_case("wake_sleep", test_wake_after_long_sleep, argc, argv);
	run_case("wake_credit", test_wake_credit_is_one_quantum, argc, argv);
	run_case("wake_keeps", test_wake_keeps_a_larger_virtual_time, argc, argv);
	run_case("parked", test_parked_apps_get_nothing, argc, argv);
	run_case("join", test_late_join, argc, argv);
	run_case("join_empty", test_join_into_an_empty_run_set_starts_at_the_floor,
		 argc, argv);
	run_case("yield", test_yield_goes_behind_peers, argc, argv);
	run_case("yield_peer", test_yielding_peer_does_not_starve_the_other, argc,
		 argv);
	run_case("submillisecond", test_sub_millisecond_slices_are_charged, argc,
		 argv);
	run_case("system", test_system_thread_first_and_shares_hold, argc, argv);
	run_case("interactive", test_interactive_app_runs_when_woken, argc, argv);
	run_case("peers", test_runnable_peers_swap_once_per_granularity, argc, argv);
	run_case("mutual", test_mutual_wake_does_not_ping_pong, argc, argv);
	run_case("chatty", test_chatty_app_preempts_once_per_wake, argc, argv);
	printf("kernel_fair_share_test: %u checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
