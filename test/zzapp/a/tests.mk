# The process-record layer (zzapp_rec.c over zzapp_as.c, mmu_page.c) and the
# card pool that host.c, the loader and the arena sit on. Compiled once for
# every suite that links the runtime objects.
A_REC_FLAGS := -DMMU_PAGE_HOST_TEST -DSMP_LOCK_HOST_TEST -I../mmu/mock -I../card_pool
A_REC_OBJS := $(BUILD)/mock_client.o $(BUILD)/a/zzapp_rec.o $(BUILD)/a/zzapp_xport.o $(BUILD)/a/zzapp_as.o \
	$(BUILD)/a/mmu_page.o $(BUILD)/a/host_rec_glue.o $(BUILD)/a/host_decode_stubs.o
A_POOL_OBJS := $(BUILD)/a/card_pool.o $(BUILD)/a/sdk_smp_lock.o
A_RUNTIME_OBJS := $(BUILD)/a/zzapp_loader.o $(BUILD)/a/zzapp_host.o \
	$(BUILD)/a/zzapp_api.o $(BUILD)/a/zzapp_kernel.o $(A_REC_OBJS) $(A_POOL_OBJS)
ZZAPP_TESTS += $(BUILD)/a/loader_smoke $(BUILD)/a/runtime_host_test \
	$(BUILD)/a/loader_elf_test $(BUILD)/a/display_fallback_test \
	$(BUILD)/a/core_api_test $(BUILD)/a/map_unmap_test \
	$(BUILD)/a/svc_return_test $(BUILD)/a/page_flags_test \
	$(BUILD)/a/msg_wait_stall_test $(BUILD)/a/retire_test \
	$(BUILD)/a/loader_dynamic_test $(BUILD)/a/sleep_time_test \
	$(BUILD)/a/sleep_stop_test

$(BUILD)/a/zzapp_rec.o: $(FW)/zzapp_rec.c $(FW)/zzapp_rec.h $(FW)/zzapp_as.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) $(A_REC_FLAGS) -c -o $@ $<
$(BUILD)/a/zzapp_xport.o: $(FW)/zzapp_xport.c $(FW)/zzapp_xport.h $(FW)/zzapp_rec.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) $(A_REC_FLAGS) -c -o $@ $<
$(BUILD)/a/zzapp_as.o: $(FW)/zzapp_as.c $(FW)/zzapp_as.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) $(A_REC_FLAGS) -c -o $@ $<
$(BUILD)/a/mmu_page.o: $(FW)/mmu_page.c $(FW)/mmu_page.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) $(A_REC_FLAGS) -c -o $@ $<
$(BUILD)/a/host_rec_glue.o: host_rec_glue.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) $(A_REC_FLAGS) -c -o $@ $<
$(BUILD)/a/host_decode_stubs.o: host_decode_stubs.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) $(A_REC_FLAGS) -c -o $@ $<
$(BUILD)/a/card_pool.o: $(FW)/card_pool.c $(FW)/card_pool.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) $(A_REC_FLAGS) -c -o $@ $<
$(BUILD)/a/sdk_smp_lock.o: $(FW)/sdk_smp_lock.c $(FW)/sdk_smp_lock.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) $(A_REC_FLAGS) -c -o $@ $<

$(BUILD)/a/zzapp_loader.o: $(FW)/zzapp_loader.c $(FW)/zzapp_runtime.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -c -o $@ $<
$(BUILD)/a/zzapp_loader_incremental.o: $(FW)/zzapp_loader.c $(FW)/zzapp_runtime.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_LOADER_INCREMENTAL_HOST_TEST -c -o $@ $<
$(BUILD)/a/zzapp_loader_real_as_incremental.o: $(FW)/zzapp_loader.c $(FW)/zzapp_runtime.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_LOADER_INCREMENTAL_HOST_TEST \
		-DZZAPP_LOADER_REAL_AS_HOST_TEST -c -o $@ $<
$(BUILD)/a/zzapp_host.o: $(FW)/zzapp_host.c $(FW)/zzapp_runtime.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -c -o $@ $<
$(BUILD)/a/zzapp_api.o: $(FW)/zzapp_api.c $(FW)/zzapp_runtime.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -c -o $@ $<
$(BUILD)/a/zzapp_kernel.o: $(FW)/zzapp_kernel.c $(FW)/zzapp_kernel.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -c -o $@ $<
$(BUILD)/a/zzapp_services.o: $(FW)/zzapp_services.c $(FW)/zzapp_services.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -c -o $@ $<
$(BUILD)/a/loader_smoke: a/loader_smoke.c $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $< $(A_RUNTIME_OBJS)
$(BUILD)/a/runtime_host_test: a/runtime_host_test.c $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $< $(A_RUNTIME_OBJS)
# Suites that need the real card pool and commit ledger: the loader's staging
# block is a class APP pool block, as on the card.
LOADER_INCREMENTAL_HOST_OBJS := $(BUILD)/a/zzapp_loader_incremental.o \
	$(BUILD)/a/zzapp_host.o $(BUILD)/a/zzapp_api.o $(BUILD)/a/zzapp_kernel.o \
	$(A_REC_OBJS)
