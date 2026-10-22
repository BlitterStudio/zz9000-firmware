CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGET := $(BUILD_DIR)/allocator_test

CPPFLAGS += -I$(SRC_DIR)

SOURCES := allocator_test.c $(SRC_DIR)/surface_allocator.c \
	$(SRC_DIR)/sdk_smp_lock.c
DEPS := $(SRC_DIR)/surface_allocator.h $(SRC_DIR)/sdk_smp_lock.h

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

clean:
	rm -rf $(BUILD_DIR)
