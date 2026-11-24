CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
MOCK_DIR := mock
BUILD_DIR := build
CONVERT_TARGET := $(BUILD_DIR)/audio_convert_test
CONVERT_EXACT_TARGET := $(BUILD_DIR)/audio_convert_exact_test
CAPTURE_TARGET := $(BUILD_DIR)/audio_capture_test
PROFILE_TARGET := $(BUILD_DIR)/audio_profile_test
PLAYBACK_TARGET := $(BUILD_DIR)/audio_playback_frontier_test
RATE_TARGET := $(BUILD_DIR)/audio_playback_rate_test
DRAIN_TARGET := $(BUILD_DIR)/audio_stream_drain_test
CLOSE_TARGET := $(BUILD_DIR)/audio_stream_close_test
PRECONVERT_TARGET := $(BUILD_DIR)/audio_pump_preconvert_test
RECLAIM_TARGET := $(BUILD_DIR)/core1_reclaim_test
SCENE_TARGET := $(BUILD_DIR)/audio_scene_test
METER_TARGET := $(BUILD_DIR)/audio_meter_test
CONTROL_TARGET := $(BUILD_DIR)/audio_control_test
CONFIG_TARGET := $(BUILD_DIR)/audio_config_test
MP3_PROBE_TARGET := $(BUILD_DIR)/mp3_frame_probe_test
MPEG_LAYER2_TARGET := $(BUILD_DIR)/mpeg_layer2_decode_test
FABRIC_TARGET := $(BUILD_DIR)/audio_fabric_test
RING_TARGET := $(BUILD_DIR)/audio_fabric_ring_test
BENCH_SMOKE_TARGET := $(BUILD_DIR)/bench_smoke_test
MEDIA_VIEW_TARGET := $(BUILD_DIR)/audio_pump_media_view_test
STREAM_DECODE_TARGET := $(BUILD_DIR)/audio_stream_decode_test
REDUCE_TARGET := $(BUILD_DIR)/audio_reduce_test
VOICE_TARGET := $(BUILD_DIR)/audio_voice_test
VOICE_PT4_TARGET := $(BUILD_DIR)/audio_voice_pt4_test
VOICE_SESSION_TARGET := $(BUILD_DIR)/audio_voice_session_test
VOICE_BASELINE_TARGET := $(BUILD_DIR)/audio_voice_baseline_test
VOICE_RAMP_TARGET := $(BUILD_DIR)/audio_voice_ramp_test
VOICE_LIMITER_TARGET := $(BUILD_DIR)/audio_voice_limiter_test
VOICE_CUBIC_TARGET := $(BUILD_DIR)/audio_voice_cubic_test
VOICE_CUBIC_GUARD_TARGET := $(BUILD_DIR)/audio_voice_cubic_guard_test
VOICE_OVERSAMPLE_TARGET := $(BUILD_DIR)/audio_voice_oversample_test
VOICE_LINEAR_GUARD_TARGET := $(BUILD_DIR)/audio_voice_linear_guard_test
VOICE_OS_CUBIC_TARGET := $(BUILD_DIR)/audio_voice_oversample_cubic_test
VOICE_OS_CUBIC_COMPAT_TARGET := $(BUILD_DIR)/audio_voice_oversample_cubic_compat_test
VOICE_OS_COMPAT_TARGET := $(BUILD_DIR)/audio_voice_oversample_compat_test
VOICE_PARITY_TARGET := $(BUILD_DIR)/audio_voice_abi_parity_test
TARGETS := $(CONVERT_TARGET) $(CONVERT_EXACT_TARGET) $(CAPTURE_TARGET) $(PROFILE_TARGET) \
	$(PLAYBACK_TARGET) $(RATE_TARGET) $(PRECONVERT_TARGET) \
	$(MEDIA_VIEW_TARGET) $(DRAIN_TARGET) $(CLOSE_TARGET) $(RECLAIM_TARGET) \
	$(SCENE_TARGET) $(METER_TARGET) $(CONTROL_TARGET) $(CONFIG_TARGET) \
	$(MP3_PROBE_TARGET) $(MPEG_LAYER2_TARGET) $(FABRIC_TARGET) \
	$(RING_TARGET) $(BENCH_SMOKE_TARGET) $(STREAM_DECODE_TARGET) \
	$(REDUCE_TARGET) $(VOICE_TARGET) $(VOICE_PT4_TARGET) \
	$(VOICE_BASELINE_TARGET) \
	$(VOICE_RAMP_TARGET) $(VOICE_LIMITER_TARGET) $(VOICE_CUBIC_TARGET) \
	$(VOICE_CUBIC_GUARD_TARGET) $(VOICE_OVERSAMPLE_TARGET) \
	$(VOICE_LINEAR_GUARD_TARGET) $(VOICE_OS_CUBIC_TARGET) \
	$(VOICE_OS_CUBIC_COMPAT_TARGET) $(VOICE_OS_COMPAT_TARGET) \
	$(VOICE_PARITY_TARGET) \
	$(VOICE_SESSION_TARGET)

