#ifndef ZZM_MPEG2_MODULE_H
#define ZZM_MPEG2_MODULE_H

#include <stdint.h>
#include "zzm_runtime.h"

void mpeg2_module_set_owner(uint32_t owner);
const zzm_services_t *mpeg2_module_services(void);
void *mpeg2_module_alloc(uint32_t bytes);
void mpeg2_module_free(void *ptr);

#endif /* ZZM_MPEG2_MODULE_H */
