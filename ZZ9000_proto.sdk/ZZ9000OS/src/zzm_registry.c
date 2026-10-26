#include <string.h>
#include "sdk_mailbox.h"
#include "zzm_loader.h"
#include "zzm_manifest.h"
#include "zzm_registry.h"

#define ZZM_REGISTRY_MODULES 8U
#define ZZM_REGISTRY_SERVICES 24U

static const void *module_ops[ZZM_REGISTRY_MODULES];
static uint32_t module_ops_size[ZZM_REGISTRY_MODULES];

typedef struct zzm_provided_service {
    uint32_t module_id;
    uint32_t id;
    uint32_t version;
    const void *interface;
    uint32_t interface_size;
} zzm_provided_service_t;
static zzm_provided_service_t provided_services[ZZM_REGISTRY_SERVICES];

static int module_index(uint32_t module_id)
{
    zzm_status_t status;
    uint32_t i;
    for (i = 0; i < zzm_loader_count(); i++) {
        if (zzm_loader_status(i, &status) && status.id == module_id)
            return (int)i;
    }
    return -1;
}

static const void *lookup_service(uint32_t service_id, uint32_t min_version)
{
    uint32_t i;
    for (i = 0; i < ZZM_REGISTRY_SERVICES; i++) {
        if (provided_services[i].interface &&
            provided_services[i].id == service_id &&
            provided_services[i].version >= min_version)
            return provided_services[i].interface;
    }
    return 0;
}

enum zzm_codec_availability zzm_registry_availability(uint32_t module_id)
{
    int index = module_index(module_id);
    zzm_status_t status;

    if (index < 0 || !zzm_loader_status((uint32_t)index, &status))
        return ZZM_CODEC_MISMATCH;
    if (status.state == ZZM_STATE_AVAILABLE)
        (void)zzm_loader_ensure_loaded(module_id);
    if (!zzm_loader_status((uint32_t)index, &status))
        return ZZM_CODEC_MISMATCH;
    return status.state == ZZM_STATE_LOADED ? ZZM_CODEC_AVAILABLE :
           status.state == ZZM_STATE_MISSING ? ZZM_CODEC_MISSING :
                                               ZZM_CODEC_MISMATCH;
}

uint16_t zzm_registry_sdk_status(uint32_t module_id)
{
    const enum zzm_codec_availability availability =
        zzm_registry_availability(module_id);
    return availability == ZZM_CODEC_AVAILABLE ? SDK_STATUS_OK :
           availability == ZZM_CODEC_MISSING ? SDK_STATUS_MODULE_MISSING :
                                                SDK_STATUS_MODULE_MISMATCH;
}

int zzm_registry_register_ops(uint32_t module_id, const void *ops,
                              uint32_t size)
{
    int index = module_index(module_id);
    if (!ops || !size || index < 0 || (uint32_t)index >= ZZM_REGISTRY_MODULES ||
        !zzm_loader_can_register(module_id))
        return 0;
    module_ops[index] = ops;
    module_ops_size[index] = size;
    return 1;
}

int zzm_registry_register_service(uint32_t module_id, uint32_t service_id,
                                  uint32_t version, const void *interface,
                                  uint32_t interface_size)
{
    uint32_t i;
    if (!interface || !interface_size || !service_id || !version ||
        !zzm_loader_can_register(module_id) ||
        !zzm_loader_service_declared(module_id, service_id, version))
        return 0;
    for (i = 0; i < ZZM_REGISTRY_SERVICES; i++) {
        if (provided_services[i].interface &&
            provided_services[i].id == service_id)
            return 0;
    }
    for (i = 0; i < ZZM_REGISTRY_SERVICES; i++) {
        if (!provided_services[i].interface) {
            provided_services[i].module_id = module_id;
            provided_services[i].id = service_id;
            provided_services[i].version = version;
            provided_services[i].interface = interface;
            provided_services[i].interface_size = interface_size;
            return 1;
        }
    }
    return 0;
}

const void *zzm_registry_get_service(uint32_t service_id, uint32_t min_version)
{
    const void *service = lookup_service(service_id, min_version);
    if (!service && zzm_loader_ensure_service(service_id, min_version))
        service = lookup_service(service_id, min_version);
    return service;
}

const void *zzm_registry_ops(uint32_t module_id, uint32_t min_size)
{
    int index;
    if (zzm_registry_availability(module_id) != ZZM_CODEC_AVAILABLE)
        return 0;
    index = module_index(module_id);
    if (index < 0 || (uint32_t)index >= ZZM_REGISTRY_MODULES ||
        !module_ops[index] || module_ops_size[index] < min_size)
        return 0;
    return module_ops[index];
}

void zzm_registry_unregister_module(uint32_t module_id)
{
    int index = module_index(module_id);
    uint32_t i;
    if (index >= 0 && (uint32_t)index < ZZM_REGISTRY_MODULES) {
        module_ops[index] = 0;
        module_ops_size[index] = 0U;
    }
    for (i = 0; i < ZZM_REGISTRY_SERVICES; i++) {
        if (provided_services[i].module_id == module_id)
            memset(&provided_services[i], 0, sizeof(provided_services[i]));
    }
}

int zzm_registry_module_services_ready(uint32_t module_id)
{
    uint32_t i;
    uint32_t registered = 0U;
    for (i = 0; i < ZZM_REGISTRY_SERVICES; i++) {
        if (provided_services[i].module_id == module_id &&
            provided_services[i].interface)
            registered++;
    }
    return registered == zzm_loader_service_declared_count(module_id);
}