# mock/ shadows ff.h and xil_cache.h for the TUs (zz_config.c) whose
# real firmware dependencies are FatFs and the Zynq cache API. The
# fabric lease test additionally compiles xil_cache.h in recording
# mode (-DXIL_CACHE_MOCK_RECORD) so its cache-fidelity scenario can
# assert the exact invalidate/flush ranges; every other target keeps
CPPFLAGS += -I$(MOCK_DIR) -I$(SRC_DIR) -Ihost_stubs

.PHONY: test clean check-tables speed

test: $(TARGETS)
	$(CONVERT_TARGET)
	$(CONVERT_EXACT_TARGET)
	$(CAPTURE_TARGET)
	$(RATE_TARGET)
	$(PRECONVERT_TARGET)
	$(PROFILE_TARGET)
	$(PLAYBACK_TARGET)
	$(DRAIN_TARGET)
	$(CLOSE_TARGET)
	$(RECLAIM_TARGET)
	$(SCENE_TARGET)
	$(CONFIG_TARGET)
	$(METER_TARGET)
	$(CONTROL_TARGET)
	$(MPEG_LAYER2_TARGET)
	$(FABRIC_TARGET) --srcdir $(SRC_DIR)
	$(MEDIA_VIEW_TARGET)
	$(RING_TARGET)
	$(BENCH_SMOKE_TARGET)
	$(STREAM_DECODE_TARGET) fixtures/tone.mp3
	$(REDUCE_TARGET)
	$(VOICE_TARGET)
	$(VOICE_BASELINE_TARGET)
	$(VOICE_PT4_TARGET)
	$(VOICE_SESSION_TARGET)
	$(VOICE_RAMP_TARGET)
	$(VOICE_PARITY_TARGET)
	$(VOICE_LIMITER_TARGET)
	$(VOICE_CUBIC_TARGET)
	$(VOICE_CUBIC_GUARD_TARGET)
	$(VOICE_OVERSAMPLE_TARGET)
	$(VOICE_LINEAR_GUARD_TARGET)
	$(VOICE_OS_CUBIC_TARGET)
	$(VOICE_OS_CUBIC_COMPAT_TARGET)
	$(VOICE_OS_COMPAT_TARGET)

# U2 fabric compositor: the characterization golden (pump_golden.c, a
# verbatim capture of the pre-fabric pump) drives the parity scenarios
# against audio_fabric.c + audio_fabric_lease.c through the host seam
# stubs in the shared harness (fabric_test_common.c).
$(FABRIC_TARGET): audio_fabric_test.c pump_golden.c pump_golden.h \
	fabric_test_common.c fabric_test_common.h \
	$(SRC_DIR)/audio_fabric.c $(SRC_DIR)/audio_fabric_lease.c \
	$(SRC_DIR)/audio_fabric.h $(SRC_DIR)/audio_fabric_internal.h \
	$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert.h \
	$(SRC_DIR)/audio_convert_tables.c $(SRC_DIR)/audio_convert_tables.h \
	$(SRC_DIR)/audio_playback_frontier.h $(SRC_DIR)/ax.h \
	$(SRC_DIR)/memorymap.h $(SRC_DIR)/sdk_mailbox.h \
	$(SRC_DIR)/audio_stream_drain.h \
	$(MOCK_DIR)/xil_cache.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAUDIO_FABRIC_HOST_TEST \
		-DXIL_CACHE_MOCK_MODEL \
		-o $@ audio_fabric_test.c \
		pump_golden.c fabric_test_common.c \
		$(SRC_DIR)/audio_fabric.c $(SRC_DIR)/audio_fabric_lease.c \
		$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert_tables.c -lm

