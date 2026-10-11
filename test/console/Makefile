# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c99

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGET := $(BUILD_DIR)/console_encode_test

CPPFLAGS += -I$(SRC_DIR)

SOURCES := console_encode_test.c $(SRC_DIR)/sdk_console_encode.c \
	$(SRC_DIR)/rfb/rfb_encode.c \
	$(SRC_DIR)/rfb/rfb_words.c
DEPS := $(SRC_DIR)/sdk_console_encode.h $(SRC_DIR)/rfb/rfb_encode.h \
	$(SRC_DIR)/sdk_mailbox.h $(SRC_DIR)/sdk_surface.h

.PHONY: test clean

test: $(TARGET)
	$(TARGET) $(SRC_DIR)/sdk_mailbox.c

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

clean:
	rm -rf $(BUILD_DIR)
