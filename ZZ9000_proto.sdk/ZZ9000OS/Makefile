# ZZ9000OS Standalone Makefile
# Replaces Xilinx SDK auto-generated build system
# Requires: ARM GNU Toolchain (brew install --cask gcc-arm-embedded)

CC = arm-none-eabi-gcc
SIZE = arm-none-eabi-size

# CPU/FPU flags matching original Xilinx SDK config
CPU_FLAGS = -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard

# Paths
SRC_DIR = src
BSP_DIR = ../zz9000_bsp_new/ps7_cortexa9_0
BSP_INC = $(BSP_DIR)/include
BSP_LIB = $(BSP_DIR)/lib
BUILD_DIR = build

# Include paths
INCLUDES = -I$(SRC_DIR) -I$(BSP_INC)

# Compiler flags
CFLAGS = $(CPU_FLAGS) $(INCLUDES) -Wall -O2 -c
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -Wno-incompatible-pointer-types -Wno-return-mismatch -Wno-implicit-function-declaration

# Linker flags
LDFLAGS = $(CPU_FLAGS)
LDFLAGS += -Wl,-build-id=none
LDFLAGS += -specs=$(SRC_DIR)/Xilinx.spec
LDFLAGS += -Wl,-T -Wl,$(SRC_DIR)/lscript.ld
LDFLAGS += -L$(BSP_LIB)

# Libraries (order matters for --start-group)
LIBS = -lm -Wl,--start-group,-lxil,-lgcc,-lc,--end-group

# Source files
C_SRCS = \
	$(SRC_DIR)/main.c \
	$(SRC_DIR)/gfx.c \
	$(SRC_DIR)/video.c \
	$(SRC_DIR)/ethernet.c \
	$(SRC_DIR)/hdmi.c \
	$(SRC_DIR)/interrupt.c \
	$(SRC_DIR)/platform.c \
	$(SRC_DIR)/adc.c \
	$(SRC_DIR)/ax.c \
	$(SRC_DIR)/bootrom.c \
	$(SRC_DIR)/codec37.c \
	$(SRC_DIR)/codec47.c \
	$(SRC_DIR)/compression.c \
	$(SRC_DIR)/core2.c \
	$(SRC_DIR)/dma_acc.c \
	$(SRC_DIR)/dma_rtg.c \
	$(SRC_DIR)/usb.c \
	$(SRC_DIR)/mp3/decode_mp3.c \
	$(SRC_DIR)/compression/audio/adpcm.c \
	$(SRC_DIR)/usb/ehci-hcd.c \
	$(SRC_DIR)/usb/ehci-zynq.c \
	$(SRC_DIR)/usb/ulpi-viewport.c \
	$(SRC_DIR)/usb/ulpi.c \
	$(SRC_DIR)/usb/usb_hub.c \
	$(SRC_DIR)/usb/usb_storage.c \
	$(SRC_DIR)/usb/usb.c \
	$(SRC_DIR)/newlib_compat.c

# Object files (in build dir, preserving subdirectory structure)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))

# Output
TARGET = $(BUILD_DIR)/ZZ9000OS.elf

.PHONY: all clean size

all: $(TARGET) size

$(TARGET): $(OBJS)
	@echo 'Linking: $@'
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@echo 'Done.'

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

size: $(TARGET)
	$(SIZE) $(TARGET)

clean:
	rm -rf $(BUILD_DIR)
