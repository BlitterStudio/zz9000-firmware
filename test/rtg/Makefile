CC ?= cc
CFLAGS ?= -O2 -g
PYTHON ?= python3

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGET := $(BUILD_DIR)/rtg_regression

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -I$(SRC_DIR)
WARNINGS := -Wall -Wextra -Wno-int-to-pointer-cast -Wno-unused-parameter
WARNINGS += -Wno-implicit-fallthrough -Wno-type-limits -Wno-unused-but-set-variable
LDLIBS := -lm

.PHONY: test bench clean

test: $(TARGET)
	$(TARGET)
	$(PYTHON) pitch_contract_test.py

bench: $(TARGET)
	$(TARGET) --bench

$(TARGET): rtg_regression.c $(SRC_DIR)/gfx.c $(SRC_DIR)/gfx.h $(SRC_DIR)/video.h $(SRC_DIR)/zz_video_modes.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ rtg_regression.c $(SRC_DIR)/gfx.c $(LDLIBS)

clean:
	rm -rf $(BUILD_DIR)
