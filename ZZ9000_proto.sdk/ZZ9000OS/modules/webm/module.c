#include "zzm_format.h"
#include "zzm_registry.h"
#include "zzm_runtime.h"
#include "zzm_webm_ops.h"
#include "webm_module_alloc.h"

extern const struct SDKVideoDecoderOps *zzm_webm_ops(uint32_t codec);

static const struct zzm_webm_ops webm_ops = {
	ZZM_WEBM_OPS_ABI_VERSION,
	sizeof(webm_ops),
	zzm_webm_ops,
};
int _kill(int pid, int signal)
{
	(void)pid; (void)signal;
	return -1;
}
int _getpid(void) { return -1; }



int zzm_module_init(const zzm_services_t *services)
{
	if (!services || services->abi_version != ZZM_MODULE_ABI_VERSION ||
	    services->size < sizeof(*services) || !services->card_alloc ||
	    !services->card_free || !services->register_ops)
		return -1;
	webm_module_alloc_init(services);
	return services->register_ops(ZZM_MODULE_WEBM, &webm_ops,
	                              sizeof(webm_ops)) ? 0 : -1;
}
