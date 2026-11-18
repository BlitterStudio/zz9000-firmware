B_DEFERRED_OBJS := $(BUILD)/b/zz9k_host.o $(BUILD)/b/zz9k_library.o
ZZAPP_TESTS += $(BUILD)/b/deferred_mailbox_test $(BUILD)/b/reset_lifecycle_test \
	$(BUILD)/b/teardown_test $(BUILD)/b/loader_zero_test \
	$(BUILD)/b/slot_selection_test $(BUILD)/b/host_visible_limit_test

# zz9k_host.c is also built by SDK CMake without -Werror; its Amiga-only
# payload-copy loop is intentionally unused on native host builds.
B_SDK_CPPFLAGS := -I../../sdk/include -I../../sdk/host/include -I../../sdk/amiga/include
B_SDK_CFLAGS := $(filter-out -Werror,$(ZZAPP_HARNESS_CFLAGS))

$(BUILD)/b/zz9k_host.o: ../../sdk/host/src/zz9k_host.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(B_SDK_CPPFLAGS) $(B_SDK_CFLAGS) -c -o $@ $<

$(BUILD)/b/zz9k_library.o: ../../sdk/amiga/src/zz9k_library.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(B_SDK_CPPFLAGS) $(B_SDK_CFLAGS) -c -o $@ $<

$(BUILD)/b/deferred_mailbox_test: b/deferred_mailbox_test.c $(B_DEFERRED_OBJS) $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(B_SDK_CPPFLAGS) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_HOST_DEFERRED_TEST -pthread -o $@ $< $(B_DEFERRED_OBJS) $(A_RUNTIME_OBJS)

$(BUILD)/b/reset_lifecycle_test: b/reset_lifecycle_test.c $(A_RUNTIME_OBJS) $(BUILD)/aar.o $(BUILD)/syscalls.o | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_HOST_DEFERRED_TEST -o $@ $< $(A_RUNTIME_OBJS) $(BUILD)/aar.o $(BUILD)/syscalls.o

$(BUILD)/b/host_visible_limit_test: b/host_visible_limit_test.c $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_HOST_DEFERRED_TEST -o $@ $< $(A_RUNTIME_OBJS)

$(BUILD)/b/teardown_test: b/teardown_test.c $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_HOST_DEFERRED_TEST -o $@ $< $(A_RUNTIME_OBJS)

LOADER_ZERO_TEST_SOURCES := b/loader_zero_test.c $(FW)/zzapp_mem.c \
	$(FW)/card_pool.c $(FW)/sdk_smp_lock.c ../card_pool/host_card_pool.c

$(BUILD)/b/loader_zero_test: $(LOADER_ZERO_TEST_SOURCES) $(FW)/zzapp_mem.h \
	$(FW)/card_pool.h $(FW)/sdk_smp_lock.h ../card_pool/host_card_pool.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DSMP_LOCK_HOST_TEST -I../card_pool -o $@ \
		$(LOADER_ZERO_TEST_SOURCES)


$(BUILD)/b/spin_template.o: $(SDK)/templates/spin.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -Daar_main=spin_template_main -c -o $@ $<
$(BUILD)/b/slot_selection_test: b/slot_selection_test.c \
	$(BUILD)/b/spin_template.o $(BUILD)/aar.o \
	$(LOADER_INCREMENTAL_HOST_OBJS) $(LOADER_INCREMENTAL_HOST_SOURCES) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DSMP_LOCK_HOST_TEST -I../card_pool \
		-DZZAPP_HOST_DEFERRED_TEST -Wl,--wrap=zzapp_loader_retire_resources \
		-Wl,--wrap=zzapp_kernel_stop_app \
		-o $@ $< $(BUILD)/b/spin_template.o \
		$(BUILD)/aar.o $(LOADER_INCREMENTAL_HOST_OBJS) \
		$(LOADER_INCREMENTAL_HOST_SOURCES)

$(BUILD)/b/load_end_status_test: b/load_end_status_test.c $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_HOST_DEFERRED_TEST -o $@ $< $(A_RUNTIME_OBJS)

# Suites that include zzapp_services.c on its own (stub kernel and loader) run
# over the record arena and the table layers. Table addresses go into 32-bit
# descriptors, so the programs are non-PIE and sit below 4 GiB.
B_REC_SOURCES := $(BUILD)/mock_client.o $(FW)/zzapp_rec.c $(FW)/zzapp_xport.c $(FW)/zzapp_as.c $(FW)/mmu_page.c \
	$(FW)/sdk_smp_lock.c $(FW)/card_pool.c ../card_pool/host_card_pool.c
B_REC_DEPS := $(FW)/zzapp_rec.h $(FW)/zzapp_xport.h mock_client.h $(FW)/zzapp_as.h $(FW)/mmu_page.h \
	$(FW)/sdk_smp_lock.h $(FW)/card_pool.h ../card_pool/host_card_pool.h \
	$(FW)/zzapp_services.c $(FW)/zzapp_services.h $(FW)/zzapp_proto.h \
	b/svc_env.h
B_REC_FLAGS := -fno-pie -no-pie -DMMU_PAGE_HOST_TEST -DSMP_LOCK_HOST_TEST \
	-I../card_pool -I../mmu/mock
B_STANDALONE_TESTS := mock_client_test services_test present_flush_test msg_wait_test \
	admission_test queue_runtime_test lost_client_test

define B_STANDALONE_RULE
$(BUILD)/b/$(1): b/$(1).c $(B_REC_SOURCES) $(B_REC_DEPS) | $(BUILD)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(ZZAPP_HARNESS_CFLAGS) $$(B_REC_FLAGS) -o $$@ $$< $$(B_REC_SOURCES)
endef
$(foreach t,$(B_STANDALONE_TESTS),$(eval $(call B_STANDALONE_RULE,$(t))))

# Real runtime many-app coverage: no launcher windows, only the record arena.
$(BUILD)/b/many_apps_test: b/many_apps_test.c $(LOADER_REAL_AS_HOST_OBJS) $(LOADER_REAL_AS_HOST_SOURCES) $(BUILD)/a/host_rec_glue.o $(FW)/zzapp_services.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) $(A_REC_FLAGS) -DZZAPP_HOST_DEFERRED_TEST \
		-o $@ $< $(LOADER_REAL_AS_HOST_OBJS) \
		$(LOADER_REAL_AS_HOST_SOURCES) $(BUILD)/a/host_rec_glue.o
