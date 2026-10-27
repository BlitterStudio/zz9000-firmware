/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ETHERNET_IRQ_THROTTLE_H_
#define ETHERNET_IRQ_THROTTLE_H_

#include <stdint.h>


struct ethernet_irq_throttle {
	uint32_t last_raise_ticks;
	uint32_t last_rx_sequence;
	uint16_t last_backlog;
	uint8_t initialized;
};

static inline void ethernet_irq_throttle_reset(struct ethernet_irq_throttle *state)
{
	state->last_raise_ticks = 0U;
	state->last_rx_sequence = 0U;
	state->last_backlog = 0U;
	state->initialized = 0U;
}

static inline int ethernet_irq_throttle_should_raise(
	struct ethernet_irq_throttle *state, int enabled, uint16_t backlog,
	uint32_t rx_sequence, int irq_pending, uint32_t now_ticks,
	uint32_t repeat_interval_ticks)
{
	if (!enabled || backlog == 0U) {
		ethernet_irq_throttle_reset(state);
		return 0;
	}

	if (irq_pending)
		return 0;

	if (!state->initialized || backlog != state->last_backlog ||
	    rx_sequence != state->last_rx_sequence ||
	    (uint32_t)(now_ticks - state->last_raise_ticks) >=
		repeat_interval_ticks) {
		state->last_raise_ticks = now_ticks;
		state->last_rx_sequence = rx_sequence;
		state->last_backlog = backlog;
		state->initialized = 1U;
		return 1;
	}

	return 0;
}

#endif /* ETHERNET_IRQ_THROTTLE_H_ */
