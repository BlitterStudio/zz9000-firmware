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

$(BUILD)/b/reset_lifecycle_test: b/reset_lifecycle_test.c $(A_RUNTIME_OBJS) $(BUILD)/syscalls.o | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_HOST_DEFERRED_TEST -o $@ $< $(A_RUNTIME_OBJS) $(BUILD)/syscalls.o

$(BUILD)/b/host_visible_limit_test: b/host_visible_limit_test.c $(A_RUNTIME_OBJS) $(BUILD)/syscalls.o | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_HOST_DEFERRED_TEST -o $@ $< $(A_RUNTIME_OBJS) $(BUILD)/syscalls.o

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
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -Dzz_main=spin_template_main -c -o $@ $<
$(BUILD)/b/slot_selection_test: b/slot_selection_test.c \
	$(BUILD)/b/spin_template.o $(BUILD)/zzapp.o \
	$(LOADER_INCREMENTAL_HOST_OBJS) $(LOADER_INCREMENTAL_HOST_SOURCES) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DSMP_LOCK_HOST_TEST -I../card_pool \
		-DZZAPP_HOST_DEFERRED_TEST -Wl,--wrap=zzapp_loader_release \
		-Wl,--wrap=zzapp_kernel_stop_app \
		-o $@ $< $(BUILD)/b/spin_template.o \
		$(BUILD)/zzapp.o $(LOADER_INCREMENTAL_HOST_OBJS) \
		$(LOADER_INCREMENTAL_HOST_SOURCES)
