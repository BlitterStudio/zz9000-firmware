CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections -fno-pie -fno-builtin
LDFLAGS ?= -no-pie -Wl,--gc-sections

SRC := ../../ZZ9000_proto.sdk/ZZ9000OS/src
RUNTIME := ../../ZZ9000_proto.sdk/ZZ9000OS/modules/libzzm_rt/zzm_rt.c
BUILD_DIR := build
TARGET := $(BUILD_DIR)/zzm_rt_test

CPPFLAGS += -I$(SRC)
SOURCES := zzm_rt_test.c $(RUNTIME)
DEPS := $(SRC)/zzm_runtime.h $(SRC)/zzm_format.h

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES)

clean:
	rm -rf $(BUILD_DIR)
