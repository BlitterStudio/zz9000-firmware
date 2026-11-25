/*
 * Host check of the MAILBOX_POLL_DEADLINE_US decision: the scan cadence it
 * produces in a model of the main loop's poll site (events first, then the
 * idle-pass counter, the deadline, the periodic scan, in that order; every
 * scan restarts the deadline). One timer tick is one microsecond.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include "mailbox_poll_deadline.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct sim {
	uint64_t now;
	uint64_t last_scan;
	uint32_t divider;
	uint64_t max_gap;       /* longest time between two scans */
	uint32_t periodic;
	uint32_t event;
	uint32_t deadline;
};

static void scan(struct sim *s, uint32_t *counter)
{
	uint64_t gap = s->now - s->last_scan;

	if (gap > s->max_gap)
		s->max_gap = gap;
	(*counter)++;
	s->last_scan = s->now;
}

/* One idle pass of `ticks`. `event` when a register event is waiting. */
static void idle_pass(struct sim *s, uint64_t ticks, int event,
                      uint64_t deadline_ticks)
{
	s->now += ticks;
	if (event)
		scan(s, &s->event);
	s->divider++;
	if (mailbox_poll_deadline_due(s->divider, s->now, s->last_scan,
	                              deadline_ticks))
		scan(s, &s->deadline);
	if ((s->divider & 0xffU) == 0U)
		scan(s, &s->periodic);
}

static void test_cadence_is_bounded_by_the_deadline(void)
{
	struct sim s = { 0 };
	uint32_t i;

	s.last_scan = 0;
	for (i = 0U; i < 100000U; i++)
		idle_pass(&s, 13U, 0, 250U);
	/* 13 us passes: the divider alone would scan every 3328 us. */
	CHECK(s.max_gap <= 250U + 13U);
	CHECK(s.deadline > 4000U);
	/* Every scan restarts the deadline, so one scan per 250 us and no
	 * extra: 1.3 s of loop is about 5200 scans. */
	CHECK(s.deadline + s.periodic > 4800U && s.deadline + s.periodic < 5400U);
}

static void test_events_restart_the_deadline(void)
{
	struct sim s = { 0 };
	uint32_t i;

	/* An event scan every 65 us keeps the ring read; the deadline never adds
	 * a full poll on top of it. */
	for (i = 0U; i < 20000U; i++)
		idle_pass(&s, 13U, (i % 5U) == 4U, 250U);
	CHECK(s.event == 4000U);
	CHECK(s.deadline == 0U);
	CHECK(s.max_gap <= 65U);
}

static void test_long_work_costs_one_scan(void)
{
	struct sim s = { 0 };
	uint32_t i;
	uint32_t before;

	for (i = 0U; i < 1000U; i++)
		idle_pass(&s, 13U, 0, 250U);
	before = s.deadline + s.periodic;
	/* A 6 ms pass: the next idle pass scans once, not once per missed
	 * deadline, and the cadence resumes after it. */
	idle_pass(&s, 6000U, 0, 250U);
	CHECK(s.deadline + s.periodic == before + 1U);
	idle_pass(&s, 13U, 0, 250U);
	CHECK(s.deadline + s.periodic == before + 1U);
	CHECK(s.max_gap >= 6000U);
	for (i = 0U; i < 1000U; i++)
		idle_pass(&s, 13U, 0, 250U);
	CHECK(s.deadline + s.periodic > before + 1U + 45U);
}

static void test_wrap_and_stale_stamps(void)
{
	struct sim s = { 0 };
	uint32_t i;

	/* Across the 64-bit wrap of the timer. */
	s.now = UINT64_MAX - 5000U;
	s.last_scan = s.now;
	for (i = 0U; i < 2000U; i++)
		idle_pass(&s, 13U, 0, 250U);
	CHECK(s.now < 100000U);
	CHECK(s.max_gap <= 250U + 13U);

	/* A top-of-pass stamp older than the last scan is not due. */
	CHECK(!mailbox_poll_deadline_due(1U, 1000U, 1500U, 250U));
	CHECK(!mailbox_poll_deadline_due(1U, 1000U, 1000U, 250U));
	CHECK(!mailbox_poll_deadline_due(1U, 1249U, 1000U, 250U));
	CHECK(mailbox_poll_deadline_due(1U, 1250U, 1000U, 250U));
	/* The periodic pass is the divider's, not the deadline's. */
	CHECK(!mailbox_poll_deadline_due(256U, 100000U, 0U, 250U));
}

int main(void)
{
	test_cadence_is_bounded_by_the_deadline();
	test_events_restart_the_deadline();
	test_long_work_costs_one_scan();
	test_wrap_and_stale_stamps();
	if (failures == 0)
		printf("mailbox_poll_deadline_test: ok\n");
	return failures != 0;
}
