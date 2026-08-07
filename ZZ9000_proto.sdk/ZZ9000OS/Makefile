# ZZ9000OS Standalone Makefile
#
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Replaces Xilinx SDK auto-generated build system
#
# Requires: ARM GNU Toolchain with newlib (NOT picolibc)
#
# macOS:  brew install --cask gcc-arm-embedded
# Linux:  Download the official toolchain from:
#         https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
#         (The Debian gcc-arm-none-eabi package uses picolibc, which is
#         incompatible with the Xilinx BSP that depends on newlib internals
#         like _impure_ptr and stdout.)
#
# Tested with: arm-none-eabi-gcc 12.2, 14.2, 15.2 (Arm GNU Toolchain)

CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size
CC_FOR_BUILD ?= cc

CPU_FLAGS = -mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard

# Paths
SRC_DIR = src
BSP_DIR = ../zz9000_bsp_new/ps7_cortexa9_0
BSP_INC = $(BSP_DIR)/include
BSP_LIB = $(BSP_DIR)/lib
BUILD_DIR = build
LIBJPEG_TURBO_VERSION ?= 3.1.4.1
LIBJPEG_TURBO_DIR ?= $(BUILD_DIR)/deps/libjpeg-turbo/$(LIBJPEG_TURBO_VERSION)
LIBJPEG_TURBO_SRC ?= $(LIBJPEG_TURBO_DIR)/src
LIBJPEG_TURBO_BUILD ?= $(LIBJPEG_TURBO_DIR)/build-simd
LIBJPEG_TURBO_LIB ?= $(LIBJPEG_TURBO_BUILD)/libjpeg.a
ZLIB_VERSION ?= 1.3.2
ZLIB_DIR ?= $(BUILD_DIR)/deps/zlib/$(ZLIB_VERSION)
ZLIB_SRC ?= $(ZLIB_DIR)/src
ZLIB_BUILD ?= $(ZLIB_DIR)/build
ZLIB_LIB ?= $(ZLIB_BUILD)/libz.a
LIBPNG_VERSION ?= 1.6.58
LIBPNG_DIR ?= $(BUILD_DIR)/deps/libpng/$(LIBPNG_VERSION)
LIBPNG_SRC ?= $(LIBPNG_DIR)/src
LIBPNG_BUILD ?= $(LIBPNG_DIR)/build-neon
LIBPNG_LIB ?= $(LIBPNG_BUILD)/liblibpng16_static.a
LZMA_SDK_VERSION ?= 26.01
LZMA_SDK_DIR ?= $(BUILD_DIR)/deps/lzma-sdk/$(LZMA_SDK_VERSION)
LZMA_SDK_SRC ?= $(LZMA_SDK_DIR)/src
LZMA_SDK_BUILD ?= $(LZMA_SDK_DIR)/build

# Include paths
INCLUDES = -I$(SRC_DIR) -I$(SRC_DIR)/bearssl -I$(BSP_INC)
INCLUDES += -I$(LIBJPEG_TURBO_BUILD) -I$(LIBJPEG_TURBO_SRC)/src
INCLUDES += -I$(ZLIB_SRC)
INCLUDES += -I$(LIBPNG_BUILD) -I$(LIBPNG_SRC)
INCLUDES += -I$(LZMA_SDK_SRC)/C

# Common flags (shared by all optimization levels)
COMMON_FLAGS = $(INCLUDES) -Wall -c
COMMON_FLAGS += -ffunction-sections -fdata-sections
COMMON_FLAGS += -fno-unwind-tables -fno-asynchronous-unwind-tables
COMMON_FLAGS += -fno-strict-aliasing
COMMON_FLAGS += -Wno-incompatible-pointer-types -Wno-return-mismatch -Wno-implicit-function-declaration
COMMON_FLAGS += -DCONFIG_EHCI_IS_TDI
# TXFIFO threshold tuning: raises the EHCI TX FIFO fill threshold
# from 2 to 4 DWORDs before a transaction is launched. Compensates
# for DDR / AXI pipeline latency on platforms where the RTG engine
# contests the AXI bus — the EHCI controller would otherwise drain
# its FIFO faster than DDR can refill it, producing Data Buffer
# Errors (status=0x20) on HS bulk OUT.
COMMON_FLAGS += -DCONFIG_USB_EHCI_TXFIFO_THRESH=4

# -O2 flags (default for all files)
EXTRA_CFLAGS ?=
CFLAGS = $(CPU_FLAGS) $(COMMON_FLAGS) -O2 $(EXTRA_CFLAGS)

