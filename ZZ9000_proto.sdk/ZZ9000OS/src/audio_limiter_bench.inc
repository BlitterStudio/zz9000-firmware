/* Included after audio_scene.c definitions in the isolated instrument only. */
#include "limiter_parameter.h"
extern int audio_adau_limiter_verify(uint16_t address, uint32_t expected);
extern int audio_adau_limiter_threshold_get(uint32_t *value);

int audio_scene_limiter_bench_active(void)
{
    return limiter_bench.active;
}

static void limiter_bench_reset(void)
{
    if (limiter_bench.active) {
        active_scene_index = limiter_bench.original_index;
        scenes[active_scene_index] = limiter_bench.original_scene;
        baseline_paula = limiter_bench.original_paula;
        baseline_ax = limiter_bench.original_ax;
    }
    memset(&limiter_bench, 0, sizeof(limiter_bench));
}

static void limiter_bench_restore_begin(void)
{
    limiter_bench.restoring = 1;
    limiter_bench.queued_restore = 0;
    limiter_bench.threshold = limiter_bench.original_threshold;
    active_scene_index = limiter_bench.original_index;
    scenes[active_scene_index] = limiter_bench.original_scene;
    baseline_paula = limiter_bench.original_paula;
    baseline_ax = limiter_bench.original_ax;
    limiter_bench.phase = 1;
}

static void limiter_bench_poll(void)
{
    XTime now;
    uint8_t bytes[4];
    int rc;
    if (!limiter_bench.active)
        return;
    XTime_GetTime(&now);
    if (!limiter_bench.restoring && (uint64_t)(now - limiter_bench.heartbeat) >
            (uint64_t)COUNTS_PER_SECOND * 30U)
        limiter_bench.queued_restore = 1;
    if (limiter_bench.queued_restore && !limiter_bench.phase &&
            !commit_in_progress)
        limiter_bench_restore_begin();
    if (!limiter_bench.phase)
        return;
    if (limiter_bench.phase == 1) {
        rc = audio_adau_set_vol_pan(0, 50);
        if (rc != 0) goto failed;
        limiter_bench.phase = 2;
        return;
    }
    if (limiter_bench.phase >= 2 && limiter_bench.phase <= 4) {
        sdk_put_be32(bytes, limiter_bench.threshold);
        rc = audio_adau_safe_param_substep(LIMITER_THRESHOLD_ADDR,
                bytes, limiter_bench.phase - 2);
        if (rc < 0) goto failed;
        limiter_bench.phase++;
        return;
    }
    if (limiter_bench.phase == 5) {
        /* Real full commit, not a diff against pre-mute DSP state. */
        if (audio_adau_limiter_verify(LIMITER_THRESHOLD_ADDR,
                limiter_bench.threshold) != 0) goto failed;
        if (!limiter_bench.restoring) {
            scenes[active_scene_index] = default_scenes[0];
            baseline_paula = limiter_bench.case_id == 2U ? 18U : 36U;
            baseline_ax = limiter_bench.case_id == 2U ? 30U : 60U;
            limiter_bench.armed = 1;
        }
        last_applied_valid = 0;
        commit_begin(active_scene_index, 0);
        limiter_bench.phase = 6;
        return;
    }
    if (commit_in_progress)
        return;
    if (commit.failed) goto failed;
    limiter_bench.phase = 0;
    if (limiter_bench.restoring) {
        limiter_bench.active = 0;
        limiter_bench.restoring = 0;
        limiter_bench.failed = 0; /* restoration is independently verified */
        limiter_bench.armed = 0;
        limiter_bench.queued_restore = 0;
        limiter_bench.case_id = 6;
    }
    return;
failed:
    /* Never leave an unverified boosted path sounding. Report failure,
     * mute, and leave the saved settings recoverable by RESTORE/reset. */
    limiter_bench.failed = 1;
    (void)audio_adau_set_vol_pan(0, 50);
    limiter_bench.phase = 0;
    if (!limiter_bench.restoring)
        limiter_bench.queued_restore = 1;
}

uint16_t audio_scene_limiter_bench_command(uint32_t command,
        uint8_t *reply, uint16_t *reply_len)
{
    int i;
    if (command > 7U)
        return SDK_STATUS_BAD_REQUEST;
    XTime_GetTime(&limiter_bench.heartbeat);
    if (command == 1U) {
        if (limiter_bench.active || commit_in_progress || pending_valid ||
                save.phase != SAVE_IDLE || baseline_staged || calibration_staged)
            return SDK_STATUS_BUSY;
        if (ceiling_paula != 48U || ceiling_ax != 80U)
            return SDK_STATUS_BAD_REQUEST;
        for (i = 0; i < AUDIO_SCENE_COUNT; ++i)
            if (staging[i].has_draft) return SDK_STATUS_BUSY;
        for (i = 0; i < AUDIO_SCENE_OWNER_SLOTS; ++i)
            if (trims[i].paula || trims[i].ax) return SDK_STATUS_BUSY;
        /* Snapshot the live DSP threshold before any mutation: RESTORE
         * must return the operator's engagement, not a hardcoded bypass
         * (the boot image now arms 0.47 FS). One bounded I2C read,
         * serialized with poll-side I2C by the service loop. Zero is
         * rejected: no writer legitimately sets it, and restoring it
         * would clamp playback to silence. */
        if (audio_adau_limiter_threshold_get(
                &limiter_bench.original_threshold) != 0 ||
                limiter_bench.original_threshold == 0U)
            return SDK_STATUS_IO_ERROR;
        limiter_bench.original_index = active_scene_index;
        limiter_bench.original_scene = scenes[active_scene_index];
        limiter_bench.original_paula = baseline_paula;
        limiter_bench.original_ax = baseline_ax;
        limiter_bench.active = 1;
        limiter_bench.armed = 0;
        limiter_bench.failed = 0;
        limiter_bench.restoring = 0;
        limiter_bench.queued_restore = 0;
        command = 2U;
    }
    if (command >= 2U && command <= 5U) {
        if (!limiter_bench.active || limiter_bench.failed)
            return SDK_STATUS_IO_ERROR;
        if (limiter_bench.phase || commit_in_progress ||
                limiter_bench.restoring || limiter_bench.queued_restore)
            return SDK_STATUS_BUSY;
        limiter_bench.case_id = command;
        limiter_bench.threshold = command <= 3U ? 2097152U :
            command == 4U ? 2936013U : 3942646U;
        limiter_bench.phase = 1;
    } else if (command == 6U && limiter_bench.active) {
        limiter_bench.queued_restore = 1;
    }
    memset(reply, 0, 48);
    sdk_put_be32(reply, LIMITER_BENCH_MAGIC);
    sdk_put_be32(reply + 4, (limiter_bench.active ? 1U : 0U) |
        ((limiter_bench.phase || commit_in_progress ||
          limiter_bench.queued_restore) ? 2U : 0U) |
        (limiter_bench.failed ? 4U : 0U));
    sdk_put_be32(reply + 8, limiter_bench.case_id);
    sdk_put_be32(reply + 12, limiter_bench.threshold);
    sdk_put_be32(reply + 16, (uint32_t)baseline_paula |
        ((uint32_t)baseline_ax << 8));
    *reply_len = 48;
    return SDK_STATUS_OK;
}
