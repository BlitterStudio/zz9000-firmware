F_RUNTIME_OBJS := $(BUILD)/a/zzapp_loader.o $(BUILD)/a/zzapp_host.o \
	$(BUILD)/a/zzapp_api.o $(BUILD)/a/zzapp_kernel.o
ZZAPP_TESTS += $(BUILD)/f/minigl_api_test

$(BUILD)/f/minigl_api_test: f/minigl_api_test.c $(FW)/zzm_minigl_host.c \
	$(F_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DTASKQ_HOST_TEST -o $@ $^

# The matrix header ships with the AAR templates, which are not on the default
# include path.
$(BUILD)/f/mgl_conformance_matrix_test: ZZAPP_HARNESS_CFLAGS += -I$(SDK)/templates