# Bench smoke: drives the bench protocol end to end (boost/restore,
# watchdog, warm reset, fail-closed BEGIN, leg validation, parity boot
# default) against the scene and control modules. host_stubs/ supplies
# the XTime shim backed by the smoke's manual clock.
$(BENCH_SMOKE_TARGET): bench_smoke.c audio_scene_test.c \
	$(SRC_DIR)/sdk_audio_control.c $(SRC_DIR)/audio_scene.c \
	$(SRC_DIR)/audio_scene.h $(SRC_DIR)/sdk_audio_control.h \
	$(SRC_DIR)/audio_limiter_bench.h $(SRC_DIR)/audio_limiter_bench.inc \
	$(SRC_DIR)/limiter_parameter.h $(SRC_DIR)/zz_config.c \
	$(SRC_DIR)/zz_config.h $(MOCK_DIR)/ff.h $(MOCK_DIR)/xil_cache.h \
	host_stubs/xtime_l.h fatfs_mock.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ bench_smoke.c \
		$(SRC_DIR)/sdk_audio_control.c $(SRC_DIR)/audio_scene.c \
		$(SRC_DIR)/zz_config.c fatfs_mock.c -lm

# U3 direct-ring lease plane: the SDK-facing acquire/release/state API
# against the same two firmware TUs, with xil_cache.h compiled in
# recording mode so the cache-fidelity scenario asserts the exact
# invalidate/flush ranges.
$(RING_TARGET): audio_fabric_ring_test.c \
	fabric_test_common.c fabric_test_common.h \
	$(SRC_DIR)/audio_fabric.c $(SRC_DIR)/audio_fabric_lease.c \
	$(SRC_DIR)/audio_fabric.h $(SRC_DIR)/audio_fabric_internal.h \
	$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert.h \
	$(SRC_DIR)/audio_convert_tables.c $(SRC_DIR)/audio_convert_tables.h \
	$(SRC_DIR)/audio_playback_frontier.h $(SRC_DIR)/ax.h \
	$(SRC_DIR)/memorymap.h $(SRC_DIR)/sdk_mailbox.h \
	$(SRC_DIR)/sdk_aperture_layout.h \
	$(MOCK_DIR)/xil_cache.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAUDIO_FABRIC_HOST_TEST \
		-DXIL_CACHE_MOCK_RECORD \
		-o $@ audio_fabric_ring_test.c fabric_test_common.c \
		$(SRC_DIR)/audio_fabric.c $(SRC_DIR)/audio_fabric_lease.c \
		$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert_tables.c -lm

$(CONVERT_TARGET): audio_convert_test.c $(SRC_DIR)/audio_convert.c \
	$(SRC_DIR)/audio_convert.h $(SRC_DIR)/audio_convert_tables.c \
	$(SRC_DIR)/audio_convert_tables.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_convert_test.c \
		$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert_tables.c -lm

$(CONVERT_EXACT_TARGET): audio_convert_exact_test.c \
	$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert.h \
	$(SRC_DIR)/audio_convert_tables.c $(SRC_DIR)/audio_convert_tables.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_convert_exact_test.c \
		$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert_tables.c

$(CAPTURE_TARGET): audio_capture_test.c $(SRC_DIR)/audio_capture.c \
	$(SRC_DIR)/audio_capture.h $(SRC_DIR)/audio_convert.c \
	$(SRC_DIR)/audio_convert.h $(SRC_DIR)/audio_convert_tables.c \
	$(SRC_DIR)/audio_convert_tables.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_capture_test.c \
		$(SRC_DIR)/audio_capture.c $(SRC_DIR)/audio_convert.c \
		$(SRC_DIR)/audio_convert_tables.c -lm

$(PROFILE_TARGET): audio_profile_test.c $(SRC_DIR)/adau.h \
	$(SRC_DIR)/audio_dsp_gain.h \
	$(SRC_DIR)/ax.h $(SRC_DIR)/sdk_crypto.c $(SRC_DIR)/sdk_crypto.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffunction-sections \
		-Wl,--gc-sections -o $@ audio_profile_test.c \
		$(SRC_DIR)/sdk_crypto.c -lm

$(PLAYBACK_TARGET): audio_playback_frontier_test.c \
	$(SRC_DIR)/audio_playback_frontier.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_playback_frontier_test.c

$(RATE_TARGET): audio_playback_rate_test.c $(SRC_DIR)/audio_playback_rate.h \
	$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert.h \
	$(SRC_DIR)/audio_convert_tables.c $(SRC_DIR)/audio_convert_tables.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_playback_rate_test.c \
		$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert_tables.c -lm

