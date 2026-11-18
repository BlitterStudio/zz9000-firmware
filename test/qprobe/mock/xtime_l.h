/* Host stub: a clock that advances by qprobe_mock_tick on every read. */
#ifndef XTIME_L_H
#define XTIME_L_H
#include <stdint.h>
typedef uint64_t XTime;
#ifndef COUNTS_PER_SECOND
#define COUNTS_PER_SECOND 1000000ULL
#endif
extern XTime qprobe_mock_time;
extern XTime qprobe_mock_tick;
static inline void XTime_GetTime(XTime *time)
{
	qprobe_mock_time += qprobe_mock_tick;
	*time = qprobe_mock_time;
}
#endif