LOADER_INCREMENTAL_HOST_SOURCES := $(FW)/zzapp_mem.c $(FW)/card_pool.c \
	$(FW)/sdk_smp_lock.c ../card_pool/host_card_pool.c
LOADER_REAL_AS_HOST_OBJS := $(BUILD)/a/zzapp_loader_real_as_incremental.o \
	$(BUILD)/a/zzapp_host.o $(BUILD)/a/zzapp_api.o $(BUILD)/a/zzapp_kernel.o \
	$(BUILD)/mock_client.o $(BUILD)/a/zzapp_rec.o $(BUILD)/a/zzapp_xport.o $(BUILD)/a/host_decode_stubs.o
LOADER_REAL_AS_HOST_SOURCES := $(FW)/zzapp_mem.c $(FW)/card_pool.c \
	$(FW)/sdk_smp_lock.c $(FW)/zzapp_as.c $(FW)/mmu_page.c \
	../card_pool/host_card_pool.c
$(BUILD)/a/retire_test: a/retire_test.c $(LOADER_REAL_AS_HOST_OBJS) $(LOADER_REAL_AS_HOST_SOURCES) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DSMP_LOCK_HOST_TEST -DMMU_PAGE_HOST_TEST \
		-I../card_pool -I../mmu/mock -o $@ $< \
		$(LOADER_REAL_AS_HOST_OBJS) $(LOADER_REAL_AS_HOST_SOURCES)
$(BUILD)/a/loader_dynamic_test: a/loader_dynamic_test.c $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $< $(A_RUNTIME_OBJS)
$(BUILD)/a/loader_elf_test: a/loader_elf_test.c $(LOADER_REAL_AS_HOST_OBJS) $(LOADER_REAL_AS_HOST_SOURCES) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DSMP_LOCK_HOST_TEST -DMMU_PAGE_HOST_TEST \
		-I../card_pool -I../mmu/mock -o $@ $< \
		$(LOADER_REAL_AS_HOST_OBJS) $(LOADER_REAL_AS_HOST_SOURCES)
$(BUILD)/a/display_fallback_test: a/display_fallback_test.c $(A_RUNTIME_OBJS) $(BUILD)/a/zzapp_services.o | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $< $(A_RUNTIME_OBJS) $(BUILD)/a/zzapp_services.o
$(BUILD)/a/msg_wait_stall_test: a/msg_wait_stall_test.c $(A_RUNTIME_OBJS) $(BUILD)/a/zzapp_services.o | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $< $(A_RUNTIME_OBJS) $(BUILD)/a/zzapp_services.o

$(BUILD)/a/core_api_test: a/core_api_test.c $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $< $(A_RUNTIME_OBJS)

$(BUILD)/a/map_unmap_test: a/map_unmap_test.c $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $< $(A_RUNTIME_OBJS)

$(BUILD)/a/svc_return_test: a/svc_return_test.c $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $< $(A_RUNTIME_OBJS)

$(BUILD)/a/page_flags_test: a/page_flags_test.c $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $< $(A_RUNTIME_OBJS)

# Opt-in: `make test ZZAPP_PROBE_ELF=<app.elf>` feeds a real app ELF through the
# loader in 64 KiB chunks. Not named *_test.c so auto-discovery skips it.
ifneq ($(strip $(ZZAPP_PROBE_ELF)),)
ZZAPP_TESTS += $(BUILD)/a/loader_file_check
$(BUILD)/a/loader_file_check: a/loader_file_check.c $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_PROBE_ELF=\"$(ZZAPP_PROBE_ELF)\" -o $@ $< $(A_RUNTIME_OBJS)
endif

$(BUILD)/a/aar_api_test: a/aar_api_test.c $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $< $(A_RUNTIME_OBJS)

$(BUILD)/a/sleep_stop_test: a/sleep_stop_test.c $(A_RUNTIME_OBJS) $(BUILD)/a/zzapp_services.o | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $< $(A_RUNTIME_OBJS) $(BUILD)/a/zzapp_services.o

$(BUILD)/a/sleep_time_test: a/sleep_time_test.c $(A_RUNTIME_OBJS) $(BUILD)/a/zzapp_services.o | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $< $(A_RUNTIME_OBJS) $(BUILD)/a/zzapp_services.o
