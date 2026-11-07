# GPL test driver for the app runtime host harness. Slices may either drop a
# self-contained <slice>/*_test.c (auto-discovered) or add <slice>/tests.mk
# and append targets to ZZAPP_TESTS.
#
# SPDX-License-Identifier: GPL-3.0-or-later

CC ?= cc
BUILD := build
SDK := ../../sdk/zzapp
FW := ../../ZZ9000_proto.sdk/ZZ9000OS/src
HOST_CPPFLAGS := -DZZAPP_HOST_TEST -I$(SDK)/include -I. -I$(FW)
HOST_CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror
ZZAPP_HARNESS_CFLAGS := $(HOST_CPPFLAGS) $(HOST_CFLAGS)
ZZAPP_HARNESS_OBJS := $(BUILD)/mock_window.o $(BUILD)/mock_api.o $(BUILD)/host_loader.o \
	$(BUILD)/zzapp.o $(BUILD)/gl_wrap.o $(BUILD)/syscalls.o

# Hook for A/B/C test owners. They may append custom binaries/rules here.
-include $(wildcard [a-f]/tests.mk)

AUTO_TEST_SOURCES := $(wildcard [a-f]/*_test.c)
AUTO_TESTS := $(patsubst %.c,$(BUILD)/%,$(AUTO_TEST_SOURCES))
ZZAPP_TESTS ?=
ALL_TESTS := $(AUTO_TESTS) $(ZZAPP_TESTS)

.PHONY: test clean module-harness

test: $(ALL_TESTS)
	@set -e; for test in $(ALL_TESTS); do $$test; done

# Library CI links this host-only runtime archive with the pinned module and
# front-end objects, then supplies registry fakes from its test driver.
MODULE_HARNESS := $(BUILD)/libzzapp_module_harness.a
MODULE_HARNESS_OBJS := $(BUILD)/a/zzapp_loader.o $(BUILD)/a/zzapp_host.o \
	$(BUILD)/a/zzapp_api.o

module-harness: $(MODULE_HARNESS)

$(MODULE_HARNESS): $(MODULE_HARNESS_OBJS) | $(BUILD)
	$(AR) rcs $@ $^

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/mock_window.o: mock_window.c mock_window.h | $(BUILD)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -c -o $@ $<

$(BUILD)/mock_api.o: mock_api.c mock_api.h mock_window.h | $(BUILD)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -c -o $@ $<

$(BUILD)/host_loader.o: host_loader.c host_loader.h | $(BUILD)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -c -o $@ $<

$(BUILD)/zzapp.o: $(SDK)/src/zzapp.c $(SDK)/src/zzapp_internal.h | $(BUILD)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -I$(SDK)/src -c -o $@ $<

$(BUILD)/gl_wrap.o: $(SDK)/src/gl_wrap.c $(SDK)/src/zzapp_internal.h | $(BUILD)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -I$(SDK)/src -c -o $@ $<

$(BUILD)/syscalls.o: $(SDK)/src/syscalls.c | $(BUILD)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -c -o $@ $<

$(BUILD)/%: %.c $(ZZAPP_HARNESS_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -rdynamic -o $@ $< $(ZZAPP_HARNESS_OBJS) -ldl

clean:
	rm -rf $(BUILD)
