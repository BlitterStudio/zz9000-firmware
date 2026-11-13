# SPDX-License-Identifier: GPL-3.0-or-later
MEM_TEST_SOURCES := g/mem_test.c $(FW)/zzapp_mem.c $(FW)/card_pool.c \
	$(FW)/sdk_smp_lock.c ../card_pool/host_card_pool.c
MEM_TEST_DEPS := $(FW)/zzapp_mem.h $(FW)/card_pool.h \
	$(FW)/sdk_smp_lock.h ../card_pool/host_card_pool.h

$(BUILD)/g/mem_test: $(MEM_TEST_SOURCES) $(MEM_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DSMP_LOCK_HOST_TEST -I../card_pool -o $@ $(MEM_TEST_SOURCES)

LOW_MEMORY_EVENT_SOURCES := g/low_memory_event_test.c $(FW)/zzapp_mem.c \
	$(FW)/card_pool.c $(FW)/sdk_smp_lock.c ../card_pool/host_card_pool.c

$(BUILD)/g/low_memory_event_test: $(LOW_MEMORY_EVENT_SOURCES) $(A_RUNTIME_OBJS) \
	$(BUILD)/a/zzapp_services.o | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DSMP_LOCK_HOST_TEST -I../card_pool -o $@ \
		$(LOW_MEMORY_EVENT_SOURCES) $(A_RUNTIME_OBJS) $(BUILD)/a/zzapp_services.o

KERNEL_SCHED_TEST_SOURCES := g/kernel_sched_test.c $(FW)/zzapp_kernel.c
KERNEL_SCHED_TEST_DEPS := $(FW)/zzapp_kernel.h

$(BUILD)/g/kernel_sched_test: $(KERNEL_SCHED_TEST_SOURCES) $(KERNEL_SCHED_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $(KERNEL_SCHED_TEST_SOURCES)

SVC_TEST_SOURCES := g/svc_test.c $(FW)/zzapp_svc.c $(FW)/zzapp_as.c \
	$(FW)/mmu_page.c $(FW)/sdk_smp_lock.c
SVC_TEST_DEPS := $(FW)/zzapp_svc.h $(FW)/zzapp_gl_contract.h $(FW)/zzapp_as.h \
	$(FW)/mmu_page.h $(FW)/sdk_smp_lock.h

ZZAPP_TESTS += $(BUILD)/g/svc_no_counters_test

$(BUILD)/g/svc_test: $(SVC_TEST_SOURCES) $(SVC_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_PERF_COUNTERS=1 -DZZAPP_SVC_COUNTERS_TEST -DMMU_PAGE_HOST_TEST -I../mmu/mock -o $@ $(SVC_TEST_SOURCES)

$(BUILD)/g/svc_no_counters_test: $(SVC_TEST_SOURCES) $(SVC_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_PERF_COUNTERS=0 -DMMU_PAGE_HOST_TEST -I../mmu/mock -o $@ $(SVC_TEST_SOURCES)

ZZAPP_TESTS += $(BUILD)/g/svc_probes_test

$(BUILD)/g/svc_probes_test: $(SVC_TEST_SOURCES) $(SVC_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_PERF_COUNTERS=0 -DZZAPP_SVC_PROBES=1 -DMMU_PAGE_HOST_TEST -I../mmu/mock -o $@ $(SVC_TEST_SOURCES)

ZZAPP_TESTS += $(BUILD)/g/svc_probes_counters_test

$(BUILD)/g/svc_probes_counters_test: $(SVC_TEST_SOURCES) $(SVC_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_PERF_COUNTERS=1 -DZZAPP_SVC_COUNTERS_TEST -DZZAPP_SVC_PROBES=1 -DMMU_PAGE_HOST_TEST -I../mmu/mock -o $@ $(SVC_TEST_SOURCES)
