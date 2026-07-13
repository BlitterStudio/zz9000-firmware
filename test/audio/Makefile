CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGET := $(BUILD_DIR)/audio_capture_test

CPPFLAGS += -I$(SRC_DIR)

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): audio_capture_test.c $(SRC_DIR)/audio_capture.c \
	$(SRC_DIR)/audio_capture.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ audio_capture_test.c \
		$(SRC_DIR)/audio_capture.c

clean:
	rm -rf $(BUILD_DIR)
