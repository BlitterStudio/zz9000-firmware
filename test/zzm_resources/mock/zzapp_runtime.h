#ifndef ZZAPP_RUNTIME_H
#define ZZAPP_RUNTIME_H
#include <stdint.h>
void zzapp_api_enter(void);
void zzapp_api_leave(void);
uint32_t zzapp_current_generation(void);
#endif
