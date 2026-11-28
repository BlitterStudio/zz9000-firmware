/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The host runtime's stop latch, as a strong symbol for svc_xport_test: the
 * services define a weak one in the same file the test includes. */
#include <stdint.h>

#include "aar/app.h"
#include "zzapp_rec.h"

unsigned svc_test_latch_calls;
uint32_t svc_test_latch_last;

void zzapp_host_latch_stop(uint32_t pid)
{
	svc_test_latch_calls++;
	svc_test_latch_last = pid;
	zzapp_rec_by_pid(pid)->host.stop_requested = 1U;
}

int (*svc_test_swap_tracked)(uint32_t surface_id, uint64_t frame_id);

int zzapp_minigl_swap_tracked(uint32_t surface_id, uint64_t frame_id)
{
	return svc_test_swap_tracked ? svc_test_swap_tracked(surface_id, frame_id) :
		AAR_E_NOSYS;
}
