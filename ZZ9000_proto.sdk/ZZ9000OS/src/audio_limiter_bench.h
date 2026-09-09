/* Private, magic+calibration-gated bench protocol for hardware qualification. */
#ifndef AUDIO_LIMITER_BENCH_H
#define AUDIO_LIMITER_BENCH_H
#include "xtime_l.h"
#define LIMITER_BENCH_MAGIC 0x4c494d31U
struct limiter_bench_state {
    int active, phase, failed, restoring, queued_restore, armed;
    uint32_t case_id, threshold, original_threshold;
    XTime heartbeat;
    struct audio_scene_def original_scene;
    uint8_t original_index, original_paula, original_ax;
};
static struct limiter_bench_state limiter_bench;
static void limiter_bench_poll(void);
static void limiter_bench_reset(void);
#endif
