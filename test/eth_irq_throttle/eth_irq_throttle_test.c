// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ethernet_irq_throttle.h"

static int failures;

#define TEST_REPEAT_INTERVAL 500U

#define CHECK(cond, msg) do { \
	if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

static void first_raise_is_immediate(void)
{
	struct ethernet_irq_throttle state;

	memset(&state, 0, sizeof(state));
	CHECK(ethernet_irq_throttle_should_raise(&state, 1, 1U, 1U, 0, 100U,
	                                         TEST_REPEAT_INTERVAL),
	      "first backlog frame raises immediately");
	CHECK(!ethernet_irq_throttle_should_raise(&state, 1, 1U, 1U, 1, 101U,
	                                          TEST_REPEAT_INTERVAL),
	      "an asserted IRQ is not raised again");
}

static void repeated_ack_waits_for_interval(void)
{
	struct ethernet_irq_throttle state;

	memset(&state, 0, sizeof(state));
	CHECK(ethernet_irq_throttle_should_raise(&state, 1, 1U, 1U, 0, 100U,
	                                         TEST_REPEAT_INTERVAL),
	      "initial raise succeeds");
	CHECK(!ethernet_irq_throttle_should_raise(&state, 1, 1U, 1U, 0,
	                                          100U + TEST_REPEAT_INTERVAL - 1U,
	                                          TEST_REPEAT_INTERVAL),
	      "ack without progress waits for the repeat interval");
	CHECK(ethernet_irq_throttle_should_raise(&state, 1, 1U, 1U, 0,
	                                         100U + TEST_REPEAT_INTERVAL,
	                                         TEST_REPEAT_INTERVAL),
	      "ack without progress re-raises at the repeat interval");
}

static void progress_raises_immediately(void)
{
	struct ethernet_irq_throttle state;

	memset(&state, 0, sizeof(state));
	CHECK(ethernet_irq_throttle_should_raise(&state, 1, 2U, 8U, 0, 100U,
	                                         TEST_REPEAT_INTERVAL),
	      "initial raise succeeds");
	CHECK(ethernet_irq_throttle_should_raise(&state, 1, 1U, 8U, 0, 101U,
	                                         TEST_REPEAT_INTERVAL),
	      "backlog consumption re-raises immediately");
	CHECK(ethernet_irq_throttle_should_raise(&state, 1, 1U, 9U, 0, 102U,
	                                         TEST_REPEAT_INTERVAL),
	      "a received frame re-raises immediately");
}

static void reenable_is_immediate(void)
{
	struct ethernet_irq_throttle state;

	memset(&state, 0, sizeof(state));
	CHECK(ethernet_irq_throttle_should_raise(&state, 1, 1U, 1U, 0, 100U,
	                                         TEST_REPEAT_INTERVAL),
	      "initial raise succeeds");
	CHECK(!ethernet_irq_throttle_should_raise(&state, 0, 1U, 1U, 0, 101U,
	                                          TEST_REPEAT_INTERVAL),
	      "disabling the IRQ clears throttle state");
	CHECK(ethernet_irq_throttle_should_raise(&state, 1, 1U, 1U, 0, 102U,
	                                         TEST_REPEAT_INTERVAL),
	      "re-enabling raises immediately");
}

static void timer_wrap_preserves_interval(void)
{
	struct ethernet_irq_throttle state;
	uint32_t start = UINT32_MAX - 250U;

	memset(&state, 0, sizeof(state));
	CHECK(ethernet_irq_throttle_should_raise(&state, 1, 1U, 1U, 0, start,
	                                         TEST_REPEAT_INTERVAL),
	      "initial raise before timer wrap succeeds");
	CHECK(!ethernet_irq_throttle_should_raise(&state, 1, 1U, 1U, 0, 248U,
	                                          TEST_REPEAT_INTERVAL),
	      "pre-interval value after wrap remains throttled");
	CHECK(ethernet_irq_throttle_should_raise(&state, 1, 1U, 1U, 0, 249U,
	                                         TEST_REPEAT_INTERVAL),
	      "interval expiry after timer wrap re-raises");
}

int main(void)
{
	first_raise_is_immediate();
	repeated_ack_waits_for_interval();
	progress_raises_immediately();
	reenable_is_immediate();
	timer_wrap_preserves_interval();
	if (failures != 0)
		return 1;
	puts("ethernet IRQ throttle tests passed");
	return 0;
}
