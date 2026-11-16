#ifndef XTIME_L_H
#define XTIME_L_H
#include <stdint.h>
typedef uint64_t XTime;
static inline void XTime_GetTime(XTime *time) { *time = 0U; }
#endif