# Linker flags
LDFLAGS = $(CPU_FLAGS)
LDFLAGS += -Wl,-build-id=none
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -specs=$(SRC_DIR)/Xilinx.spec
LDFLAGS += -Wl,-T -Wl,$(SRC_DIR)/lscript.ld
LDFLAGS += -L$(BSP_LIB)

# Libraries (order matters for --start-group)
LIBS = $(LIBPNG_LIB) $(ZLIB_LIB) $(LIBJPEG_TURBO_LIB) -lm -Wl,--start-group,-lxil,-lxilffs,-lgcc,-lc,--end-group

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
	$(SRC_DIR)/audio_capture.c \
	$(SRC_DIR)/bootrom.c \
	$(SRC_DIR)/codec37.c \
	$(SRC_DIR)/codec47.c \
	$(SRC_DIR)/compression.c \
	$(SRC_DIR)/core2.c \
	$(SRC_DIR)/scheduler.c \
	$(SRC_DIR)/scheduler_arm.c \
	$(SRC_DIR)/scheduler_stress.c \
	$(SRC_DIR)/sdk_smp_lock.c \
	$(SRC_DIR)/sdk_smp_lock_arm.c \
	$(SRC_DIR)/sdk_decode_reclaim.c \
	$(SRC_DIR)/sdk_decode_reclaim_arm.c \
	$(SRC_DIR)/dma_acc.c \
	$(SRC_DIR)/dma_rtg.c \
	$(SRC_DIR)/surface_allocator.c \
	$(SRC_DIR)/sdk_palette.c \
	$(SRC_DIR)/overlay.c \
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
	$(SRC_DIR)/sd_activity_led.c \
	$(SRC_DIR)/sd_storage.c \
	$(SRC_DIR)/sd_boot.c \
	$(SRC_DIR)/zz_config.c \
	$(SRC_DIR)/fw_update.c \
	$(SRC_DIR)/usb_proxy.c \
	$(SRC_DIR)/sdk_compression.c \
	$(SRC_DIR)/lzh/extract.c \
	$(SRC_DIR)/lzh/huf.c \
	$(SRC_DIR)/lzh/dhuf.c \
	$(SRC_DIR)/lzh/shuf.c \
	$(SRC_DIR)/lzh/slide.c \
	$(SRC_DIR)/lzh/larc.c \
	$(SRC_DIR)/lzh/maketbl.c \
	$(SRC_DIR)/lzh/maketree.c \
	$(SRC_DIR)/lzh/zz9k_lzh_bitio.c \
	$(SRC_DIR)/lzh/zz9k_lzh_crcio.c \
	$(SRC_DIR)/lzh/zz9k_lzh_support.c \
	$(SRC_DIR)/lzh/zz9k_lzh_reclaim_arm.c \
	$(SRC_DIR)/lzh/sdk_decompress_lzh.c \
	$(SRC_DIR)/sdk_crypto.c \
	$(SRC_DIR)/bearssl/ec_c25519_m31.c \
	$(SRC_DIR)/bearssl/ec_p256_m31.c \
	$(SRC_DIR)/bearssl/ec_curve25519.c \
	$(SRC_DIR)/bearssl/ccopy.c \
	$(SRC_DIR)/bearssl/i31_add.c \
	$(SRC_DIR)/bearssl/i31_bitlen.c \
	$(SRC_DIR)/bearssl/i31_decmod.c \
	$(SRC_DIR)/bearssl/i31_decode.c \
	$(SRC_DIR)/bearssl/i31_decred.c \
	$(SRC_DIR)/bearssl/i31_encode.c \
	$(SRC_DIR)/bearssl/i31_fmont.c \
	$(SRC_DIR)/bearssl/i31_iszero.c \
	$(SRC_DIR)/bearssl/i31_moddiv.c \
	$(SRC_DIR)/bearssl/i31_modpow.c \
	$(SRC_DIR)/bearssl/i31_modpow2.c \
	$(SRC_DIR)/bearssl/i31_montmul.c \
	$(SRC_DIR)/bearssl/i31_mulacc.c \
	$(SRC_DIR)/bearssl/i31_muladd.c \
	$(SRC_DIR)/bearssl/i31_ninv31.c \
	$(SRC_DIR)/bearssl/i31_reduce.c \
	$(SRC_DIR)/bearssl/i31_rshift.c \
	$(SRC_DIR)/bearssl/i31_sub.c \
	$(SRC_DIR)/bearssl/i31_tmont.c \
	$(SRC_DIR)/bearssl/i32_div32.c \
	$(SRC_DIR)/bearssl/rsa_i31_pkcs1_vrfy.c \
	$(SRC_DIR)/bearssl/rsa_i31_pub.c \
	$(SRC_DIR)/bearssl/rsa_pkcs1_sig_unpad.c \
	$(SRC_DIR)/bearssl/ecdsa_i31_bits.c \
	$(SRC_DIR)/bearssl/ecdsa_i31_vrfy_raw.c \
	$(SRC_DIR)/bearssl/ec_secp256r1.c \
	$(SRC_DIR)/bearssl/ec_secp384r1.c \
	$(SRC_DIR)/bearssl/ec_secp521r1.c \
	$(SRC_DIR)/bearssl/aes_ct.c \
	$(SRC_DIR)/bearssl/aes_ct_enc.c \
	$(SRC_DIR)/bearssl/aes_ct_ctr.c \
	$(SRC_DIR)/bearssl/ghash_ctmul.c \
	$(SRC_DIR)/bearssl/gcm.c \
	$(SRC_DIR)/sdk_image_stream.c \
	$(SRC_DIR)/sdk_video_stream.c \
	$(SRC_DIR)/sdk_media_profile.c \
	$(SRC_DIR)/sdk_media_session.c \
	$(SRC_DIR)/sdk_media_timeline.c \
	$(SRC_DIR)/sdk_video_plmpeg.c \
	$(SRC_DIR)/sdk_video_yuy2.c \
	$(SRC_DIR)/sdk_jpeg.c \
	$(SRC_DIR)/jmem_zz9k.c \
	$(SRC_DIR)/sdk_surface.c \
	$(SRC_DIR)/sdk_mailbox.c \
	$(SRC_DIR)/watchdog.c \
	$(SRC_DIR)/newlib_compat.c

