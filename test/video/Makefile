CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGETS := \
	$(BUILD_DIR)/video_vdma_test \
	$(BUILD_DIR)/video_formatter_source_test \
	$(BUILD_DIR)/overlay_schedule_test

CPPFLAGS += -I$(SRC_DIR)

DEPS :=

.PHONY: test clean

test: $(TARGETS)
	$(BUILD_DIR)/video_vdma_test
	$(BUILD_DIR)/video_formatter_source_test
	$(BUILD_DIR)/overlay_schedule_test

$(BUILD_DIR)/video_vdma_test: video_vdma_test.c $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ video_vdma_test.c

$(BUILD_DIR)/video_formatter_source_test: video_formatter_source_test.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ video_formatter_source_test.c

$(BUILD_DIR)/overlay_schedule_test: overlay_schedule_test.c $(SRC_DIR)/overlay_schedule.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ overlay_schedule_test.c

clean:
	rm -rf $(BUILD_DIR)