$(PRECONVERT_TARGET): audio_pump_preconvert_test.c \
	$(SRC_DIR)/audio_pump_preconvert.c \
	$(SRC_DIR)/audio_pump_preconvert.h \
	$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert.h \
	$(SRC_DIR)/audio_convert_tables.c $(SRC_DIR)/audio_convert_tables.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_pump_preconvert_test.c \
		$(SRC_DIR)/audio_pump_preconvert.c \
		$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert_tables.c -lm

# Media-session preconvert view: the exact 48 kHz view <-> session cursor
# mapping that keeps the fabric ISR on the memcpy-only path (drivers#83).
$(MEDIA_VIEW_TARGET): audio_pump_media_view_test.c \
	$(SRC_DIR)/audio_pump_media_view.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_pump_media_view_test.c

$(DRAIN_TARGET): audio_stream_drain_test.c \
	$(SRC_DIR)/audio_stream_drain.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_stream_drain_test.c

$(CLOSE_TARGET): audio_stream_close_test.c \
	$(SRC_DIR)/audio_stream_close.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_stream_close_test.c

$(RECLAIM_TARGET): core1_reclaim_test.c \
	$(SRC_DIR)/sdk_core1_reclaim.c $(SRC_DIR)/sdk_core1_reclaim.h \
	$(SRC_DIR)/scheduler.c $(SRC_DIR)/scheduler.h \
	$(SRC_DIR)/audio_stream_close.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTASKQ_HOST_TEST -o $@ \
		core1_reclaim_test.c $(SRC_DIR)/sdk_core1_reclaim.c $(SRC_DIR)/scheduler.c

$(SCENE_TARGET): audio_scene_test.c $(SRC_DIR)/audio_scene.c \
	$(SRC_DIR)/audio_scene.h $(SRC_DIR)/ax.h \
	$(SRC_DIR)/audio_dsp_gain.h $(SRC_DIR)/zz_config.c \
	$(SRC_DIR)/zz_config.h $(MOCK_DIR)/ff.h $(MOCK_DIR)/xil_cache.h \
	fatfs_mock.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_scene_test.c \
		$(SRC_DIR)/audio_scene.c $(SRC_DIR)/zz_config.c fatfs_mock.c -lm


# Includes audio_scene.c directly (single TU) for meter-state seeding;
# pthread drives the concurrent single-writer snapshot test.
$(METER_TARGET): audio_meter_test.c $(SRC_DIR)/audio_scene.c \
	$(SRC_DIR)/audio_scene.h $(SRC_DIR)/sdk_mailbox.h \
	$(SRC_DIR)/ax.h $(SRC_DIR)/audio_dsp_gain.h \
	$(SRC_DIR)/memorymap.h $(SRC_DIR)/zz_config.c \
	$(SRC_DIR)/zz_config.h $(MOCK_DIR)/ff.h $(MOCK_DIR)/xil_cache.h \
	fatfs_mock.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_meter_test.c \
		$(SRC_DIR)/zz_config.c fatfs_mock.c -pthread -lm

# The control-plane dispatch links the scene module (arbiter state)
# with recording stubs for the five ax.h DSP setters supplied by the
# test, so every master-chain write is observable in order. zz_config.c
# + the FatFs mock ride along since U5: SCENE_SAVE reaches the real
# persistence writer.
$(CONTROL_TARGET): audio_control_test.c $(SRC_DIR)/sdk_audio_control.c \
	$(SRC_DIR)/sdk_audio_control.h $(SRC_DIR)/audio_scene.c \
	$(SRC_DIR)/audio_scene.h $(SRC_DIR)/sdk_mailbox.h \
	$(SRC_DIR)/ax.h $(SRC_DIR)/audio_dsp_gain.h \
	$(SRC_DIR)/zz_config.c $(SRC_DIR)/zz_config.h \
	$(MOCK_DIR)/ff.h $(MOCK_DIR)/xil_cache.h fatfs_mock.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_control_test.c \
		$(SRC_DIR)/sdk_audio_control.c $(SRC_DIR)/audio_scene.c \
		$(SRC_DIR)/zz_config.c fatfs_mock.c -lm

