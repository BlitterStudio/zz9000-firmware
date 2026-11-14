#include <assert.h>
#include <stdint.h>

void scheduler_core1_fallback_host_test(void);

static uint32_t stage;
static uint32_t recovery_calls;

void core1_halt(void)
{
    assert(stage == 0U);
    stage = 1U;
}

void zzapp_recover_halted_core1(void)
{
    assert(stage == 1U);
    recovery_calls++;
    stage = 2U;
}

void sdk_compression_reclaim_core1_decode(void)
{
    assert(stage == 2U);
    assert(recovery_calls == 1U);
    stage = 3U;
}

void zzapp_services_reclaim_halted_audio(void)
{
    assert(stage == 3U);
    stage = 4U;
}

void zzm_runtime_quarantine_module(uint32_t module)
{
    (void)module;
    assert(stage == 4U);
    stage = 5U;
}

int main(void)
{
    scheduler_core1_fallback_host_test();
    assert(recovery_calls == 1U);
    assert(stage == 5U);
    return 0;
}
