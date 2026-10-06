// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * REG_ZZ_ETH_TX_STATUS must count asynchronous submissions retired in order:
 * a driver frees its oldest TX-window slots by the count.
 */
#include <stdio.h>
#include <string.h>
#include "eth_tx_order.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

static void refusal_behind_outstanding_sends(void)
{
	struct eth_tx_order q;
	memset(&q, 0, sizeof(q));

	eth_tx_order_push(&q, 1);	/* A */
	eth_tx_order_push(&q, 1);	/* B */
	eth_tx_order_push(&q, 1);	/* C */
	eth_tx_order_push(&q, 0);	/* D refused: no BD */
	CHECK(q.done == 0, "a refusal behind DMA-owned sends is not counted yet");

	eth_tx_order_retire_bd(&q);	/* A */
	CHECK(q.done == 1, "the first send retires alone");
	eth_tx_order_retire_bd(&q);	/* B */
	CHECK(q.done == 2, "the second send retires alone");
	eth_tx_order_retire_bd(&q);	/* C, then D behind it */
	CHECK(q.done == 4, "the refusal retires with the send ahead of it");
	CHECK(q.len == 0, "nothing is left outstanding");
}

static void refusal_with_nothing_outstanding(void)
{
	struct eth_tx_order q;
	memset(&q, 0, sizeof(q));

	eth_tx_order_push(&q, 0);
	CHECK(q.done == 1 && q.len == 0, "a refusal with nothing ahead counts at once");
}

static void synchronous_bd_is_not_counted(void)
{
	struct eth_tx_order q;
	memset(&q, 0, sizeof(q));

	eth_tx_order_retire_bd(&q);
	CHECK(q.done == 0, "a BD the asynchronous path did not submit is not counted");
}

static void restart_retires_everything(void)
{
	struct eth_tx_order q;
	memset(&q, 0, sizeof(q));

	eth_tx_order_push(&q, 1);
	eth_tx_order_push(&q, 0);
	eth_tx_order_push(&q, 1);
	eth_tx_order_flush(&q);
	CHECK(q.done == 3 && q.len == 0, "a DMA restart retires what was outstanding");
	eth_tx_order_retire_bd(&q);
	CHECK(q.done == 3, "a late completion after the restart is not counted twice");
}

/* The driver's view: four slots, freed oldest first by the count.  A slot is
 * free only after its own submission was retired. */
static void driver_model_never_frees_a_busy_slot(void)
{
	struct eth_tx_order q;
	int in_flight[4];	/* GEM still owns the slot */
	int queue[4];		/* slot of each outstanding submission, oldest first */
	int nq = 0, gem = 0, next = 0;
	unsigned seed = 12345;
	uint16_t seen = 0;
	int round;

	memset(&q, 0, sizeof(q));
	memset(in_flight, 0, sizeof(in_flight));
	for (round = 0; round < 40000; round++) {
		seed = seed * 1103515245u + 12345u;
		if (nq < 4 && (seed >> 16) % 3 != 0) {
			int refuse = ((seed >> 8) % 7) == 0;
			queue[nq++] = next;
			if (!refuse)
				in_flight[next] = 1, gem++;
			eth_tx_order_push(&q, refuse ? 0 : 1);
			next = (next + 1) % 4;
		} else if (gem > 0) {
			/* the GEM finishes its oldest BD */
			int i;
			for (i = 0; i < nq; i++)
				if (in_flight[queue[i]]) { in_flight[queue[i]] = 0; break; }
			gem--;
			eth_tx_order_retire_bd(&q);
		}
		/* the driver reclaims by count */
		while (seen != q.done) {
			CHECK(nq > 0, "the count never runs ahead of submissions");
			CHECK(!in_flight[queue[0]], "a slot is freed only once the GEM has finished it");
			memmove(queue, queue + 1, (size_t)(nq - 1) * sizeof(queue[0]));
			nq--;
			seen++;
			if (failures) return;
		}
	}
	CHECK(q.done != 0, "the count advanced (and wrapped through uint16_t)");
}

int main(void)
{
	refusal_behind_outstanding_sends();
	refusal_with_nothing_outstanding();
	synchronous_bd_is_not_counted();
	restart_retires_everything();
	driver_model_never_frees_a_busy_slot();
	if (failures == 0)
		printf("eth_tx_order: all checks passed\n");
	return failures ? 1 : 0;
}
