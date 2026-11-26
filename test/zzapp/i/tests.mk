# SPDX-License-Identifier: GPL-3.0-or-later
# The AAR transport core (zzapp_xport.[ch]) over the record arena and an
# injected host-visible heap. The two-thread case needs -pthread.
XPORT_TEST_SOURCES := i/xport_test.c $(FW)/zzapp_xport.c $(FW)/zzapp_rec.c \
	$(FW)/zzapp_as.c $(FW)/mmu_page.c $(FW)/sdk_smp_lock.c $(FW)/card_pool.c \
	../card_pool/host_card_pool.c
XPORT_TEST_DEPS := $(FW)/zzapp_xport.h $(FW)/zzapp_rec.h $(FW)/zzapp_proto.h \
	$(FW)/zzapp_as.h $(FW)/mmu_page.h $(FW)/sdk_smp_lock.h $(FW)/card_pool.h \
	../card_pool/host_card_pool.h

$(BUILD)/i/xport_test: $(XPORT_TEST_SOURCES) $(XPORT_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -fno-pie -no-pie -pthread -DMMU_PAGE_HOST_TEST \
		-DSMP_LOCK_HOST_TEST -I../card_pool -I../mmu/mock -o $@ $(XPORT_TEST_SOURCES)

# The private wire, on its own.
$(BUILD)/i/proto_wire_test: i/proto_wire_test.c $(FW)/zzapp_proto.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -Wall -Wextra -Werror -I$(FW) \
		-o $@ i/proto_wire_test.c

# The lifecycle on the transport: host.c and the loader over the record arena,
# with the real surface of the transport (zzapp_xport.c) and a services layer
# reduced to what the host calls. Links like a/retire_test; the lists are
# spelled out so the rule does not depend on which tests.mk make reads first.
LIFE_TEST_OBJS := $(BUILD)/a/zzapp_loader_real_as_incremental.o \
	$(BUILD)/a/zzapp_host.o $(BUILD)/a/zzapp_api.o $(BUILD)/a/zzapp_kernel.o \
	$(BUILD)/a/zzapp_rec.o $(BUILD)/a/host_decode_stubs.o
LIFE_TEST_SOURCES := $(FW)/zzapp_xport.c $(FW)/zzapp_mem.c $(FW)/card_pool.c \
	$(FW)/sdk_smp_lock.c $(FW)/zzapp_as.c $(FW)/mmu_page.c \
	../card_pool/host_card_pool.c
$(BUILD)/i/life_test: i/life_test.c $(LIFE_TEST_OBJS) $(LIFE_TEST_SOURCES) $(FW)/zzapp_xport.h $(FW)/zzapp_state.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DSMP_LOCK_HOST_TEST -DMMU_PAGE_HOST_TEST \
		-I../card_pool -I../mmu/mock -o $@ i/life_test.c $(LIFE_TEST_OBJS) \
		$(LIFE_TEST_SOURCES)

# The fault stack snapshot, the stall record and the profiler, over the same
# runtime objects as life_test.
$(BUILD)/i/fault_profile_test: i/fault_profile_test.c $(LIFE_TEST_OBJS) $(LIFE_TEST_SOURCES) $(FW)/zzapp_xport.h $(FW)/zzapp_state.h $(FW)/zzapp_kernel.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DSMP_LOCK_HOST_TEST -DMMU_PAGE_HOST_TEST \
		-I../card_pool -I../mmu/mock -o $@ i/fault_profile_test.c $(LIFE_TEST_OBJS) \
		$(LIFE_TEST_SOURCES)
