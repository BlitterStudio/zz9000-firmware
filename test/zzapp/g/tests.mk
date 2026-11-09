# SPDX-License-Identifier: GPL-3.0-or-later
MEM_TEST_SOURCES := g/mem_test.c $(FW)/zzapp_mem.c $(FW)/card_pool.c \
	$(FW)/sdk_smp_lock.c ../card_pool/host_card_pool.c
MEM_TEST_DEPS := $(FW)/zzapp_mem.h $(FW)/card_pool.h \
	$(FW)/sdk_smp_lock.h ../card_pool/host_card_pool.h

$(BUILD)/g/mem_test: $(MEM_TEST_SOURCES) $(MEM_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DSMP_LOCK_HOST_TEST -I../card_pool -o $@ $(MEM_TEST_SOURCES)
