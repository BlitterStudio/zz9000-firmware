/* Mapping to the limiter graph's runtime threshold parameter. */
#ifndef LIMITER_PARAMETER_H
#define LIMITER_PARAMETER_H
#include <stdint.h>
#include "adau_PARAM.h"
#define LIMITER_THRESHOLD_ADDR MOD_LIMITTHRESHOLD_DCINPALG1_ADDR
#define LIMITER_BOOT_THRESHOLD_Q23 3942646U
#define LIMITER_THRESHOLD_Q23_FROM_HUNDREDTHS(hundredths) \
	((((uint32_t)(hundredths) * 8388608U) + 50U) / 100U)
#endif
