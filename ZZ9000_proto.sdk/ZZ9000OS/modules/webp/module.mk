# WebP decoder module. PNG and JPEG remain part of the boot image.
ZZM_WEBP_DIR := modules/webp
ZZM_WEBP_BUILD := $(BUILD_DIR)/modules/webp
ZZM_WEBP_ELF := $(ZZM_WEBP_BUILD)/WEBP.elf
ZZM_WEBP_IMAGE := $(BUILD_DIR)/modules/WEBP.ZZM
ZZM_WEBP_OBJS := \
	$(ZZM_WEBP_BUILD)/webp_module.o \
	$(ZZM_WEBP_BUILD)/sdk_webp_alloc.o
ZZM_WEBP_LINKER := ../../util/zzm_module.ld

$(ZZM_WEBP_BUILD)/webp_module.o: $(ZZM_WEBP_DIR)/webp_module.c $(LIBWEBP_DECODER_LIB) $(LIBWEBP_DEMUX_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -o $@ $<

$(ZZM_WEBP_BUILD)/sdk_webp_alloc.o: $(ZZM_WEBP_DIR)/sdk_webp_alloc.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -o $@ $<

$(ZZM_WEBP_ELF): $(ZZM_WEBP_OBJS) $(LIBWEBP_DECODER_LIB) $(LIBWEBP_DEMUX_LIB) $(ZZM_WEBP_LINKER)
	@mkdir -p $(dir $@)
	$(CC) $(CPU_FLAGS) -nostdlib -Wl,-build-id=none -Wl,--gc-sections \
		-Wl,-T,$(ZZM_WEBP_LINKER) -Wl,--defsym,ZZM_SLOT=0x09c00000 \
		-Wl,--defsym,ZZM_SLOT_SIZE=0x00400000 -Wl,--wrap=malloc \
		-Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc -o $@ \
		$(ZZM_WEBP_OBJS) -Wl,--start-group $(LIBWEBP_DEMUX_LIB) \
		$(LIBWEBP_DECODER_LIB) -lm -lc -lgcc -Wl,--end-group

$(ZZM_WEBP_IMAGE): $(ZZM_WEBP_ELF) $(ZZM_PACKER)
	@mkdir -p $(dir $@)
	$(ZZM_PACKER) 3 1 $< $@

modules: $(ZZM_WEBP_IMAGE)
