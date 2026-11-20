/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * The ZZ9000 platform of aar.library (layer L3). It implements the operation
 * table of the aar repository's client/src/aar_plat.h: the mailbox calls of
 * the private wire zzapp_proto.h, the 68k board (zz9k.library, expansion.library,
 * mmu.library) and the doorbell. Everything above it, the handle table, the
 * queue pump, events, messages, status, tags, the lease interrupt and the
 * dispatcher, is the portable core and the AmigaOS shell in the aar
 * repository (MIT). The firmware SDK build links the three into one
 * aar.library, which is GPL-3.0-or-later because of this layer.
 *
 * The library opens zz9k.library once, from its dispatcher, and carries every
 * mailbox call through the explicit-base calls of it. It does not drive the
 * board registers. mmu.library is opened lazily, at the first claim or map,
 * so a query works without it.
 *
 * Threading. The operations run in the context of the calling task. zz9k.library
 * serialises the mailbox; nothing here holds a lock across a mailbox call. Each
 * claim owns its doorbell request, so tasks that ring different claims never
 * share one. The dispatcher and the owner may both ring or retire the doorbell
 * of one claim: a try-lock inside the claim makes the loser report AAR_E_BUSY
 * (a doorbell is a hint) and `release` waits for the winner.
 *
 * The platform sits behind aar_zz_board: the 68k implements it in this file,
 * and the host test of the SDK (sdk/tests/aar_zz9000_test.c) supplies a fake
 * one over the firmware's own zzapp_xport.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AAR_ZZ9000_H
#define AAR_ZZ9000_H

#include <stdint.h>

#include "aar/app.h"
#include "aar/transport.h"

#include "zzapp_proto.h"

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__)
#define AAR_ZZ9000_AMIGA 1
#else
#define AAR_ZZ9000_AMIGA 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* One mailbox reply. `status` is ZZ9K_STATUS_* of the reply (0 is OK); the
 * payload holds the reply bytes whatever the status (a refusal carries the
 * AAR_REASON_* word at 0). */
typedef struct aar_zz_reply {
	int32_t status;
	uint32_t len;
	uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES];
} aar_zz_reply;

/* The storage of one doorbell request. It lives in the claim, so every claim
 * has its own, and is zeroed before the claim's first use. Only the board
 * looks inside. */
#define AAR_ZZ_BELL_WORDS 96u
typedef struct aar_zz_bell {
	uint32_t words[AAR_ZZ_BELL_WORDS];
} aar_zz_bell;

typedef struct aar_zz_board {
	void *user;
	/*
	 * One mailbox call with a request payload of `len` bytes (at most 48).
	 * `long_op` is non-zero for the operations the card answers late (load
	 * begin and end, start): the call then waits up to two minutes and
	 * retries a BUSY answer. Returns the status of the exchange: 0 when a
	 * reply arrived and its status is OK, else the ZZ9K_STATUS_* that says
	 * why. `reply` is filled whenever a reply arrived.
	 */
	int (*call)(void *user, uint16_t opcode, const uint8_t *payload,
		    uint32_t len, int long_op, aar_zz_reply *reply);
	/*
	 * A mailbox request nobody waits for (the doorbell), kept in `bell`.
	 * AAR_OK when it was queued, AAR_E_BUSY when the board cannot take one
	 * now, another AAR_E_* when it cannot. The caller has checked that
	 * the previous one on this bell is done.
	 */
	int (*bell_post)(void *user, aar_zz_bell *bell, uint16_t opcode,
			 const uint8_t *payload, uint32_t len);
	/* 1 once the request in `bell` has completed (polls the mailbox once). */
	int (*bell_done)(void *user, aar_zz_bell *bell);
	/* Withdraws a request that is still in flight. */
	void (*bell_cancel)(void *user, aar_zz_bell *bell);
	/* Makes the board ready to map (mmu.library on the 68k). AAR_OK, or
	 * AAR_E_NOSYS when it cannot. May be NULL. */
	int (*prepare)(void *user);
	/*
	 * The CPU pointer of [token, token + bytes), made cache-inhibited.
	 * `token` is a board offset (an aar_shared haddr: the card address
	 * less ADDR_ADJ). AAR_OK, AAR_E_INVAL when the range is outside the
	 * board, AAR_E_NOMEM or AAR_E_NOSYS when it cannot map.
	 */
	int (*map)(void *user, uint32_t token, uint32_t bytes,
		   volatile uint8_t **ptr);
	/* The inverse: the token of a pointer into the board, AAR_E_INVAL if
	 * it is outside. */
	int (*token)(void *user, const volatile void *ptr, uint32_t bytes,
		     uint32_t *token);
	/* Barrier hooks of aar_queue_*; NULL on the 68k. */
	aar_barrier_fn acquire;
	aar_barrier_fn release;
} aar_zz_board;

#if !AAR_ZZ9000_AMIGA
/*
 * The host build has no board of its own: the test installs one before the
 * first `attach`. Passing NULL removes it.
 */
void aar_zz9000_host_board(const aar_zz_board *board);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AAR_ZZ9000_H */
