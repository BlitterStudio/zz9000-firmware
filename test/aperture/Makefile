CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror
PYTHON ?= python3

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGET := $(BUILD_DIR)/aperture_layout_test

CPPFLAGS += -I$(SRC_DIR)
SOURCES := aperture_layout_test.c $(SRC_DIR)/sdk_aperture_layout.c
DEPS := $(SRC_DIR)/sdk_aperture_layout.h

.PHONY: test clean

test: $(TARGET)
	$(TARGET)
	$(PYTHON) rtl_contract_test.py

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

clean:
	rm -rf $(BUILD_DIR)
