# SPDX-License-Identifier: GPL-3.0-or-later
ZZAPP_TESTS += $(BUILD)/e/map_test

$(BUILD)/e/map_app.so: e/map_app.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -fPIC -shared -o $@ $<

$(BUILD)/e/map_test: e/map_test.c $(BUILD)/e/map_app.so $(ZZAPP_HARNESS_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -rdynamic -o $@ $< $(ZZAPP_HARNESS_OBJS) -ldl
