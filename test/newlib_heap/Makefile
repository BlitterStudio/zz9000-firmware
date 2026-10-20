CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror

FIRMWARE := ../../ZZ9000_proto.sdk/ZZ9000OS
SRC_DIR := $(FIRMWARE)/src
BSP_INCLUDE := ../../ZZ9000_proto.sdk/zz9000_bsp_new/ps7_cortexa9_0/include
BUILD_DIR := build
TARGET := $(BUILD_DIR)/newlib_heap_test

CPPFLAGS += -I$(SRC_DIR) -I$(BSP_INCLUDE)
SOURCES := newlib_heap_test.c $(SRC_DIR)/newlib_heap.c
DEPS := $(SRC_DIR)/newlib_heap.h $(SRC_DIR)/sdk_smp_lock.h \
	$(BSP_INCLUDE)/xil_types.h

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

clean:
	rm -rf $(BUILD_DIR)
