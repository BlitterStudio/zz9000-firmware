# SPDX-License-Identifier: GPL-3.0-or-later
# The app services over the AAR transport (zzapp_services.c, included by the
# test with the stub kernel and loader) on the record arena and an injected
# host-visible heap.
SVC_XPORT_TEST_SOURCES := c/svc_xport_test.c c/svc_stubs.c $(FW)/zzapp_xport.c $(FW)/zzapp_rec.c \
	$(FW)/zzapp_as.c $(FW)/mmu_page.c $(FW)/sdk_smp_lock.c $(FW)/card_pool.c \
	../card_pool/host_card_pool.c
SVC_XPORT_TEST_DEPS := $(FW)/zzapp_services.c $(FW)/zzapp_services.h \
	$(FW)/zzapp_xport.h $(FW)/zzapp_rec.h $(FW)/zzapp_proto.h $(FW)/zzapp_runtime.h \
	$(FW)/zzapp_as.h $(FW)/mmu_page.h $(FW)/sdk_smp_lock.h $(FW)/card_pool.h \
	../card_pool/host_card_pool.h

$(BUILD)/c/svc_xport_test: $(SVC_XPORT_TEST_SOURCES) $(SVC_XPORT_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -fno-pie -no-pie -DMMU_PAGE_HOST_TEST \
		-DSMP_LOCK_HOST_TEST -I../card_pool -I../mmu/mock -o $@ $(SVC_XPORT_TEST_SOURCES)
