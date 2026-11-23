/*
 * Module load step instrument (-DZZM_LOAD_BENCH, firmware only).
 *
 * The loader runs one load step per call on core 1 with the session-begin
 * guard held. This times every step, keeps the longest one per phase and
 * module, and prints them from the core-0 main loop when no voice session is
 * open. See docs/modules.md.
 */
#ifndef ZZM_LOAD_BENCH_H
#define ZZM_LOAD_BENCH_H

#include <stddef.h>
#include <stdint.h>

#define ZZM_LOAD_BENCH_MODULES 8U
#define ZZM_LOAD_BENCH_PHASES 7U

/* Phase of a load step, from the loader's state before it runs: open covers
 * the first step (storage check, quota, f_open, and for v1 modules the
 * dependency loads), then one phase per load state, then failure cleanup. */
enum zzm_load_bench_phase {
    ZZM_LOAD_BENCH_OPEN = 0,
    ZZM_LOAD_BENCH_READ_HASH,
    ZZM_LOAD_BENCH_BSS_ZERO,
    ZZM_LOAD_BENCH_DCACHE_FLUSH,
    ZZM_LOAD_BENCH_ICACHE_INVALIDATE,
    ZZM_LOAD_BENCH_INIT,
    ZZM_LOAD_BENCH_FAIL,
};

#if defined(ZZM_LOAD_BENCH) || defined(ZZM_LOAD_BENCH_HOST_TEST)

/* Core 1, after one load step. */
void zzm_load_bench_record(uint32_t module, const char *name, uint32_t phase,
                           uint64_t ticks);
/* Format one module's line. Returns the length snprintf would produce, or 0
 * when the module has no recorded step. */
int zzm_load_bench_format(char *buf, size_t size, uint32_t module,
                          uint32_t t_ms);

#endif

#ifdef ZZM_LOAD_BENCH

/* Global timer ticks. */
uint64_t zzm_load_bench_now(void);
/* Core 0 main loop: at most one report per second, only for modules that
 * saw a new longest step. The caller skips it while a voice session is
 * open. */
void zzm_load_bench_poll(void);

#endif /* ZZM_LOAD_BENCH */

#endif /* ZZM_LOAD_BENCH_H */
