CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections
SRC := ../../ZZ9000_proto.sdk/ZZ9000OS/src
LOADER_TARGET := build/module_loader_test
MAILBOX_TARGET := build/mailbox_image_begin_test
PACKER_TARGET := build/zzm_pack
PACKER_TEST_TARGET := build/zzm_pack_test
BENCH_TARGET := build/zzm_load_bench_test
LOADER_BENCH_TARGET := build/module_loader_bench_test

IMAGE_TEST_CPPFLAGS :=
ifneq ($(wildcard $(SRC)/../build/deps/libjpeg-turbo/3.1.4.1/src/src/jpeglib.h),)
IMAGE_TEST_CPPFLAGS += -I$(SRC)/../build/deps/libjpeg-turbo/3.1.4.1/src/src
endif
ifneq ($(wildcard $(SRC)/../build/deps/libjpeg-turbo/3.1.4.1/build-simd/jconfig.h),)
IMAGE_TEST_CPPFLAGS += -I$(SRC)/../build/deps/libjpeg-turbo/3.1.4.1/build-simd
endif
ifneq ($(wildcard $(SRC)/../build/deps/libpng/1.6.58/src/png.h),)
IMAGE_TEST_CPPFLAGS += -I$(SRC)/../build/deps/libpng/1.6.58/src
endif
ifneq ($(wildcard $(SRC)/../build/deps/libpng/1.6.58/build-neon/pnglibconf.h),)
IMAGE_TEST_CPPFLAGS += -I$(SRC)/../build/deps/libpng/1.6.58/build-neon
endif

.PHONY: test clean
test: $(LOADER_TARGET) $(LOADER_BENCH_TARGET) $(BENCH_TARGET) $(MAILBOX_TARGET) $(PACKER_TARGET) $(PACKER_TEST_TARGET)
	$(LOADER_TARGET)
	$(LOADER_BENCH_TARGET)
	$(BENCH_TARGET)
	$(MAILBOX_TARGET)
	$(PACKER_TEST_TARGET) ./$(PACKER_TARGET)
$(LOADER_TARGET): module_loader_test.c $(SRC)/zzm_loader.c $(SRC)/zzm_manifest.c $(SRC)/zzm_registry.c $(SRC)/sdk_video_stream.c $(SRC)/sdk_image_stream.c $(SRC)/sdk_smp_lock.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Wno-clobbered $(IMAGE_TEST_CPPFLAGS) -DZZM_LOADER_HOST_TEST -DSDK_VIDEO_HOST_TEST -DSDK_IMAGE_HOST_TEST -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all -I$(SRC) -idirafter mock -o $@ $^
# The same loader test with the load step instrument compiled in: the hook in
# the loader runs on every step, one timer tick is one microsecond.
$(LOADER_BENCH_TARGET): module_loader_test.c $(SRC)/zzm_loader.c $(SRC)/zzm_manifest.c $(SRC)/zzm_registry.c $(SRC)/sdk_video_stream.c $(SRC)/sdk_image_stream.c $(SRC)/sdk_smp_lock.c $(SRC)/zzm_load_bench.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Wno-clobbered $(IMAGE_TEST_CPPFLAGS) -DZZM_LOADER_HOST_TEST -DSDK_VIDEO_HOST_TEST -DSDK_IMAGE_HOST_TEST -DZZM_LOAD_BENCH -DCOUNTS_PER_SECOND=1000000U -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all -I$(SRC) -idirafter mock -o $@ $^
$(BENCH_TARGET): zzm_load_bench_test.c $(SRC)/zzm_load_bench.c $(SRC)/zzm_load_bench.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Werror -DZZM_LOAD_BENCH_HOST_TEST -DCOUNTS_PER_SECOND=1000000U -I$(SRC) -o $@ zzm_load_bench_test.c $(SRC)/zzm_load_bench.c
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
