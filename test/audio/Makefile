CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
CAPTURE_TARGET := $(BUILD_DIR)/audio_capture_test
PROFILE_TARGET := $(BUILD_DIR)/audio_profile_test
PLAYBACK_TARGET := $(BUILD_DIR)/audio_playback_frontier_test
TARGETS := $(CAPTURE_TARGET) $(PROFILE_TARGET) $(PLAYBACK_TARGET)

CPPFLAGS += -I$(SRC_DIR)

.PHONY: test clean

test: $(TARGETS)
	$(CAPTURE_TARGET)
	$(PROFILE_TARGET)
	$(PLAYBACK_TARGET)

$(CAPTURE_TARGET): audio_capture_test.c $(SRC_DIR)/audio_capture.c \
	$(SRC_DIR)/audio_capture.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_capture_test.c \
		$(SRC_DIR)/audio_capture.c

$(PROFILE_TARGET): audio_profile_test.c $(SRC_DIR)/adau.h \
	$(SRC_DIR)/ax.h $(SRC_DIR)/sdk_crypto.c $(SRC_DIR)/sdk_crypto.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffunction-sections \
		-Wl,--gc-sections -o $@ audio_profile_test.c \
		$(SRC_DIR)/sdk_crypto.c

$(PLAYBACK_TARGET): audio_playback_frontier_test.c \
	$(SRC_DIR)/audio_playback_frontier.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_playback_frontier_test.c

clean:
	rm -rf $(BUILD_DIR)
