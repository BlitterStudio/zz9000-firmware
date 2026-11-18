# SPDX-License-Identifier: GPL-3.0-or-later
# Host tests of the queue coherence probe (temporary AAR cutover diagnostic).
#
# qprobe_test runs sdk_qprobe.c against a fake Amiga. mailbox_teardown_test
# runs the production sdk_mailbox.c (the handler and every path that frees a
# shared-buffer slot) against the same probe code; like test/modules it links
# the mailbox with unresolved symbols ignored, as it calls only a few handlers.

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror

SRC := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD := build
CPPFLAGS += -DSDK_QPROBE_HOST_TEST -Imock -I$(SRC)

PROBE_DEPS := $(SRC)/sdk_qprobe.c $(SRC)/sdk_qprobe.h \
	../../sdk/tools/zz9k-qprobe.h $(wildcard mock/*.h)

.PHONY: test clean

test: $(BUILD)/qprobe_test $(BUILD)/mailbox_teardown_test
	$(BUILD)/qprobe_test
	$(BUILD)/mailbox_teardown_test

$(BUILD)/qprobe_test: qprobe_test.c $(PROBE_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ qprobe_test.c $(SRC)/sdk_qprobe.c

# The mailbox needs the mocks of test/modules for the headers it includes
# (zlib, FatFs, libjpeg, sleep); the probe mocks come first.
$(BUILD)/mailbox_teardown_test: mailbox_teardown_test.c $(PROBE_DEPS) \
	$(SRC)/sdk_mailbox.c $(SRC)/scheduler.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -DTASKQ_HOST_TEST -DSDK_MAILBOX_HOST_TEST \
		-include ../modules/mock/mailbox_test_decls.h \
		-I../modules/mock $(CFLAGS) -Wno-error -static \
		-Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all \
		-o $@ mailbox_teardown_test.c $(SRC)/sdk_qprobe.c \
		$(SRC)/sdk_mailbox.c $(SRC)/scheduler.c

clean:
	rm -rf $(BUILD)
