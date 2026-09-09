/* Host-only XTime stub for the instrument bench smoke (bench_smoke.c).
 * XTime is the smoke's manually advanced clock; COUNTS_PER_SECOND is
 * scaled so the smoke's `bench_clock += 31000` exceeds the bench's
 * 30 s watchdog window (30 * 1000 = 30000 counts). Never part of any
 * firmware build. */
#ifndef XTIME_L_HOST_STUB_H
#define XTIME_L_HOST_STUB_H
#include <stdint.h>
typedef uint64_t XTime;
extern uint64_t bench_clock;
#define COUNTS_PER_SECOND 1000U
static inline void XTime_GetTime(XTime *t) { *t = bench_clock; }
#endif
