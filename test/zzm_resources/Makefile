CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections
LDFLAGS ?= -Wl,--gc-sections
SRC := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGET := $(BUILD_DIR)/zzm_resources_test

CPPFLAGS += -DMINIGL_HOST_TEST -DSMP_LOCK_HOST_TEST -I mock -I$(SRC)
SOURCES := zzm_resources_test.c $(SRC)/zzm_runtime.c $(SRC)/card_pool.c \
	$(SRC)/surface_allocator.c $(SRC)/sdk_palette.c $(SRC)/sdk_smp_lock.c
DEPS := $(SRC)/zzm_runtime.h $(SRC)/card_pool.h $(SRC)/surface_allocator.h \
	$(SRC)/sdk_palette.h

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES)

clean:
	rm -rf $(BUILD_DIR)
