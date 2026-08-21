CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
CONVERT_TARGET := $(BUILD_DIR)/audio_convert_test
CAPTURE_TARGET := $(BUILD_DIR)/audio_capture_test
PROFILE_TARGET := $(BUILD_DIR)/audio_profile_test
PLAYBACK_TARGET := $(BUILD_DIR)/audio_playback_frontier_test
RATE_TARGET := $(BUILD_DIR)/audio_playback_rate_test
DRAIN_TARGET := $(BUILD_DIR)/audio_stream_drain_test
SCENE_TARGET := $(BUILD_DIR)/audio_scene_test
METER_TARGET := $(BUILD_DIR)/audio_meter_test
CONTROL_TARGET := $(BUILD_DIR)/audio_control_test
MP3_PROBE_TARGET := $(BUILD_DIR)/mp3_frame_probe_test
MPEG_LAYER2_TARGET := $(BUILD_DIR)/mpeg_layer2_decode_test
TARGETS := $(CONVERT_TARGET) $(CAPTURE_TARGET) $(PROFILE_TARGET) \
	$(PLAYBACK_TARGET) $(RATE_TARGET) $(DRAIN_TARGET) $(SCENE_TARGET) \
	$(METER_TARGET) $(CONTROL_TARGET) $(MP3_PROBE_TARGET) \
	$(MPEG_LAYER2_TARGET)

CPPFLAGS += -I$(SRC_DIR)

.PHONY: test clean check-tables

test: $(TARGETS)
	$(CONVERT_TARGET)
	$(CAPTURE_TARGET)
	$(RATE_TARGET)
	$(PROFILE_TARGET)
	$(PLAYBACK_TARGET)
	$(DRAIN_TARGET)
	$(SCENE_TARGET)
	$(METER_TARGET)
	$(CONTROL_TARGET)
	$(MPEG_LAYER2_TARGET)
$(CONVERT_TARGET): audio_convert_test.c $(SRC_DIR)/audio_convert.c \
	$(SRC_DIR)/audio_convert.h $(SRC_DIR)/audio_convert_tables.c \
	$(SRC_DIR)/audio_convert_tables.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_convert_test.c \
		$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert_tables.c -lm

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

$(DRAIN_TARGET): audio_stream_drain_test.c \
	$(SRC_DIR)/audio_stream_drain.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_stream_drain_test.c

$(SCENE_TARGET): audio_scene_test.c $(SRC_DIR)/audio_scene.c \
	$(SRC_DIR)/audio_scene.h $(SRC_DIR)/ax.h \
	$(SRC_DIR)/audio_dsp_gain.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_scene_test.c \
		$(SRC_DIR)/audio_scene.c -lm


# Includes audio_scene.c directly (single TU) for meter-state seeding;
# pthread drives the concurrent single-writer snapshot test.
$(METER_TARGET): audio_meter_test.c $(SRC_DIR)/audio_scene.c \
	$(SRC_DIR)/audio_scene.h $(SRC_DIR)/sdk_mailbox.h \
	$(SRC_DIR)/ax.h $(SRC_DIR)/audio_dsp_gain.h \
	$(SRC_DIR)/memorymap.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_meter_test.c -pthread -lm

# The control-plane dispatch links the scene module (arbiter state)
# with recording stubs for the five ax.h DSP setters supplied by the
# test, so every master-chain write is observable in order.
$(CONTROL_TARGET): audio_control_test.c $(SRC_DIR)/sdk_audio_control.c \
	$(SRC_DIR)/sdk_audio_control.h $(SRC_DIR)/audio_scene.c \
	$(SRC_DIR)/audio_scene.h $(SRC_DIR)/sdk_mailbox.h \
	$(SRC_DIR)/ax.h $(SRC_DIR)/audio_dsp_gain.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_control_test.c \
		$(SRC_DIR)/sdk_audio_control.c $(SRC_DIR)/audio_scene.c -lm

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

check-tables:
	python3 ../../util/gen_audio_convert_tables.py
	git diff --exit-code -- ../../ZZ9000_proto.sdk/ZZ9000OS/src/audio_convert_tables.c \
		../../ZZ9000_proto.sdk/ZZ9000OS/src/audio_convert_tables.h

clean:
	rm -rf $(BUILD_DIR)
