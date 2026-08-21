CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
CAPTURE_TARGET := $(BUILD_DIR)/audio_capture_test
PROFILE_TARGET := $(BUILD_DIR)/audio_profile_test
PLAYBACK_TARGET := $(BUILD_DIR)/audio_playback_frontier_test
DRAIN_TARGET := $(BUILD_DIR)/audio_stream_drain_test
MP3_PROBE_TARGET := $(BUILD_DIR)/mp3_frame_probe_test
CONVERT_TARGET := $(BUILD_DIR)/audio_convert_test
TARGETS := $(CONVERT_TARGET) $(CAPTURE_TARGET) $(PROFILE_TARGET) $(PLAYBACK_TARGET) \
	$(DRAIN_TARGET) $(MP3_PROBE_TARGET) $(MPEG_LAYER2_TARGET)

CPPFLAGS += -I$(SRC_DIR)

.PHONY: test clean

test: $(TARGETS)
	$(CONVERT_TARGET)
	$(CAPTURE_TARGET)
	$(PROFILE_TARGET)
	$(PLAYBACK_TARGET)
	$(DRAIN_TARGET)
	$(MP3_PROBE_TARGET)
	$(MPEG_LAYER2_TARGET)

$(CONVERT_TARGET): audio_convert_test.c $(SRC_DIR)/audio_convert.c \
	$(SRC_DIR)/audio_convert.h $(SRC_DIR)/audio_convert_tables.c \
	$(SRC_DIR)/audio_convert_tables.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_convert_test.c \
		$(SRC_DIR)/audio_convert.c $(SRC_DIR)/audio_convert_tables.c -lm

$(CAPTURE_TARGET): audio_capture_test.c $(SRC_DIR)/audio_capture.c \
	$(SRC_DIR)/audio_capture.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_capture_test.c \
		$(SRC_DIR)/audio_capture.c

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

$(DRAIN_TARGET): audio_stream_drain_test.c \
	$(SRC_DIR)/audio_stream_drain.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_stream_drain_test.c

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

clean:
	rm -rf $(BUILD_DIR)
