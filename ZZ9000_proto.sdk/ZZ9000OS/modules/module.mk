# Shared module runtime and link policy. Included before every module recipe.
ZZM_RT_DIR := modules/libzzm_rt
ZZM_RT_BUILD := $(BUILD_DIR)/modules/libzzm_rt
ZZM_RT_LIB := $(ZZM_RT_BUILD)/libzzm_rt.a
ZZM_CONFORMANCE := ../../util/check_zzm_conformance.sh
define zzm_module_link
$(CC) $(CPU_FLAGS) -nostdlib -Wl,-build-id=none -Wl,--gc-sections \
	-Wl,-T,$(3) -Wl,--defsym,ZZM_SLOT=$(4) -Wl,--defsym,ZZM_SLOT_SIZE=$(5) \
	-Wl,--undefined=malloc -Wl,--undefined=free -Wl,--undefined=calloc \
	-Wl,--undefined=realloc -Wl,--undefined=memalign \
	-Wl,--undefined=posix_memalign -Wl,--undefined=aligned_alloc \
	-Wl,--undefined=_malloc_r -Wl,--undefined=_free_r \
	-Wl,--undefined=_calloc_r -Wl,--undefined=_realloc_r \
	-Wl,--undefined=_memalign_r -Wl,--undefined=zzm_rt_identity \
	-Wl,--undefined=zzm_module_descriptor \
	-Wl,--whole-archive $(ZZM_RT_LIB) -Wl,--no-whole-archive -o $(6) \
	$(1) $(2)
endef

$(ZZM_RT_BUILD)/zzm_rt.o: $(ZZM_RT_DIR)/zzm_rt.c $(SRC_DIR)/zzm_runtime.h
	@mkdir -p $(dir $@)
	$(CC) $(CPU_FLAGS) $(COMMON_FLAGS) -O2 -I$(SRC_DIR) -MMD -MP -o $@ $<

$(ZZM_RT_BUILD)/zzm_divmod.o: $(ZZM_RT_DIR)/zzm_divmod.S
	@mkdir -p $(dir $@)
	$(CC) $(CPU_FLAGS) $(COMMON_FLAGS) -c -o $@ $<

$(ZZM_RT_LIB): $(ZZM_RT_BUILD)/zzm_rt.o $(ZZM_RT_BUILD)/zzm_divmod.o
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# C++ modules must add -fno-exceptions -fno-rtti to their own CXXFLAGS.
