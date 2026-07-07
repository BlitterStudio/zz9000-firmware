# ZZ9000FSBL Standalone Makefile
#
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Replaces the Xilinx SDK auto-generated FSBL build. Same toolchain
# constraint as ZZ9000OS (see its Makefile header): Arm GNU Toolchain
# with newlib, NOT Debian's picolibc-based gcc-arm-none-eabi.
#
# ps7_init_gpl.c/h are vendored from the Vivado build's sysdef by
# util/refresh_ps7_init.sh and MUST be refreshed after any PS (PCW_*)
# configuration change — build_fsbl.sh gates on this.

CC = arm-none-eabi-gcc
SIZE = arm-none-eabi-size

CPU_FLAGS = -mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard

SRC_DIR = src
BSP_DIR = ../zz9000_bsp_new/ps7_cortexa9_0
BSP_INC = $(BSP_DIR)/include
BSP_LIB = $(BSP_DIR)/lib
BUILD_DIR = build

INCLUDES = -I$(SRC_DIR) -I$(BSP_INC)

# Vintage Xilinx sources: keep the same suppressions the OS build needs
# for the shared BSP headers. Debug prints: uncomment FSBL_DEBUG_INFO.
CFLAGS = $(CPU_FLAGS) $(INCLUDES) -Wall -c -O2
CFLAGS += -fno-strict-aliasing
CFLAGS += -Wno-incompatible-pointer-types -Wno-implicit-function-declaration
#CFLAGS += -DFSBL_DEBUG_INFO

# Stock FSBL link: lscript.ld does not KEEP() the vector table, so no
# --gc-sections here (unlike ZZ9000OS).
LDFLAGS = $(CPU_FLAGS)
LDFLAGS += -Wl,-build-id=none
LDFLAGS += -specs=$(SRC_DIR)/Xilinx.spec
LDFLAGS += -Wl,-T -Wl,$(SRC_DIR)/lscript.ld
LDFLAGS += -L$(BSP_LIB)

LIBS = -Wl,--start-group,-lxil,-lxilffs,-lgcc,-lc,--end-group

# qspi/nand/nor/rsa self-gate on XPAR_*/RSA_SUPPORT and compile empty
# with this BSP (SD-only boot); listed for fidelity to the stock FSBL.
C_SRCS = \
	$(SRC_DIR)/main.c \
	$(SRC_DIR)/image_mover.c \
	$(SRC_DIR)/pcap.c \
	$(SRC_DIR)/fsbl_hooks.c \
	$(SRC_DIR)/md5.c \
	$(SRC_DIR)/sd.c \
	$(SRC_DIR)/qspi.c \
	$(SRC_DIR)/nand.c \
	$(SRC_DIR)/nor.c \
	$(SRC_DIR)/rsa.c \
	$(SRC_DIR)/ps7_init_gpl.c \
	$(SRC_DIR)/newlib_compat.c

S_SRCS = $(SRC_DIR)/fsbl_handoff.S

OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))
OBJS += $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(S_SRCS))

TARGET = $(BUILD_DIR)/ZZ9000FSBL.elf

.PHONY: all clean size

all: $(TARGET) size

$(TARGET): $(OBJS)
	@echo 'Linking: $@'
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@echo 'Done.'

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -o $@ $<

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CPU_FLAGS) -c -o $@ $<

size: $(TARGET)
	$(SIZE) $(TARGET)

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJS:.o=.d)
