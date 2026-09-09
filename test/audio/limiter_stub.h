/* Host-only link stubs for the bench protocol's direct DSP accesses
 * (their real definitions live in ax.c, which these suites do not
 * link). These suites never run a bench session; the symbols only
 * need to resolve. The bench smoke builds its own stateful versions
 * and defines ZZ_TEST_LIMITER_STUBS_EXCLUDED before including
 * anything that pulls this in. Exactly one TU per test binary
 * includes this header. */
#ifndef LIMITER_TEST_STUB_H
#define LIMITER_TEST_STUB_H

#ifndef ZZ_TEST_LIMITER_STUBS_EXCLUDED
#include <stdint.h>

int audio_adau_safe_param_substep(uint16_t address,
	const uint8_t value[4], int substep)
{
	(void)address;
	(void)value;
	return substep == 2 ? 1 : 0;
}

/* Backing storage for host_stubs/xtime_l.h's XTime shim. These
 * suites never advance it: the bench machinery they (passively)
 * carry sees a frozen clock, which never trips a watchdog because no
 * session is ever active. */
uint64_t bench_clock;

int audio_adau_limiter_verify(uint16_t address, uint32_t expected)
{
	(void)address;
	(void)expected;
	return 0;
}

int audio_adau_limiter_threshold_get(uint32_t *value)
{
	if (value != 0)
		*value = 3942646U; /* boot default (0.47 FS) */
	return 0;
}
#endif

#endif
