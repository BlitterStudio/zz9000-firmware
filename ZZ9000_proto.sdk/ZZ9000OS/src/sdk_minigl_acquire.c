#include <stdint.h>
#include "sdk_mailbox.h"
#include "zzm_loader.h"
#include "zzm_minigl_host.h"

static volatile uint32_t minigl_acquire_cancelled;
static uint32_t minigl_acquire_refs;

void sdk_mailbox_begin_amiga_reset(void)
{
	/* This is deliberately lock-free: a core-1 loader slice may hold the
	 * session lock while reset recovery has IRQs masked. */
	minigl_acquire_cancelled = 1U;
	__sync_synchronize();
}

int sdk_mailbox_minigl_acquire_cancelled(void)
{
	return minigl_acquire_cancelled != 0U;
}

uint16_t sdk_mailbox_minigl_acquire_on_owner(uint8_t *result_payload,
                                             uint32_t *result_len)
{
	uint32_t generation = 0U;
	int load_result;

	*result_len = 0U;
	if (minigl_acquire_cancelled) {
		zzm_loader_cancel_loading(ZZM_MODULE_MINIGL);
		return SDK_STATUS_IO_ERROR;
	}
	load_result = zzm_loader_acquire(ZZM_MODULE_MINIGL);
	if (load_result == ZZM_LOAD_IN_PROGRESS)
		return SDK_TASK_IN_PROGRESS;
	if (load_result != ZZM_LOAD_READY)
		return SDK_STATUS_MODULE_MISSING;
	if (minigl_acquire_cancelled) {
		zzm_loader_release(ZZM_MODULE_MINIGL);
		return SDK_STATUS_IO_ERROR;
	}
	if (!zzm_minigl_transport_acquire(&generation)) {
		zzm_loader_release(ZZM_MODULE_MINIGL);
		return SDK_STATUS_MODULE_MISMATCH;
	}
	if (minigl_acquire_cancelled) {
		(void)zzm_minigl_transport_release();
		zzm_loader_release(ZZM_MODULE_MINIGL);
		return SDK_STATUS_IO_ERROR;
	}
	minigl_acquire_refs++;
	result_payload[0] = (uint8_t)(generation >> 24);
	result_payload[1] = (uint8_t)(generation >> 16);
	result_payload[2] = (uint8_t)(generation >> 8);
	result_payload[3] = (uint8_t)generation;
	*result_len = sizeof(generation);
	return SDK_STATUS_OK;
}

int sdk_mailbox_minigl_release_on_owner(void)
{
	if (!zzm_minigl_transport_release())
		return 0;
	if (minigl_acquire_refs != 0U) {
		minigl_acquire_refs--;
		zzm_loader_release(ZZM_MODULE_MINIGL);
	}
	return 1;
}

void sdk_mailbox_minigl_reset_release_all(void)
{
	while (minigl_acquire_refs != 0U) {
		minigl_acquire_refs--;
		zzm_loader_release(ZZM_MODULE_MINIGL);
	}
}

void sdk_mailbox_finish_amiga_reset(void)
{
	minigl_acquire_cancelled = 0U;
}
