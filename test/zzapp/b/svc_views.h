/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Views of the window layer for suites that include zzapp_services.c. The old
 * suites speak of slots; a slot is a launcher window, and the app behind it is
 * a record addressed by pid, which is not the window number once records were
 * used before. Select a window for the service context, and name its parts
 * through the layers. Include after zzapp_services.c.
 */
#ifndef B_SVC_VIEWS_H
#define B_SVC_VIEWS_H

#define zzapp_services_select_slot(n) zzapp_services_test_select_window(n)
#define RT(n) (*runtime_of(window_rec(&windows[(n)])))
#define TH(n) (window_rec(&windows[(n)])->thread)
#define WSTATE(n) window_state(&windows[(n)])
#define WGEN(n) (windows[(n)].claim_generation)

/* The launcher window the selected record runs behind (the old
 * zzapp_services_current_slot()). */
static uint32_t __attribute__((unused)) test_current_window(void)
{
	zzapp_rec_t *rec = service_rec();

	return rec ? rec->hdr.window : 0xffffffffU;
}
#define DEF(n) (windows[(n)].defer)
#define PID_OF(n) (window_rec(&windows[(n)])->hdr.pid)

static void __attribute__((unused)) test_force_state(uint32_t win, uint32_t state)
{
	window_rec(&windows[win])->claim.state = state;
}

/* The alias a surface or an audio ring reaches the app by is a virtual address
 * of its space; the buffer it names is checked by the physical address. */
#define PHYS(alias) \
	zzapp_as_alias_phys(&service_proc()->as, (uint32_t)(uintptr_t)(alias))

/* A claimed record has no address space until the loader builds one at
 * LOAD_END; tests that grant buffers to it make one. */
static void __attribute__((unused)) give_space(uint32_t win)
{
	zzapp_rec_t *rec = window_rec(&windows[win]);

	(void)zzapp_as_create(&rec->proc.as, rec->hdr.pid);
}

#endif
