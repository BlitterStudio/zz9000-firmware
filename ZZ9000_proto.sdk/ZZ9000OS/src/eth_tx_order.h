/*
 * Completion order of the asynchronous Ethernet send (REG_ZZ_ETH_TX bit 15).
 *
 * REG_ZZ_ETH_TX_STATUS counts asynchronous submissions retired IN ORDER, sent
 * or refused.  A driver frees its oldest TX-window slots by that count, so a
 * submission refused while earlier frames are still owned by the GEM's DMA
 * must not be counted before them, or the driver reuses a slot the GEM is
 * still reading.  The queue holds the submissions in order -- 1 for a BD the
 * GEM will retire, 0 for a refusal -- and a refusal is counted once
 * everything ahead of it has been.  A BD retired with the queue empty or a
 * refusal at its head came from the synchronous path and is not counted.
 *
 * Header-only so the host test (test/eth_tx_order) runs this exact code.
 * The caller serialises: the send handler runs in the GEM's interrupt, the
 * main loop calls in with that interrupt masked.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ETH_TX_ORDER_H
#define ETH_TX_ORDER_H

#include <stdint.h>

#define ETH_TX_ORDER_LEN 8	/* > TXBD_CNT: the host keeps at most 4 outstanding */

struct eth_tx_order {
	volatile uint16_t done;	/* the count REG_ZZ_ETH_TX_STATUS reports */
	volatile uint8_t  head;
	volatile uint8_t  len;
	volatile uint8_t  entry[ETH_TX_ORDER_LEN];
};

static inline void eth_tx_order_retire_refused(struct eth_tx_order *q)
{
	while (q->len > 0 && q->entry[q->head] == 0) {
		q->head = (uint8_t)((q->head + 1) % ETH_TX_ORDER_LEN);
		q->len--;
		q->done++;
	}
}

/* A submission: bd 1 if it reached the GEM, 0 if it was refused. */
static inline void eth_tx_order_push(struct eth_tx_order *q, uint8_t bd)
{
	if (!bd && q->len == 0) {
		q->done++;		/* nothing ahead of it */
		return;
	}
	if (q->len >= ETH_TX_ORDER_LEN) {
		/* more outstanding than the protocol allows: the host broke it,
		 * and counting now is the least harm */
		q->done++;
		return;
	}
	q->entry[(q->head + q->len) % ETH_TX_ORDER_LEN] = bd ? 1 : 0;
	q->len++;
}

/* The GEM finished a BD.  BDs complete in submission order. */
static inline void eth_tx_order_retire_bd(struct eth_tx_order *q)
{
	if (q->len == 0 || q->entry[q->head] == 0)
		return;
	q->head = (uint8_t)((q->head + 1) % ETH_TX_ORDER_LEN);
	q->len--;
	q->done++;
	eth_tx_order_retire_refused(q);
}

/* The TX ring was reset (DMA restart): what was outstanding will not
 * complete, so it is all retired now, or the host would wait for ever. */
static inline void eth_tx_order_flush(struct eth_tx_order *q)
{
	q->done = (uint16_t)(q->done + q->len);
	q->len = 0;
	q->head = 0;
}

#endif
