CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c99
SANITIZE_FLAGS := -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
ROOT := ../..
FIRMWARE_SRC := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS/src
# Must match the pins in build_vorbis.sh (and LIBVORBIS_TAG in the firmware
# Makefile).
LIBVORBIS_ROOT := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS/build/deps/vorbis/ogg-1.3.6-tremor-820fb3237ea8/host
HOST_LIBVORBIS := $(LIBVORBIS_ROOT)/libvorbisdec.a
BUILD := build
RUNTIME := $(FIRMWARE_SRC)/sdk_audio_vorbis.c $(FIRMWARE_SRC)/sdk_vorbis_alloc.c \
	$(FIRMWARE_SRC)/sdk_decode_reclaim.c $(FIRMWARE_SRC)/card_pool.c \
	$(FIRMWARE_SRC)/sdk_smp_lock.c ../card_pool/host_card_pool.c host_runtime.c
HEADERS := $(FIRMWARE_SRC)/sdk_audio_vorbis.h $(FIRMWARE_SRC)/sdk_vorbis_alloc.h \
	$(FIRMWARE_SRC)/sdk_audio_codec_io.h $(FIRMWARE_SRC)/card_pool.h \
	../card_pool/host_card_pool.h host_runtime.h vorbis_fixtures.h
INCLUDES := -I$(FIRMWARE_SRC) -I$(LIBVORBIS_ROOT)/include -I../card_pool -I.

.PHONY: test sanitize clean

test: $(BUILD)/vorbis_backend_test
	$(BUILD)/vorbis_backend_test fixtures

$(HOST_LIBVORBIS): $(ROOT)/build_vorbis.sh $(FIRMWARE_SRC)/sdk_vorbis_alloc.h
	cd $(ROOT) && bash ./build_vorbis.sh --host

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/vorbis_backend_test: vorbis_backend_test.c $(RUNTIME) $(HEADERS) \
	$(HOST_LIBVORBIS) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ vorbis_backend_test.c $(RUNTIME) $(HOST_LIBVORBIS) -lm

# Instruments the backend, arena allocator, tracker and harness; the pinned
# libogg/Tremor archive keeps its release build configuration.
sanitize: clean
	$(MAKE) CFLAGS='$(CFLAGS) $(SANITIZE_FLAGS)' test

clean:
	rm -rf $(BUILD)
