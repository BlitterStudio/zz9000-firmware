CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGET := $(BUILD_DIR)/gamma_test

CPPFLAGS += -I$(SRC_DIR)

SOURCES := gamma_test.c $(SRC_DIR)/gamma.c
DEPS := $(SRC_DIR)/gamma.h

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES) -lm

clean:
	rm -rf $(BUILD_DIR)
