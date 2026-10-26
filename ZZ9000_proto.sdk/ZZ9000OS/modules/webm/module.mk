# Included by the firmware Makefile after the boot ELF and zzm_pack are defined.
# WEBM decoder code is deliberately absent from the boot object list.
ZZM_WEBM_DIR := modules/webm
ZZM_WEBM_BUILD := $(BUILD_DIR)/modules/webm
ZZM_WEBM_ELF := $(ZZM_WEBM_BUILD)/WEBM.elf
ZZM_WEBM_IMAGE := $(BUILD_DIR)/modules/WEBM.ZZM
ZZM_WEBM_OBJS := \
	$(ZZM_WEBM_BUILD)/module.o \
	$(ZZM_WEBM_BUILD)/webm_module_alloc.o \
	$(ZZM_WEBM_BUILD)/sdk_video_webm.o \
	$(ZZM_WEBM_BUILD)/webm_parse.o \
	$(ZZM_WEBM_BUILD)/webm_window.o \
	$(ZZM_WEBM_BUILD)/sdk_vorbis_alloc.o \
	$(ZZM_WEBM_BUILD)/sdk_media_timeline.o
ZZM_WEBM_LIBS := -Wl,--just-symbols,$(TARGET) $(LIBVPX_LIB) $(LIBOPUS_LIB) $(LIBVORBIS_LIB) -lm
ZZM_WEBM_LINKER := ../../util/zzm_module.ld
ZZM_WEBM_CFLAGS := $(CPU_FLAGS) $(COMMON_FLAGS) -O3 \
	-DSDK_VIDEO_WEBM_MODULE -I$(ZZM_WEBM_DIR)

$(ZZM_WEBM_BUILD)/module.o: $(ZZM_WEBM_DIR)/module.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_WEBM_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_WEBM_BUILD)/webm_module_alloc.o: $(ZZM_WEBM_DIR)/webm_module_alloc.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_WEBM_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_WEBM_BUILD)/sdk_video_webm.o: $(ZZM_WEBM_DIR)/sdk_video_webm.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_WEBM_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_WEBM_BUILD)/webm_parse.o: $(ZZM_WEBM_DIR)/webm_parse.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_WEBM_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_WEBM_BUILD)/webm_window.o: $(ZZM_WEBM_DIR)/webm_window.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_WEBM_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_WEBM_BUILD)/sdk_vorbis_alloc.o: $(SRC_DIR)/sdk_vorbis_alloc.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_WEBM_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_WEBM_BUILD)/sdk_media_timeline.o: $(SRC_DIR)/sdk_media_timeline.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZM_WEBM_CFLAGS) -MMD -MP -o $@ $<

$(ZZM_WEBM_ELF): $(ZZM_WEBM_OBJS) $(LIBVPX_LIB) $(LIBOPUS_LIB) \
	$(LIBVORBIS_LIB) $(ZZM_WEBM_LINKER) $(ZZM_RT_LIB)
	@mkdir -p $(dir $@)
	$(call zzm_module_link,$(ZZM_WEBM_OBJS),$(ZZM_WEBM_LIBS),$(ZZM_WEBM_LINKER),0x09200000,0x00400000,$@)
	$(ZZM_CONFORMANCE) $@ 2 WEBM 0x09200000 0x00400000

$(ZZM_WEBM_IMAGE): $(ZZM_WEBM_ELF) $(ZZM_PACKER)
	@mkdir -p $(dir $@)
	$(ZZM_PACKER) 2 1 $< $@

modules: $(ZZM_WEBM_IMAGE)
