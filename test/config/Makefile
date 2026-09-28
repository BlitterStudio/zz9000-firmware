CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
MOCK_DIR := mock
BUILD_DIR := build
TARGETS := $(BUILD_DIR)/config_test $(BUILD_DIR)/boot_order_test

CPPFLAGS += -I$(MOCK_DIR) -I$(SRC_DIR)

SOURCES := config_test.c $(SRC_DIR)/zz_config.c
DEPS := $(MOCK_DIR)/ff.h $(MOCK_DIR)/xil_cache.h $(SRC_DIR)/zz_config.h $(SRC_DIR)/zz_video_modes.h

.PHONY: test clean

test: $(TARGETS)
	$(BUILD_DIR)/config_test
	$(BUILD_DIR)/boot_order_test

$(BUILD_DIR)/boot_order_test: boot_order_test.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/config_test: $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

clean:
	rm -rf $(BUILD_DIR)
