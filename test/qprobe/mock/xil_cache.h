/* Host stub: counts cache maintenance. The probe must not need any. */
#ifndef XIL_CACHE_H
#define XIL_CACHE_H
#include <stdint.h>
typedef uintptr_t INTPTR;
extern unsigned qprobe_mock_cache_ops;
static inline void Xil_DCacheFlushRange(uint32_t address, uint32_t length)
{
	(void)address;
	(void)length;
	qprobe_mock_cache_ops++;
}
static inline void Xil_DCacheInvalidateRange(uint32_t address, uint32_t length)
{
	(void)address;
	(void)length;
	qprobe_mock_cache_ops++;
}
static inline void Xil_ICacheInvalidateRange(uint32_t address, uint32_t length)
{
	(void)address;
	(void)length;
	qprobe_mock_cache_ops++;
}
#endif
