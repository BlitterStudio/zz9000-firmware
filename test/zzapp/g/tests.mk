# SPDX-License-Identifier: GPL-3.0-or-later
MEM_TEST_SOURCES := g/mem_test.c $(FW)/zzapp_mem.c $(FW)/card_pool.c \
	$(FW)/sdk_smp_lock.c ../card_pool/host_card_pool.c
MEM_TEST_DEPS := $(FW)/zzapp_mem.h $(FW)/card_pool.h \
	$(FW)/sdk_smp_lock.h ../card_pool/host_card_pool.h

$(BUILD)/g/mem_test: $(MEM_TEST_SOURCES) $(MEM_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_MEM_HOST_TEST -DSMP_LOCK_HOST_TEST -I../card_pool -o $@ $(MEM_TEST_SOURCES)

# App decoder owners (sdk_compression.h): header-only, over the card pool.
DECODE_OWNER_TEST_SOURCES := g/decode_owner_test.c $(FW)/card_pool.c \
	$(FW)/sdk_smp_lock.c ../card_pool/host_card_pool.c

$(BUILD)/g/decode_owner_test: $(DECODE_OWNER_TEST_SOURCES) $(FW)/sdk_compression.h $(FW)/zzapp_rec.h $(FW)/card_pool.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DSMP_LOCK_HOST_TEST -I../card_pool -o $@ $(DECODE_OWNER_TEST_SOURCES)

# Decode sessions (zzapp_api.c) on a real record arena and scheduler; the host
# runtime, the decode heap and the image stream are doubles in the test.
DECODE_SESSIONS_TEST_SOURCES := g/decode_sessions_test.c $(FW)/zzapp_api.c \
	$(FW)/zzapp_kernel.c $(FW)/zzapp_rec.c $(FW)/zzapp_as.c $(FW)/mmu_page.c \
	$(FW)/card_pool.c $(FW)/sdk_smp_lock.c ../card_pool/host_card_pool.c

$(BUILD)/g/decode_sessions_test: $(DECODE_SESSIONS_TEST_SOURCES) $(FW)/zzapp_rec.h $(FW)/zzapp_kernel.h $(FW)/sdk_compression.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -fno-pie -no-pie -DMMU_PAGE_HOST_TEST -DSMP_LOCK_HOST_TEST -I../card_pool -I../mmu/mock -o $@ $(DECODE_SESSIONS_TEST_SOURCES)

LOW_MEMORY_EVENT_SOURCES := g/low_memory_event_test.c $(FW)/zzapp_mem.c \
	../card_pool/host_card_pool.c

$(BUILD)/g/low_memory_event_test: $(LOW_MEMORY_EVENT_SOURCES) $(A_RUNTIME_OBJS) \
	$(BUILD)/a/zzapp_services.o | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DSMP_LOCK_HOST_TEST -I../card_pool -o $@ \
		$(LOW_MEMORY_EVENT_SOURCES) $(A_RUNTIME_OBJS) $(BUILD)/a/zzapp_services.o

KERNEL_SCHED_TEST_SOURCES := g/kernel_sched_test.c $(FW)/zzapp_kernel.c
KERNEL_SCHED_TEST_DEPS := $(FW)/zzapp_kernel.h $(FW)/zzapp_rec.h

# Record headers hold 32-bit stack and table addresses, and the stack test
# paints a real buffer through them: link below 4 GiB.
$(BUILD)/g/kernel_sched_test: $(KERNEL_SCHED_TEST_SOURCES) $(KERNEL_SCHED_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -fno-pie -no-pie -DZZAPP_KERNEL_PAINT_STACK -o $@ $(KERNEL_SCHED_TEST_SOURCES)

$(BUILD)/g/core1_restart_test: g/core1_restart_test.c $(A_RUNTIME_OBJS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -Wl,--wrap=zzapp_loader_retire_resources -Wl,--wrap=zzapp_minigl_reclaim -o $@ $< $(A_RUNTIME_OBJS)

# The SVC layer keeps its state in process records: the test attaches a static
# record arena (zzapp_rec.c) and the scheduler (zzapp_kernel.c, for the counter
# block pointer). Table addresses go into 32-bit descriptors, so link below
# 4 GiB.
SVC_TEST_SOURCES := g/svc_test.c $(FW)/zzapp_svc.c $(FW)/zzapp_as.c \
	$(FW)/mmu_page.c $(FW)/sdk_smp_lock.c $(FW)/zzapp_rec.c \
	$(FW)/zzapp_kernel.c $(FW)/card_pool.c ../card_pool/host_card_pool.c
SVC_TEST_DEPS := $(FW)/zzapp_svc.h $(FW)/zzapp_gl_contract.h $(FW)/zzapp_as.h \
	$(FW)/mmu_page.h $(FW)/sdk_smp_lock.h $(FW)/zzapp_rec.h \
	$(FW)/zzapp_kernel.h $(FW)/card_pool.h ../card_pool/host_card_pool.h
SVC_TEST_FLAGS := -fno-pie -no-pie -DMMU_PAGE_HOST_TEST -DSMP_LOCK_HOST_TEST \
	-I../card_pool -I../mmu/mock

ZZAPP_TESTS += $(BUILD)/g/svc_no_counters_test

$(BUILD)/g/svc_test: $(SVC_TEST_SOURCES) $(SVC_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_PERF_COUNTERS=1 -DZZAPP_SVC_COUNTERS_TEST $(SVC_TEST_FLAGS) -o $@ $(SVC_TEST_SOURCES)

$(BUILD)/g/svc_no_counters_test: $(SVC_TEST_SOURCES) $(SVC_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_PERF_COUNTERS=0 $(SVC_TEST_FLAGS) -o $@ $(SVC_TEST_SOURCES)

ZZAPP_TESTS += $(BUILD)/g/svc_probes_test

$(BUILD)/g/svc_probes_test: $(SVC_TEST_SOURCES) $(SVC_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_PERF_COUNTERS=0 -DZZAPP_SVC_PROBES=1 $(SVC_TEST_FLAGS) -o $@ $(SVC_TEST_SOURCES)

ZZAPP_TESTS += $(BUILD)/g/svc_probes_counters_test

$(BUILD)/g/svc_probes_counters_test: $(SVC_TEST_SOURCES) $(SVC_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DZZAPP_PERF_COUNTERS=1 -DZZAPP_SVC_COUNTERS_TEST -DZZAPP_SVC_PROBES=1 $(SVC_TEST_FLAGS) -o $@ $(SVC_TEST_SOURCES)

$(BUILD)/g/aar_events_test: g/aar_events_test.c $(A_RUNTIME_OBJS) \
	$(BUILD)/a/zzapp_services.o | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -o $@ $< $(A_RUNTIME_OBJS) \
		$(BUILD)/a/zzapp_services.o

# Weighted fair share of the scheduler (virtual run time, wake and join
# rules, yield, sub-millisecond charging) over an injected microsecond clock.
KERNEL_FAIR_SHARE_TEST_SOURCES := g/kernel_fair_share_test.c $(FW)/zzapp_kernel.c
KERNEL_FAIR_SHARE_TEST_DEPS := $(FW)/zzapp_kernel.h $(FW)/zzapp_rec.h $(FW)/zzapp_proto.h

$(BUILD)/g/kernel_fair_share_test: $(KERNEL_FAIR_SHARE_TEST_SOURCES) $(KERNEL_FAIR_SHARE_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -fno-pie -no-pie -o $@ $(KERNEL_FAIR_SHARE_TEST_SOURCES)
