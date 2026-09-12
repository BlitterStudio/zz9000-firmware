CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
PYTHON ?= python3
ifeq ($(shell uname -s),Darwin)
MODE_SWITCH_LDFLAGS := -Wl,-dead_strip
else
MODE_SWITCH_LDFLAGS := -Wl,--gc-sections
endif

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
BSP_INCLUDE := ../../ZZ9000_proto.sdk/zz9000_bsp_new/ps7_cortexa9_0/include
TARGETS := \
	$(BUILD_DIR)/video_vdma_test \
	$(BUILD_DIR)/video_scale_test \
	$(BUILD_DIR)/videocap_control_test \
	$(BUILD_DIR)/video_mode_pll_test \
	$(BUILD_DIR)/video_mode_switch_test \
	$(BUILD_DIR)/overlay_color_test \
	$(BUILD_DIR)/overlay_schedule_test \
	$(BUILD_DIR)/overlay_hw_test

CPPFLAGS += -I$(SRC_DIR)

DEPS :=

.PHONY: test clean

test: $(TARGETS)
	$(BUILD_DIR)/video_vdma_test
	$(BUILD_DIR)/video_scale_test
	$(BUILD_DIR)/videocap_control_test
	$(BUILD_DIR)/video_mode_pll_test
	$(BUILD_DIR)/video_mode_switch_test
	$(BUILD_DIR)/overlay_color_test
	$(BUILD_DIR)/overlay_schedule_test
	$(BUILD_DIR)/overlay_hw_test
	$(PYTHON) rtl_capture_contract_test.py

$(BUILD_DIR)/video_vdma_test: video_vdma_test.c $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ video_vdma_test.c

$(BUILD_DIR)/video_scale_test: video_scale_test.c $(SRC_DIR)/video_scale.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ video_scale_test.c

$(BUILD_DIR)/videocap_control_test: videocap_control_test.c $(SRC_DIR)/video.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ videocap_control_test.c

$(BUILD_DIR)/video_mode_pll_test: video_mode_pll_test.c \
		$(SRC_DIR)/zz_video_modes.c $(SRC_DIR)/zz_video_modes.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ \
		video_mode_pll_test.c $(SRC_DIR)/zz_video_modes.c -lm

$(BUILD_DIR)/video_mode_switch_test: video_mode_switch_test.c \
		stubs/xil_io.h $(SRC_DIR)/video.c $(SRC_DIR)/video.h \
		$(SRC_DIR)/video_scale.h $(SRC_DIR)/video_vdma.h \
		$(SRC_DIR)/hdmi.c $(SRC_DIR)/hdmi.h \
		$(SRC_DIR)/zz_video_modes.c $(SRC_DIR)/zz_video_modes.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Istubs -I$(BSP_INCLUDE) \
		-DXIL_IO_H -include stubs/xil_io.h \
		-include xparameters.h -include xil_printf.h \
		-ffunction-sections -fdata-sections \
		-Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
		-o $@ video_mode_switch_test.c $(SRC_DIR)/hdmi.c \
		$(SRC_DIR)/zz_video_modes.c $(MODE_SWITCH_LDFLAGS)

$(BUILD_DIR)/overlay_color_test: overlay_color_test.c $(SRC_DIR)/overlay_color.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ overlay_color_test.c

$(BUILD_DIR)/overlay_schedule_test: overlay_schedule_test.c $(SRC_DIR)/overlay_schedule.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ overlay_schedule_test.c

$(BUILD_DIR)/overlay_hw_test: overlay_hw_test.c stubs/xil_io.h \
		$(SRC_DIR)/overlay_hw.c $(SRC_DIR)/overlay_hw.h \
		$(BSP_INCLUDE)/xaxivdma_hw.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -Istubs $(CPPFLAGS) -I$(BSP_INCLUDE) $(CFLAGS) \
		-o $@ overlay_hw_test.c $(SRC_DIR)/overlay_hw.c

clean:
	rm -rf $(BUILD_DIR)
