# GPL test driver for the app runtime host harness. Slices may either drop a
# self-contained <slice>/*_test.c (auto-discovered) or add <slice>/tests.mk
# and append targets to ZZAPP_TESTS.
#
# SPDX-License-Identifier: GPL-3.0-or-later

CC ?= cc
BUILD := build
include ../../sdk/aar.mk
SDK := $(AAR_DIR)
FW := ../../ZZ9000_proto.sdk/ZZ9000OS/src
HOST_CPPFLAGS := -DZZAPP_HOST_TEST -I$(SDK)/include -I$(SDK)/libaar/src -I. -I$(FW)
HOST_CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror
ZZAPP_HARNESS_CFLAGS := $(HOST_CPPFLAGS) $(HOST_CFLAGS)
ZZAPP_HARNESS_OBJS := $(BUILD)/mock_window.o $(BUILD)/mock_api.o $(BUILD)/host_loader.o \
	$(BUILD)/aar.o $(BUILD)/gl.o $(BUILD)/syscalls.o

# Hook for test owners. They may append custom binaries/rules here.
-include $(wildcard [a-i]/tests.mk)

AUTO_TEST_SOURCES := $(wildcard [a-i]/*_test.c)
AUTO_TESTS := $(patsubst %.c,$(BUILD)/%,$(AUTO_TEST_SOURCES))
ZZAPP_TESTS ?=
# Suites a slice lists again in its tests.mk are still run once.
ALL_TESTS := $(sort $(AUTO_TESTS) $(ZZAPP_TESTS))

.PHONY: test clean module-harness

test: $(ALL_TESTS)
	@set -e; for test in $(ALL_TESTS); do $$test; done

# Library CI links this host-only runtime archive with the pinned module and
# front-end objects, then supplies registry fakes from its test driver.
MODULE_HARNESS := $(BUILD)/libzzapp_module_harness.a
MODULE_HARNESS_OBJS := $(BUILD)/a/zzapp_loader.o $(BUILD)/a/zzapp_host.o \
	$(BUILD)/a/zzapp_api.o $(BUILD)/a/host_decode_stubs.o

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

$(BUILD)/aar.o: $(SDK)/libaar/src/aar.c $(SDK)/libaar/src/aar_internal.h | $(BUILD)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -c -o $@ $<

$(BUILD)/gl.o: $(SDK)/libaar/src/gl.c $(SDK)/libaar/src/aar_internal.h | $(BUILD)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -c -o $@ $<

# libaar's newlib glue defines _sbrk, _exit, _open and the other syscall stubs.
# On a glibc host those names are the C library's own, so the wrapper renames
# them to aar_host_* before it includes the glue. The firmware include
# directory is left out: its linux/errno.h shim hides glibc's errno constants.
$(BUILD)/syscalls.o: host_syscalls.c $(SDK)/libaar/src/syscalls.c | $(BUILD)
	$(CC) -DZZAPP_HOST_TEST -I$(SDK)/include -I$(SDK)/libaar/src $(HOST_CFLAGS) -c -o $@ $<

$(BUILD)/%: %.c $(ZZAPP_HARNESS_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -rdynamic -o $@ $< $(ZZAPP_HARNESS_OBJS) -ldl

clean:
	rm -rf $(BUILD)
