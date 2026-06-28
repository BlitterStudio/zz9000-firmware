CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
MOCK_DIR := mock
BUILD_DIR := build
TARGET := $(BUILD_DIR)/fwupdate_test

CPPFLAGS += -I$(MOCK_DIR) -I$(SRC_DIR)

SOURCES := fwupdate_test.c fatfs_mock.c $(SRC_DIR)/fw_update.c
DEPS := fatfs_mock.h $(MOCK_DIR)/ff.h $(MOCK_DIR)/xil_cache.h $(SRC_DIR)/fw_update.h

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

clean:
	rm -rf $(BUILD_DIR)
