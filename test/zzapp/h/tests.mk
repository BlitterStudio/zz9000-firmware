# SPDX-License-Identifier: GPL-3.0-or-later
# Process records, handle table, record arena (slice 5b, zzapp_rec.[ch]).
REC_TEST_SOURCES := h/rec_test.c $(FW)/zzapp_rec.c $(FW)/zzapp_as.c \
	$(FW)/mmu_page.c $(FW)/sdk_smp_lock.c $(FW)/card_pool.c \
	../card_pool/host_card_pool.c
REC_TEST_DEPS := $(FW)/zzapp_rec.h $(FW)/zzapp_as.h $(FW)/mmu_page.h \
	$(FW)/sdk_smp_lock.h $(FW)/card_pool.h ../card_pool/host_card_pool.h

$(BUILD)/h/rec_test: $(REC_TEST_SOURCES) $(REC_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -fno-pie -no-pie -DMMU_PAGE_HOST_TEST -DSMP_LOCK_HOST_TEST \
		-I../card_pool -I../mmu/mock -o $@ $(REC_TEST_SOURCES)

$(BUILD)/h/coherent_test: h/coherent_test.c $(FW)/scheduler.c \
		$(FW)/scheduler.h $(REC_TEST_DEPS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -DTASKQ_HOST_TEST -o $@ \
		h/coherent_test.c $(FW)/scheduler.c
