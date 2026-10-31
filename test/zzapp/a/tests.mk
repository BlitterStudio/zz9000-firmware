A_RUNTIME_OBJS := $(BUILD)/a/zzapp_loader.o $(BUILD)/a/zzapp_host.o $(BUILD)/a/zzapp_api.o
ZZAPP_TESTS += $(BUILD)/a/loader_smoke

$(BUILD)/a/zzapp_loader.o: $(FW)/zzapp_loader.c $(FW)/zzapp_runtime.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -c -o $@ $<
$(BUILD)/a/zzapp_host.o: $(FW)/zzapp_host.c $(FW)/zzapp_runtime.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -c -o $@ $<
$(BUILD)/a/zzapp_api.o: $(FW)/zzapp_api.c $(FW)/zzapp_runtime.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -c -o $@ $<
$(BUILD)/a/loader_smoke: a/loader_smoke.c $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $< $(A_RUNTIME_OBJS)
