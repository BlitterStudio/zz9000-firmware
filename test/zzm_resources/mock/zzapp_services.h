#ifndef ZZAPP_SERVICES_H
#define ZZAPP_SERVICES_H
#include <stdint.h>
int zzapp_display_bound(uint32_t *width, uint32_t *height, uint32_t *pitch,
                        uint32_t *format, uint32_t *card_front,
                        uint32_t *card_back);
int zzapp_display_flip_bound(void);
int zzapp_svc_display_flip_async(uint32_t surface_id);
int zzapp_display_flip_idle(void);
#endif
