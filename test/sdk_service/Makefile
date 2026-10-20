# SPDX-License-Identifier: GPL-3.0-or-later

FIRMWARE = ../../ZZ9000_proto.sdk/ZZ9000OS
SRC = $(FIRMWARE)/src
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -Werror -I$(SRC)

.PHONY: test clean

TARGETS := build/service_discovery_test build/mailbox_memory_test

build/service_discovery_test: service_discovery_test.c $(SRC)/sdk_service_catalog.h
	mkdir -p build
	$(CC) $(CFLAGS) $< -o $@

build/mailbox_memory_test: mailbox_memory_test.c $(SRC)/sdk_mailbox.c
	mkdir -p build
	$(CC) $(CFLAGS) $< -o $@

test: $(TARGETS)
	./build/service_discovery_test
	./build/mailbox_memory_test $(SRC)/sdk_mailbox.c

clean:
	rm -f $(TARGETS)