# Object files shared by both bitstream-capability firmware images.
COMMON_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))
COMMON_OBJS += $(LZMA_SDK_BUILD)/LzmaDec.o
COMMON_OBJS += $(LZMA_SDK_BUILD)/Lzma2Dec.o
BOOTROM_TOOL = $(BUILD_DIR)/make_bootrom_image
BOOTROM_IMAGE_BIN = $(BUILD_DIR)/bootrom_image.bin
BOOTROM_IMAGE_OBJ = $(BUILD_DIR)/bootrom_image.o
COMMON_OBJS += $(BOOTROM_IMAGE_OBJ)
OVERLAY_HW_OBJ = $(BUILD_DIR)/overlay_hw.o
OVERLAY_HW_FALLBACK_OBJ = $(BUILD_DIR)/overlay_hw_fallback.o
OBJS = $(COMMON_OBJS) $(OVERLAY_HW_OBJ)
FALLBACK_OBJS = $(COMMON_OBJS) $(OVERLAY_HW_FALLBACK_OBJ)

# Output
TARGET = $(BUILD_DIR)/ZZ9000OS.elf
FALLBACK_TARGET = $(BUILD_DIR)/ZZ9000OS-legacy-bitstream.elf

.PHONY: all clean size FORCE

all: $(TARGET) $(FALLBACK_TARGET) size

$(TARGET): $(LIBJPEG_TURBO_LIB) $(LIBPNG_LIB) $(OBJS)
	@echo 'Linking: $@'
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@echo 'Done.'

$(FALLBACK_TARGET): $(LIBJPEG_TURBO_LIB) $(LIBPNG_LIB) $(FALLBACK_OBJS)
	@echo 'Linking: $@'
	$(CC) $(LDFLAGS) -o $@ $(FALLBACK_OBJS) $(LIBS)
	@echo 'Done.'

$(LIBJPEG_TURBO_LIB):
	@echo 'Building libjpeg-turbo dependency...'
	cd ../.. && bash ./build_libjpeg_turbo.sh

$(ZLIB_LIB):
	@echo 'Building zlib dependency...'
	cd ../.. && bash ./build_zlib.sh

$(LIBPNG_LIB): $(ZLIB_LIB)
	@echo 'Building libpng dependency...'
	cd ../.. && bash ./build_libpng.sh

$(LZMA_SDK_SRC)/C/LzmaDec.c:
	@echo 'Fetching LZMA SDK dependency...'
	cd ../.. && bash ./build_lzma_sdk.sh

$(LZMA_SDK_SRC)/C/Lzma2Dec.c: $(LZMA_SDK_SRC)/C/LzmaDec.c