# U5: the CFG audio keys, boot fold-in and the atomic temp-then-replace
# writer, against the in-memory FatFs mock with failure injection.
$(CONFIG_TARGET): audio_config_test.c $(SRC_DIR)/audio_scene.c \
	$(SRC_DIR)/audio_scene.h $(SRC_DIR)/zz_config.c \
	$(SRC_DIR)/zz_config.h $(SRC_DIR)/ax.h \
	$(MOCK_DIR)/ff.h $(MOCK_DIR)/xil_cache.h fatfs_mock.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_config_test.c \
		$(SRC_DIR)/audio_scene.c $(SRC_DIR)/zz_config.c fatfs_mock.c \
		-lm

$(MP3_PROBE_TARGET): mp3_frame_probe_test.c \
	$(SRC_DIR)/mp3/decode_mp3.c $(SRC_DIR)/mp3/mp3.h \
	$(SRC_DIR)/mp3/minimp3.h $(SRC_DIR)/mp3/minimp3_ex.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ mp3_frame_probe_test.c \
		$(SRC_DIR)/mp3/decode_mp3.c

$(MPEG_LAYER2_TARGET): mpeg_layer2_decode_test.c \
	$(SRC_DIR)/mp3/mp3_backend_config.h $(SRC_DIR)/mp3/minimp3.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ mpeg_layer2_decode_test.c

$(STREAM_DECODE_TARGET): audio_stream_decode_test.c \
	$(SRC_DIR)/sdk_audio_stream.c $(SRC_DIR)/sdk_audio_stream.h \
	$(SRC_DIR)/audio_reduce.c $(SRC_DIR)/audio_reduce.h \
	$(SRC_DIR)/audio_reduce_table.h \
	$(SRC_DIR)/sdk_audio_codec_io.h $(SRC_DIR)/audio_stream_drain.h \
	$(SRC_DIR)/mp3/decode_mp3.c $(SRC_DIR)/mp3/mp3.h \
	$(SRC_DIR)/mp3/minimp3.h $(MOCK_DIR)/xil_cache.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_stream_decode_test.c \
		$(SRC_DIR)/sdk_audio_stream.c $(SRC_DIR)/audio_reduce.c \
		$(SRC_DIR)/mp3/decode_mp3.c -lm

# Reducer (output_hz / output_channels): divisor selection, frequency
# response, chunk-split invariance, mixdown and duplication.
$(REDUCE_TARGET): audio_reduce_test.c $(SRC_DIR)/audio_reduce.c \
	$(SRC_DIR)/audio_reduce.h $(SRC_DIR)/audio_reduce_table.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_reduce_test.c \
		$(SRC_DIR)/audio_reduce.c -lm

# Voice engine core: position and loop math, command timing, rate rules,
# mixing and saturation against the integer-only engine. The test also
# includes the SDK's zz9k/voice.h to pin its timing helpers to the engine.
SDK_INC := ../../sdk/include
$(VOICE_TARGET): audio_voice_test.c $(SRC_DIR)/audio_voice.c \
	$(SRC_DIR)/audio_voice.h $(SDK_INC)/zz9k/voice.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I$(SDK_INC) $(CFLAGS) -o $@ audio_voice_test.c \
		$(SRC_DIR)/audio_voice.c

# Exact comparison against the frozen master engine (baseline/): a seeded
# random corpus, byte-identical PCM and identical engine state. The count of
# sequences is ZZ9K_VOICE_BASELINE_SEQS (default 10000).
$(VOICE_BASELINE_TARGET): audio_voice_baseline_test.c \
	baseline/audio_voice_baseline.c baseline/audio_voice_baseline.h \
	$(SRC_DIR)/audio_voice.c $(SRC_DIR)/audio_voice.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Ibaseline $(CFLAGS) -o $@ audio_voice_baseline_test.c \
		baseline/audio_voice_baseline.c $(SRC_DIR)/audio_voice.c

# Ramping and fade slots against a frame-by-frame model, plus directed cases.
# ZZ9K_VOICE_RAMP_SEQS sets the random sequences (default 20000).
$(VOICE_RAMP_TARGET): audio_voice_ramp_test.c \
	baseline/audio_voice_baseline.c baseline/audio_voice_baseline.h \
	$(SRC_DIR)/audio_voice.c $(SRC_DIR)/audio_voice.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Ibaseline $(CFLAGS) -o $@ audio_voice_ramp_test.c \
		baseline/audio_voice_baseline.c $(SRC_DIR)/audio_voice.c

