# Load each API 0.1 ELF through the host loader. Not auto-discovered.
#
# SPDX-License-Identifier: GPL-3.0-or-later

CC ?= cc
ROOT := ../../..
FW := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS/src
SDK := $(ROOT)/sdk/zzapp
CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -DZZAPP_HOST_TEST \
	-I$(SDK)/include -I.. -I$(FW)

BUILD := build
OBJS := $(BUILD)/zzapp_loader.o $(BUILD)/zzapp_host.o $(BUILD)/zzapp_api.o
ELFS := v0.1/hello.elf v0.1/pixels.elf v0.1/fault.elf

.PHONY: test clean

test: $(OBJS)
	@set -e; \
	for elf in $(ELFS); do \
	  abs=$$(cd $$(dirname $$elf) && pwd)/$$(basename $$elf); \
	  echo "probe $$elf"; \
	  $(CC) $(CFLAGS) -DZZAPP_PROBE_ELF="\"$$abs\"" -o $(BUILD)/probe \
	    ../a/loader_file_check.c $(OBJS); \
	  $(BUILD)/probe; \
	done

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/zzapp_loader.o: $(FW)/zzapp_loader.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/zzapp_host.o: $(FW)/zzapp_host.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/zzapp_api.o: $(FW)/zzapp_api.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)