$(LZMA_SDK_BUILD)/LzmaDec.o: $(LZMA_SDK_SRC)/C/LzmaDec.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -o $@ $<

$(LZMA_SDK_BUILD)/Lzma2Dec.o: $(LZMA_SDK_SRC)/C/Lzma2Dec.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -o $@ $<

# --- Per-file rules ---

# The committed non-default bitstreams do not yet contain the second VDMA or
# formatter overlay logic.  Build a matched firmware image whose backend is
# compile-time disabled, so release packaging can select software composition
# without ever touching the unmapped AXI-Lite window.
$(OVERLAY_HW_FALLBACK_OBJ): $(SRC_DIR)/overlay_hw.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -UZZ_OVERLAY_HW_PRESENT -DZZ_OVERLAY_HW_PRESENT=0 \
		-MMD -MP -o $@ $<

$(BOOTROM_TOOL): ../../util/make_bootrom_image.c $(SRC_DIR)/bootrom_layout.h $(SRC_DIR)/bootrom.h $(SRC_DIR)/diag-code.h $(SRC_DIR)/zzsd-device.h
	@mkdir -p $(dir $@)
	$(CC_FOR_BUILD) -Wall -Wextra -Werror -std=c99 -o $@ $<

$(BOOTROM_IMAGE_BIN): $(BOOTROM_TOOL) FORCE
	$< $@

$(BOOTROM_IMAGE_OBJ): $(BOOTROM_IMAGE_BIN)
	$(OBJCOPY) -I binary -O elf32-littlearm -B arm \
		--rename-section .data=.bootrom_image,alloc,load,readonly,data,contents \
		$< $@

# ulpi.c: GCC 15 null-pointer offset UB workaround (-O0 for this file only)
$(BUILD_DIR)/usb/ulpi.o: $(SRC_DIR)/usb/ulpi.c
	@mkdir -p $(dir $@)
	$(CC) $(CPU_FLAGS) $(COMMON_FLAGS) -O0 -MMD -MP -o $@ $<

# sdk_compression.c: needs the vendored LZH decode core headers (src/lzh)
# for the upcoming LZH backend (decode_lzhuf() et al).
$(BUILD_DIR)/sdk_compression.o: $(SRC_DIR)/sdk_compression.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(SRC_DIR)/lzh -MMD -MP -o $@ $<

# Video hot path: MPEG decode and per-pixel composition benefit materially
# from the more aggressive loop/inlining passes; keep the rest at -O2.
$(BUILD_DIR)/gfx.o: $(SRC_DIR)/gfx.c
	@mkdir -p $(dir $@)
	$(CC) $(CPU_FLAGS) $(COMMON_FLAGS) -O3 -MMD -MP -o $@ $<

$(BUILD_DIR)/sdk_video_plmpeg.o: $(SRC_DIR)/sdk_video_plmpeg.c
	@mkdir -p $(dir $@)
	$(CC) $(CPU_FLAGS) $(COMMON_FLAGS) -O3 -MMD -MP -o $@ $<

# Vendored LZH decode core (LHa for UNIX) + firmware I/O shim: every TU here
# includes lha.h, which does `#include <config.h>` (angle-bracket). Two flags
# are mandatory and absent from the default rule:
#   -DHAVE_CONFIG_H  : without it lha.h skips config.h entirely and falls back
#                      to <varargs.h> (a hard #error on newlib) plus fallback
#                      uid_t/gid_t/ssize_t typedefs that collide with newlib.
#   -I$(SRC_DIR)/lzh : resolves <config.h> to src/lzh/config.h. It MUST precede
#                      $(CFLAGS) because -Isrc/bearssl is already on that path
#                      and also holds a config.h — first match wins, so lzh must
#                      come first or bearssl's config would silently be used.
# Mirrors the SDK's proven `-DHAVE_CONFIG_H=1 -Itools/lha-unix` for the same core.
$(BUILD_DIR)/lzh/%.o: $(SRC_DIR)/lzh/%.c
	@mkdir -p $(dir $@)
	$(CC) -I$(SRC_DIR)/lzh -DHAVE_CONFIG_H $(CFLAGS) -MMD -MP -o $@ $<

# Default rule for all other files (-O2).
# -MMD emits a .d file next to the .o listing every header the source
# pulled in; we -include them below so header changes force a rebuild.
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -o $@ $<

size: $(TARGET)
	$(SIZE) $(TARGET)

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJS:.o=.d) $(OVERLAY_HW_FALLBACK_OBJ:.o=.d)
