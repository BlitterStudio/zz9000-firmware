#include "zzm_format.h"
#include "zzm_registry.h"
#include "zzm_runtime.h"
#include "zzm_webm_ops.h"

extern const struct SDKVideoDecoderOps *zzm_webm_ops(uint32_t codec);

static const struct zzm_webm_ops webm_ops = {
	ZZM_WEBM_OPS_ABI_VERSION,
	sizeof(webm_ops),
	zzm_webm_ops,
};

int zzm_module_init(const zzm_services_t *services)
{
	if (!services || services->abi_version != ZZM_MODULE_ABI_VERSION ||
	    services->size < sizeof(*services))
		return -1;
	return zzm_registry_register_ops(ZZM_MODULE_WEBM, &webm_ops,
	                                sizeof(webm_ops)) ? 0 : -1;
}
