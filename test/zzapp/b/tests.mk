B_DEFERRED_OBJS := $(BUILD)/b/zz9k_host.o $(BUILD)/b/zz9k_library.o
ZZAPP_TESTS += $(BUILD)/b/deferred_mailbox_test $(BUILD)/b/loader_zero_test

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
	admission_test queue_runtime_test stop_wake_test
# Sources a standalone suite links besides the shared set: the strong
# zzapp_host_latch_stop of the host runtime, where the suite checks that a
# STOP reached the record.
B_EXTRA_admission_test := c/svc_stubs.c
B_EXTRA_msg_wait_test := c/svc_stubs.c
B_EXTRA_stop_wake_test := c/svc_stubs.c

define B_STANDALONE_RULE
$(BUILD)/b/$(1): b/$(1).c $(B_REC_SOURCES) $$(B_EXTRA_$(1)) $(B_REC_DEPS) | $(BUILD)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(ZZAPP_HARNESS_CFLAGS) $$(B_REC_FLAGS) -o $$@ $$< $$(B_REC_SOURCES) $$(B_EXTRA_$(1))
endef
$(foreach t,$(B_STANDALONE_TESTS),$(eval $(call B_STANDALONE_RULE,$(t))))

# Suites over the real runtime (b/rt_env.h): the production host, loader,
# kernel, address spaces, commit ledger and retire path, with only the
# execution of ARM user code substituted. There are no launcher windows: the
# claims are bounded by the record arena alone.
B_RT_TESTS := many_apps_test load_end_status_test host_visible_limit_test load_weight_test \
	teardown_test reset_lifecycle_test exit_reset_test lost_client_test \
	concurrent_load_test record_race_test
B_RT_DEPS := b/rt_env.h mock_client.h aar_note_image.h \
	$(FW)/zzapp_services.c $(FW)/zzapp_services.h
# Per suite: B_RTX_<name> extra link inputs, B_RTF_<name> extra flags.

B_RTX_exit_reset_test := $(BUILD)/b/spin_template.o $(BUILD)/aar.o
B_RTF_exit_reset_test := -Wl,--wrap=zzapp_loader_retire_resources \
	-Wl,--wrap=zzapp_kernel_stop_app
# zzapp_rec_state and zzapp_rec_next_allocated are wrapped to free a record
# between a walk's read of its state and the walk's use of the record.
B_RTF_record_race_test := -Wl,--wrap=zzapp_rec_state \
	-Wl,--wrap=zzapp_rec_next_allocated

define B_RT_RULE
$(BUILD)/b/$(1): b/$(1).c $$(LOADER_REAL_AS_HOST_OBJS) $$(LOADER_REAL_AS_HOST_SOURCES) \
	$(BUILD)/a/host_rec_glue.o $$(B_RT_DEPS) $$(B_RTX_$(1)) | $(BUILD)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(ZZAPP_HARNESS_CFLAGS) $$(A_REC_FLAGS) -DZZAPP_HOST_DEFERRED_TEST \
		$$(B_RTF_$(1)) -o $$@ $$< $$(LOADER_REAL_AS_HOST_OBJS) \
		$$(LOADER_REAL_AS_HOST_SOURCES) $(BUILD)/a/host_rec_glue.o $$(B_RTX_$(1))
endef
$(foreach t,$(B_RT_TESTS),$(eval $(call B_RT_RULE,$(t))))