# The optional static soft knee (CONFIG limiter) against an independent
# model of the mix and the rational curve: dense sweeps, segment edges, the
# maximum master with 32 voices and 32 fade tails, CONFIG timing and split
# renders, the CONFIG validation matrix.
$(VOICE_LIMITER_TARGET): audio_voice_limiter_test.c \
	$(SRC_DIR)/audio_voice.c $(SRC_DIR)/audio_voice.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_voice_limiter_test.c \
		$(SRC_DIR)/audio_voice.c


# Per-voice interpolation and the cubic reader: real-arithmetic, unrolled
# stream and frame-by-frame model references, records, tails, formats, the
# overshoot at the largest gain and master, the limiter, split renders.
$(VOICE_CUBIC_TARGET): audio_voice_cubic_test.c \
	$(SRC_DIR)/audio_voice.c $(SRC_DIR)/audio_voice.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_voice_cubic_test.c \
		$(SRC_DIR)/audio_voice.c -lm

# The cubic reader reads nothing outside the four taps of a frame it mixes:
# the bank is a page between two protected pages and the sample is flush
# with them, so any read of an unused lane is a fault (Linux only).
$(VOICE_CUBIC_GUARD_TARGET): audio_voice_cubic_guard_test.c \
	$(SRC_DIR)/audio_voice.c $(SRC_DIR)/audio_voice.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_voice_cubic_guard_test.c \
		$(SRC_DIR)/audio_voice.c

# 2x oversampling (CONFIG OVERSAMPLE): the 96 kHz stream of the engine on a
# half-frame time base, convolved with the taps of audio_reduce_table.h by
# an independent direct convolution; impulse, level, master, ramp, history,
# acceptance and alias cases worked out by hand. ZZ9K_VOICE_OS_SEQS sets the
# random scenarios (default 3000).
$(VOICE_OVERSAMPLE_TARGET): audio_voice_oversample_test.c \
	$(SRC_DIR)/audio_voice.c $(SRC_DIR)/audio_voice.h \
	$(SRC_DIR)/audio_reduce_table.h $(SDK_INC)/zz9k/voice.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I$(SDK_INC) $(CFLAGS) -o $@ audio_voice_oversample_test.c \
		$(SRC_DIR)/audio_voice.c -lm

# The same test built with AUDIO_VOICE_OVERSAMPLE_CUBIC=1, the engine that
# accepts the cubic interpolation under 2x (qualification only): its random
# scenarios then draw the cubic selectors too, against the 96 kHz time base.
$(VOICE_OS_COMPAT_TARGET): audio_voice_oversample_test.c \
	$(SRC_DIR)/audio_voice.c $(SRC_DIR)/audio_voice.h \
	$(SRC_DIR)/audio_reduce_table.h $(SDK_INC)/zz9k/voice.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I$(SDK_INC) $(CFLAGS) -DAUDIO_VOICE_OVERSAMPLE_CUBIC=1 \
		-o $@ audio_voice_oversample_test.c $(SRC_DIR)/audio_voice.c -lm

# The linear reader reads only the two frames of each blend (the bank is a
# page between two protected pages, flush with the sample or exactly the
# sample) and mixes what an independent model of the documented rules says,
# also under 2x oversampling (Linux only). Its vector path runs on ARM only.
$(VOICE_LINEAR_GUARD_TARGET): audio_voice_linear_guard_test.c \
	$(SRC_DIR)/audio_voice.c $(SRC_DIR)/audio_voice.h \
	$(SRC_DIR)/audio_reduce_table.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_voice_linear_guard_test.c \
		$(SRC_DIR)/audio_voice.c

# 2x oversampling and the cubic interpolation: the records that combine them
# are rejected atomically (the product, AUDIO_VOICE_OVERSAMPLE_CUBIC 0), and
# with the macro at 1 (this second build, for qualification only) they are
# accepted and the cubic voice mixes under 2x.
$(VOICE_OS_CUBIC_TARGET): audio_voice_oversample_cubic_test.c \
	$(SRC_DIR)/audio_voice.c $(SRC_DIR)/audio_voice.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_voice_oversample_cubic_test.c \
		$(SRC_DIR)/audio_voice.c

$(VOICE_OS_CUBIC_COMPAT_TARGET): audio_voice_oversample_cubic_test.c \
	$(SRC_DIR)/audio_voice.c $(SRC_DIR)/audio_voice.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAUDIO_VOICE_OVERSAMPLE_CUBIC=1 -o $@ \
		audio_voice_oversample_cubic_test.c $(SRC_DIR)/audio_voice.c

