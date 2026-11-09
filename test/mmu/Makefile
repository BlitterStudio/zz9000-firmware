CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
MOCK_DIR := mock
BUILD_DIR := build
MMU_PAGE_TARGET := $(BUILD_DIR)/mmu_page_test
ZZAPP_AS_TARGET := $(BUILD_DIR)/zzapp_as_test

CPPFLAGS += -DMMU_PAGE_HOST_TEST -I$(MOCK_DIR) -I$(SRC_DIR)
MMU_PAGE_SOURCES := mmu_page_test.c $(SRC_DIR)/mmu_page.c
MMU_PAGE_DEPS := $(MOCK_DIR)/xil_cache.h $(SRC_DIR)/mmu_page.h
ZZAPP_AS_SOURCES := zzapp_as_test.c $(SRC_DIR)/zzapp_as.c $(SRC_DIR)/mmu_page.c
ZZAPP_AS_DEPS := $(MOCK_DIR)/xil_cache.h $(SRC_DIR)/zzapp_as.h \
	$(SRC_DIR)/memorymap.h $(SRC_DIR)/mmu_page.h

.PHONY: test clean

test: $(MMU_PAGE_TARGET) $(ZZAPP_AS_TARGET)
	$(MMU_PAGE_TARGET)
	$(ZZAPP_AS_TARGET)

$(MMU_PAGE_TARGET): $(MMU_PAGE_SOURCES) $(MMU_PAGE_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(MMU_PAGE_SOURCES)

$(ZZAPP_AS_TARGET): $(ZZAPP_AS_SOURCES) $(ZZAPP_AS_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(ZZAPP_AS_SOURCES)

clean:
	rm -rf $(BUILD_DIR)
