# SPDX-License-Identifier: GPL-3.0-or-later
MEM_TEST_SOURCES := g/mem_test.c $(FW)/zzapp_mem.c $(FW)/card_pool.c \
	$(FW)/sdk_smp_lock.c ../card_pool/host_card_pool.c
MEM_TEST_DEPS := $(FW)/zzapp_mem.h $(FW)/card_pool.h \
	$(FW)/sdk_smp_lock.h ../card_pool/host_card_pool.h

$(BUILD)/g/mem_test: $(MEM_TEST_SOURCES) $(MEM_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DSMP_LOCK_HOST_TEST -I../card_pool -o $@ $(MEM_TEST_SOURCES)

KERNEL_SCHED_TEST_SOURCES := g/kernel_sched_test.c $(FW)/zzapp_kernel.c
KERNEL_SCHED_TEST_DEPS := $(FW)/zzapp_kernel.h

$(BUILD)/g/kernel_sched_test: $(KERNEL_SCHED_TEST_SOURCES) $(KERNEL_SCHED_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $(KERNEL_SCHED_TEST_SOURCES)