# The voice constants of the SDK header and of the firmware headers agree.
$(VOICE_PARITY_TARGET): audio_voice_abi_parity_test.c \
	$(SRC_DIR)/audio_voice.h $(SRC_DIR)/audio_voice_session.h \
	$(SDK_INC)/zz9k/voice.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I$(SDK_INC) $(CFLAGS) -o $@ \
		audio_voice_abi_parity_test.c

# Host timing of the render path, baseline engine against the current one.
# Not part of `make test`: run `make speed`.
VOICE_SPEED_TARGET := $(BUILD_DIR)/audio_voice_speed
$(VOICE_SPEED_TARGET): audio_voice_speed.c \
	baseline/audio_voice_baseline.c baseline/audio_voice_baseline.h \
	$(SRC_DIR)/audio_voice.c $(SRC_DIR)/audio_voice.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Ibaseline $(CFLAGS) -o $@ audio_voice_speed.c \
		baseline/audio_voice_baseline.c $(SRC_DIR)/audio_voice.c

speed: $(VOICE_SPEED_TARGET)
	$(VOICE_SPEED_TARGET)

# Voice engine against libpt4 (the ProTracker 4.1 replayer, vendored under
# sdk/tools/libpt4): voice events from zz9k-voice-pt4 must reproduce
# libpt4's WIDE voice positions exactly and its output within rounding.
PT4_DIR := ../../sdk/tools
PT4_SRCS := $(addprefix $(PT4_DIR)/libpt4/src/,load.c replay.c voice.c \
	api.c tables.c float.c)
$(VOICE_PT4_TARGET): audio_voice_pt4_test.c $(SRC_DIR)/audio_voice.c \
	$(SRC_DIR)/audio_voice.h $(PT4_DIR)/zz9k-voice-pt4.c \
	$(PT4_DIR)/zz9k-voice-pt4.h $(PT4_SRCS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I$(PT4_DIR) $(CFLAGS) -o $@ audio_voice_pt4_test.c \
		$(SRC_DIR)/audio_voice.c $(PT4_DIR)/zz9k-voice-pt4.c $(PT4_SRCS)

# Voice session: the ring, event queue, render, status, reset, heartbeat and
# upload paths against the real compositor through the fabric host seam
# (the lease harness), with xil_cache.h in recording mode for the
# cache-fidelity scenario and the XTime shim as the heartbeat clock.
$(VOICE_SESSION_TARGET): audio_voice_session_test.c \
	fabric_test_common.c fabric_test_common.h \
	$(SRC_DIR)/audio_voice_session.c $(SRC_DIR)/audio_voice_session.h \
	$(SRC_DIR)/audio_voice.c $(SRC_DIR)/audio_voice.h \
	$(SRC_DIR)/audio_fabric.c $(SRC_DIR)/audio_fabric_lease.c \
	$(SRC_DIR)/audio_fabric.h $(SRC_DIR)/audio_fabric_internal.h \
	$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert.h \
	$(SRC_DIR)/audio_convert_tables.c $(SRC_DIR)/audio_convert_tables.h \
	$(SRC_DIR)/audio_playback_frontier.h $(SRC_DIR)/ax.h \
	$(SRC_DIR)/memorymap.h $(SRC_DIR)/sdk_mailbox.h \
	$(SRC_DIR)/sdk_aperture_layout.h \
	$(MOCK_DIR)/xil_cache.h host_stubs/xtime_l.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAUDIO_FABRIC_HOST_TEST \
		-DXIL_CACHE_MOCK_RECORD \
		-o $@ audio_voice_session_test.c fabric_test_common.c \
		$(SRC_DIR)/audio_voice_session.c $(SRC_DIR)/audio_voice.c \
		$(SRC_DIR)/audio_fabric.c $(SRC_DIR)/audio_fabric_lease.c \
		$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert_tables.c -lm

# The limiter table in audio_voice.c is the marked block the generator emits.
check-tables:
	python3 ../../util/gen_audio_convert_tables.py
	git diff --exit-code -- ../../ZZ9000_proto.sdk/ZZ9000OS/src/audio_convert_tables.c \
		../../ZZ9000_proto.sdk/ZZ9000OS/src/audio_convert_tables.h
	python3 ../../util/gen_voice_limiter_table.py --check $(SRC_DIR)/audio_voice.c

clean:
	rm -rf $(BUILD_DIR)
