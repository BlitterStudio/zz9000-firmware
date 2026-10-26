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
ZZM_WEBM_LIBS := $(LIBVPX_LIB) $(LIBOPUS_LIB) $(LIBVORBIS_LIB) -lm \
	-Wl,--start-group,-lgcc,-lc,--end-group
ZZM_WEBM_LINKER := ../../util/zzm_module.ld
ZZM_WEBM_ALLOC_CHECK := ../../util/check_zzm_allocators.sh
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

$(ZZM_WEBM_ELF): $(TARGET) $(ZZM_WEBM_OBJS) $(LIBVPX_LIB) $(LIBOPUS_LIB) \
	$(LIBVORBIS_LIB) $(ZZM_WEBM_LINKER) $(ZZM_WEBM_ALLOC_CHECK)
	@mkdir -p $(dir $@)
	$(CC) $(CPU_FLAGS) -nostdlib -Wl,-build-id=none -Wl,--gc-sections \
		-Wl,-T,$(ZZM_WEBM_LINKER) -Wl,--defsym,ZZM_SLOT=0x09400000 \
		-Wl,--defsym,ZZM_SLOT_SIZE=0x00800000 -Wl,--just-symbols,$(TARGET) \
		-Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc \
		-Wl,--wrap=realloc -Wl,--wrap=memalign -Wl,--wrap=posix_memalign \
		-Wl,--wrap=aligned_alloc -Wl,--undefined=__wrap_malloc \
		-Wl,--undefined=__wrap_free -Wl,--undefined=__wrap_calloc \
		-Wl,--undefined=__wrap_realloc -Wl,--undefined=__wrap_memalign \
		-Wl,--undefined=__wrap_posix_memalign \
		-Wl,--undefined=__wrap_aligned_alloc -o $@ $(ZZM_WEBM_OBJS) $(ZZM_WEBM_LIBS)
	$(ZZM_WEBM_ALLOC_CHECK) --require-shims $@

$(ZZM_WEBM_IMAGE): $(ZZM_WEBM_ELF) $(ZZM_PACKER)
	@mkdir -p $(dir $@)
	$(ZZM_PACKER) 2 1 $< $@

modules: $(ZZM_WEBM_IMAGE)
