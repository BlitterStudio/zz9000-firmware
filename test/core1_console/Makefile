CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c99

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGET := $(BUILD_DIR)/core1_console_test

CPPFLAGS += -DCORE1_CONSOLE_HOST_TEST -DCORE1_CONSOLE_DRAIN_BYTES=4 -I$(SRC_DIR)
SOURCES := core1_console_test.c $(SRC_DIR)/core1_console.c
DEPS := $(SRC_DIR)/core1_console.h
LDLIBS := -lpthread

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -rf $(BUILD_DIR)
