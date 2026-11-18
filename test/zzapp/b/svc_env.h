/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host environment of the services suites. The services keep every app in a
 * process record and take its tables from the record arena, so a suite that
 * includes zzapp_services.c also needs the table layers: this header carries
 * their host mocks and builds an arena of a chosen size over the host card
 * pool. Include it once per test program, after zzapp_services.c.
 */
#ifndef B_SVC_ENV_H
#define B_SVC_ENV_H
#include <stdlib.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "card_pool.h"
#include "host_card_pool.h"
#include "memorymap.h"
#include "xil_cache.h"
#include "mock_client.h"
#include "zzapp_rec.h"
#include "zzapp_xport.h"

struct xil_cache_mock_state g_xil_cache_mock;
int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1U; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
	if (*word)
		return 0;
	*word = 1U;
	return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0U; }

/* The host's single retire path (zzapp_host.c) is not linked into these
 * suites: the tables go back and the wire layer is detached and freed. */
int zzapp_rec_retire(uint32_t pid)
{
	zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

	if (!rec || zzapp_rec_state(pid) == ZZAPP_REC_FREE)
		return -1;
	(void)zzapp_rec_set_state(rec, ZZAPP_REC_RETIRING);
	zzapp_rec_retire_tables(rec);
	return zzapp_services_retire_wire(rec);
}

#define SVC_ENV_HEAP_BYTES (78U << 20)
#define SVC_ENV_DDR_WB 0x15de6U

static uint32_t svc_env_bsp[4096];

/*
 * A fresh card pool, a record arena of `apps` records (at least 8) and the
 * kernel address-space template. `l2_total` 0 keeps the layout default;
 * anything else sizes the shared table pool (template and reserves included),
 * so a suite can make the pool hold exactly N reserves. Returns the arena's
 * layout through *out when given.
 */
static void svc_env_init(uint32_t apps, uint32_t l2_total,
			 zzapp_rec_layout_t *out)
{
	zzapp_rec_layout_t lay;
	uint32_t addr;
	uint32_t i;

	host_card_pool_reset(64U << 20);
	zzapp_rec_layout_for(apps, SVC_ENV_HEAP_BYTES, &lay);
	if (l2_total != 0U) {
		lay.l2_total = l2_total;
		lay.l2_shared = l2_total > lay.l2_template + lay.apps * ZZAPP_AS_L2_RESERVE ?
			l2_total - lay.l2_template - lay.apps * ZZAPP_AS_L2_RESERVE : 0U;
		lay.as_bytes = zzapp_as_pool_bytes(lay.apps, l2_total);
	}
	addr = card_pool_alloc_aligned(&card_pool, lay.bytes, ZZAPP_REC_ARENA_ALIGN,
		CARD_POOL_OWNER(CARD_POOL_CLASS_APP_ARENA, 0U));
	if (addr == 0U || zzapp_rec_arena_init((void *)(uintptr_t)addr, &lay) != 0) {
		fprintf(stderr, "svc_env: arena of %u records did not init\n",
			(unsigned)apps);
		exit(2);
	}
	/* All of the 32-bit space 1:1, as the host harness does: a host buffer a
	 * test grants may sit anywhere below 4 GiB. */
	for (i = 0U; i < 4096U; i++)
		svc_env_bsp[i] = (i << 20) | SVC_ENV_DDR_WB;
	zzapp_as_init(svc_env_bsp);
	/* The host-visible heap the pair and the service buffers come from. */
	mock_heap_reset();
	{
		zzapp_xport_env_t env = mock_heap_env();

		zzapp_xport_init(&env);
	}
	if (out)
		*out = lay;
}

#include "svc_views.h"

#endif
