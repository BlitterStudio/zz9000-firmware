#define ZZ_TEST_LIMITER_STUBS_EXCLUDED
#define main original_scene_tests
#include "audio_scene_test.c"
#undef main
#include "../../ZZ9000_proto.sdk/ZZ9000OS/src/sdk_audio_control.h"
#include <assert.h>
uint64_t bench_clock;
static uint32_t threshold_written;
static uint32_t threshold_now = 3942646U; /* boot default after this change */
static int fail_threshold;
static int fail_get_threshold;
static int force_zero_threshold;
int audio_adau_safe_param_substep(uint16_t address, const uint8_t value[4], int substep)
{
    (void)address;
    if (fail_threshold) return -1;
    if (substep == 2) {
        threshold_written = sdk_get_be32(value);
        threshold_now = threshold_written;
    }
    return substep == 2 ? 1 : 0;
}
int audio_adau_limiter_verify(uint16_t address, uint32_t expected)
{
    (void)address;
    return expected == threshold_written ? 0 : -1;
}
int audio_adau_limiter_threshold_get(uint32_t *value)
{
    if (value == 0) return -1;
    if (fail_get_threshold) return -1;
    *value = force_zero_threshold ? 0U : threshold_now;
    return 0;
}
static uint8_t reply[48];
static uint16_t call_bench(uint32_t command)
{
    uint8_t payload[48] = {0};
    uint16_t len = 0;
    uint16_t rc;
    sdk_put_be32(payload + 4, 0x4c494d31U);
    sdk_put_be32(payload + 8, command);
    rc = sdk_audio_control_run(SDK_OP_AUDIO_SCENE_WRITE, payload, 48, reply, &len);
    if (rc == SDK_STATUS_OK) {
        assert(len == 48);
        assert(sdk_get_be32(reply) == 0x4c494d31U);
    }
    return rc;
}
static void settle(void)
{
    int i;
    for (i = 0; i < 2000; ++i) audio_scene_poll();
    assert(call_bench(0) == SDK_STATUS_OK);
    assert(!(sdk_get_be32(reply + 4) & 2));
}
int main(void)
{
    struct audio_scene_def original;
    struct audio_scene_lease_gain_result grant;
    uint8_t payload[48] = {0};
    uint16_t len;
    int v, pan;
    setbuf(stdout, NULL);
    audio_scene_init();
    audio_scene_set_calibration(48,80);
    pump_scene();
    audio_scene_set_baseline(18,30);
    pump_scene();
    clear_writes();
    original = *audio_scene_get(0);
    assert(call_bench(3) == SDK_STATUS_IO_ERROR);
    assert(call_bench(1) == SDK_STATUS_OK);
    settle();
    assert(sdk_get_be32(reply + 16) == (18U | (30U << 8)));
    assert(call_bench(5) == SDK_STATUS_OK);
    settle();
    assert(sdk_get_be32(reply + 16) == (36U | (60U << 8)));
    assert(threshold_written == 3942646U);
    assert(last_write(WRITE_VOLPAN, &v, &pan) && v == 100);
    assert(audio_scene_lease_gain_compose(128, &grant) == 0);
    assert(grant.applied == 128 && !grant.bounded);
    assert(audio_scene_save_start(0) == AUDIO_SCENE_SAVE_REJECTED);
    assert(sdk_audio_control_run(SDK_OP_AUDIO_SCENE_SELECT,payload,48,reply,&len) == SDK_STATUS_BUSY);
    assert(call_bench(6) == SDK_STATUS_OK);
    settle();
    assert(!(sdk_get_be32(reply + 4) & 1));
    assert(threshold_written == 3942646U);
    assert(audio_scene_baseline_paula() == 18 && audio_scene_baseline_ax() == 30);
    assert(memcmp(audio_scene_get(0), &original, sizeof(original)) == 0);
    puts("PASS: boost applies, mutation/save blocked, exact state restored");
    assert(call_bench(1) == SDK_STATUS_OK);
    settle();
    bench_clock += 31000;
    settle();
    assert(!(sdk_get_be32(reply + 4) & 1));
    puts("PASS: abandoned session watchdog restores state");
    assert(call_bench(1) == SDK_STATUS_OK);
    settle();
    assert(call_bench(4) == SDK_STATUS_OK);
    settle();
    assert(audio_scene_apply_after_dsp_init() == 0);
    assert(call_bench(0) == SDK_STATUS_OK);
    assert(!(sdk_get_be32(reply + 4) & 1));
    assert(audio_scene_baseline_paula() == 18 && audio_scene_baseline_ax() == 30);
    puts("PASS: warm reset discards temporary boost");
    assert(call_bench(1) == SDK_STATUS_OK);
    settle();
    assert(call_bench(5) == SDK_STATUS_OK);
    fail_threshold = 1;
    settle();
    assert(sdk_get_be32(reply + 4) & 4);
    assert(last_write(WRITE_VOLPAN, &v, &pan) && v == 0);
    fail_threshold = 0;
    assert(call_bench(6) == SDK_STATUS_OK);
    settle();
    assert(!(sdk_get_be32(reply + 4) & 1));
    assert(threshold_written == 2936013U); /* failure path restores the BEGIN snapshot */
    puts("PASS: threshold write failure mutes, reports failure, permits restoration");
    fail_get_threshold = 1;
    assert(call_bench(1) == SDK_STATUS_IO_ERROR);
    fail_get_threshold = 0;
    assert(call_bench(0) == SDK_STATUS_OK);
    assert(!(sdk_get_be32(reply + 4) & 1));
    force_zero_threshold = 1;
    assert(call_bench(1) == SDK_STATUS_IO_ERROR);
    force_zero_threshold = 0;
    puts("PASS: unreadable or zero threshold rejects BEGIN without mutating");
    assert(audio_scene_stage_param(0, SDK_AUDIO_SCENE_PARAM_BASELINE,
        SDK_AUDIO_BALANCE_PACK(49, 30)) != 0);
    assert(audio_scene_stage_param(0, SDK_AUDIO_SCENE_PARAM_BASELINE,
        SDK_AUDIO_BALANCE_PACK(36, 81)) != 0);
    assert(audio_scene_set_baseline(36, 72) == 0);
    pump_scene();
    assert(audio_scene_baseline_paula() == 36 &&
        audio_scene_baseline_ax() == 72);
    puts("PASS: baseline legs beyond a clean ceiling are rejected at stage");
    audio_scene_set_calibration(48, 80);
    audio_scene_baseline_apply_parity_default();
    assert(audio_scene_baseline_paula() == 36 &&
        audio_scene_baseline_ax() == 72);
    audio_scene_set_calibration(256, 256);
    audio_scene_baseline_apply_parity_default();
    assert(audio_scene_baseline_paula() == 192 &&
        audio_scene_baseline_ax() == 255);
    puts("PASS: parity boot default tracks calibration (36/72 at 48/80)");
    return 0;
}
