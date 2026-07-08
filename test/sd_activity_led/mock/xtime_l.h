#ifndef XTIME_L_H
#define XTIME_L_H

#include <stdint.h>

typedef uint64_t XTime;

#define COUNTS_PER_SECOND 1000000ULL

void XTime_GetTime(XTime *Time);

#endif /* XTIME_L_H */
