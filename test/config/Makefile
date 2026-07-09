CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
MOCK_DIR := mock
BUILD_DIR := build
TARGET := $(BUILD_DIR)/config_test

CPPFLAGS += -I$(MOCK_DIR) -I$(SRC_DIR)

SOURCES := config_test.c $(SRC_DIR)/zz_config.c
DEPS := $(MOCK_DIR)/ff.h $(SRC_DIR)/zz_config.h $(SRC_DIR)/zz_video_modes.h

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

clean:
	rm -rf $(BUILD_DIR)
