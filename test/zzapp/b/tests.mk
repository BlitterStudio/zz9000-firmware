B_DEFERRED_OBJS := $(BUILD)/b/zz9k_host.o $(BUILD)/b/zz9k_library.o
ZZAPP_TESTS += $(BUILD)/b/deferred_mailbox_test

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
