CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGET := $(BUILD_DIR)/card_pool_test

CPPFLAGS += -DSMP_LOCK_HOST_TEST -I$(SRC_DIR)

SOURCES := card_pool_test.c $(SRC_DIR)/card_pool.c $(SRC_DIR)/sdk_smp_lock.c
DEPS := $(SRC_DIR)/card_pool.h $(SRC_DIR)/memorymap.h $(SRC_DIR)/sdk_smp_lock.h

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

clean:
	rm -rf $(BUILD_DIR)
