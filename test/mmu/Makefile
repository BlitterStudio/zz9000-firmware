CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
MOCK_DIR := mock
BUILD_DIR := build
TARGET := $(BUILD_DIR)/mmu_page_test

CPPFLAGS += -DMMU_PAGE_HOST_TEST -I$(MOCK_DIR) -I$(SRC_DIR)
SOURCES := mmu_page_test.c $(SRC_DIR)/mmu_page.c
DEPS := $(MOCK_DIR)/xil_cache.h $(SRC_DIR)/mmu_page.h

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

clean:
	rm -rf $(BUILD_DIR)
