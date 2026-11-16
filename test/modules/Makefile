CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections
SRC := ../../ZZ9000_proto.sdk/ZZ9000OS/src
LOADER_TARGET := build/module_loader_test
MAILBOX_TARGET := build/mailbox_image_begin_test
PACKER_TARGET := build/zzm_pack
PACKER_TEST_TARGET := build/zzm_pack_test

.PHONY: test clean
test: $(LOADER_TARGET) $(MAILBOX_TARGET) $(PACKER_TARGET) $(PACKER_TEST_TARGET)
	$(LOADER_TARGET)
	$(MAILBOX_TARGET)
	$(PACKER_TEST_TARGET) ./$(PACKER_TARGET)
$(LOADER_TARGET): module_loader_test.c $(SRC)/zzm_loader.c $(SRC)/zzm_manifest.c $(SRC)/zzm_registry.c $(SRC)/sdk_video_stream.c $(SRC)/sdk_smp_lock.c
	@mkdir -p build
	$(CC) $(CFLAGS) -DZZM_LOADER_HOST_TEST -DSDK_VIDEO_HOST_TEST -Wl,--gc-sections -Imock -I$(SRC) -o $@ $^
$(MAILBOX_TARGET): mailbox_image_begin_test.c $(SRC)/sdk_mailbox.c $(SRC)/scheduler.c
	@mkdir -p build
	$(CC) $(CFLAGS) -static -DTASKQ_HOST_TEST -DSDK_MAILBOX_HOST_TEST \
		-include mock/mailbox_test_decls.h -Wl,--gc-sections \
		-Wl,--unresolved-symbols=ignore-all -Imock -I$(SRC) -o $@ $^
$(PACKER_TARGET): ../../util/zzm_pack.c $(SRC)/zzm_manifest.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Werror -I$(SRC) -o $@ $^
$(PACKER_TEST_TARGET): zzm_pack_test.c $(SRC)/zzm_format.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Werror -I$(SRC) -o $@ zzm_pack_test.c
clean:
	rm -rf build
