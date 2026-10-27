CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGET := $(BUILD_DIR)/sd_boot_test

CPPFLAGS += -I$(SRC_DIR)

SOURCES := sd_boot_test.c $(SRC_DIR)/sd_boot.c
DEPS := $(SRC_DIR)/sd_boot.h $(SRC_DIR)/sd_storage.h

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

clean:
	rm -rf $(BUILD_DIR)
