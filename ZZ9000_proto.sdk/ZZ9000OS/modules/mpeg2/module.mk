# MPEG-1/MPEG-2 Program Stream and Matroska decoder module.
# Decoder implementation is deliberately absent from BOOT's object list.
ZZM_MPEG2_DIR := modules/mpeg2
ZZM_MPEG2_BUILD := $(BUILD_DIR)/modules/mpeg2
ZZM_MPEG2_ELF := $(ZZM_MPEG2_BUILD)/MPEG2.elf
ZZM_MPEG2_IMAGE := $(BUILD_DIR)/modules/MPEG2.ZZM
ZZM_MPEG2_OBJS := \
	$(ZZM_MPEG2_BUILD)/module.o \
	$(ZZM_MPEG2_BUILD)/sdk_video_mpeg2.o \
	$(ZZM_MPEG2_BUILD)/sdk_mpeg2_neon.o \
	$(ZZM_MPEG2_BUILD)/sdk_dvd_ps.o \
	$(ZZM_MPEG2_BUILD)/sdk_dvd_lpcm.o \
	$(ZZM_MPEG2_BUILD)/sdk_dvd_ac3.o \
	$(ZZM_MPEG2_BUILD)/webm_parse.o \
	$(ZZM_MPEG2_BUILD)/webm_window.o \
	$(ZZM_MPEG2_BUILD)/sdk_media_timeline.o \
	$(ZZM_MPEG2_BUILD)/decode_mp3.o
ZZM_MPEG2_LINKER := ../../util/zzm_module.ld
ZZM_MPEG2_CFLAGS := $(CPU_FLAGS) $(COMMON_FLAGS) -O3 \
	-DSDK_VIDEO_MPEG2_MODULE -I$(ZZM_MPEG2_DIR) \
	-I$(LIBMPEG2_INCLUDE_DIR) -I$(A52_INCLUDE_DIR)
ZZM_MPEG2_LIBS := -Wl,--start-group $(LIBMPEG2_LIB) $(A52_LIB) -lm -lc -lgcc -Wl,--end-group

$(ZZM_MPEG2_BUILD)/module.o: $(ZZM_MPEG2_DIR)/module.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_MPEG2_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_MPEG2_BUILD)/sdk_video_mpeg2.o: $(SRC_DIR)/sdk_video_mpeg2.c $(LIBMPEG2_LIB) $(A52_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_MPEG2_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_MPEG2_BUILD)/sdk_mpeg2_neon.o: $(SRC_DIR)/sdk_mpeg2_neon.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_MPEG2_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_MPEG2_BUILD)/sdk_dvd_ps.o: $(SRC_DIR)/sdk_dvd_ps.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_MPEG2_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_MPEG2_BUILD)/sdk_dvd_lpcm.o: $(SRC_DIR)/sdk_dvd_lpcm.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_MPEG2_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_MPEG2_BUILD)/sdk_dvd_ac3.o: $(SRC_DIR)/sdk_dvd_ac3.c $(A52_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_MPEG2_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_MPEG2_BUILD)/webm_parse.o: modules/webm/webm_parse.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_MPEG2_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_MPEG2_BUILD)/webm_window.o: modules/webm/webm_window.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_MPEG2_CFLAGS) -MMD -MP -o $@ $<


$(ZZM_MPEG2_BUILD)/sdk_media_timeline.o: $(SRC_DIR)/sdk_media_timeline.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_MPEG2_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_MPEG2_BUILD)/decode_mp3.o: $(SRC_DIR)/mp3/decode_mp3.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_MPEG2_CFLAGS) -MMD -MP -o $@ $<


$(ZZM_MPEG2_ELF): $(ZZM_MPEG2_OBJS) $(LIBMPEG2_LIB) $(A52_LIB) $(ZZM_MPEG2_LINKER) $(ZZM_RT_LIB)
	@mkdir -p $(dir $@)
	$(call zzm_module_link,$(ZZM_MPEG2_OBJS),$(ZZM_MPEG2_LIBS),$(ZZM_MPEG2_LINKER),0x09000000,0x00200000,$@)
	$(ZZM_CONFORMANCE) $@ 1 MPEG2 0x09000000 0x00200000

$(ZZM_MPEG2_IMAGE): $(ZZM_MPEG2_ELF) $(ZZM_PACKER)
	@mkdir -p $(dir $@)
	$(ZZM_PACKER) 1 1 $< $@

modules: $(ZZM_MPEG2_IMAGE)
