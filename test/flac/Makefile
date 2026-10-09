CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c99
SANITIZE_FLAGS := -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
ROOT := ../..
FIRMWARE_SRC := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS/src
LIBFLAC_ROOT := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS/build/deps/flac/1.5.0/host
HOST_LIBFLAC := $(LIBFLAC_ROOT)/libflacdec.a
BUILD := build
RUNTIME := $(FIRMWARE_SRC)/sdk_audio_flac.c $(FIRMWARE_SRC)/sdk_flac_alloc.c \
	$(FIRMWARE_SRC)/sdk_decode_reclaim.c host_runtime.c
INCLUDES := -I$(FIRMWARE_SRC) -I$(LIBFLAC_ROOT)/include -I.

.PHONY: test sanitize clean

test: $(BUILD)/flac_backend_test
	$(BUILD)/flac_backend_test

$(HOST_LIBFLAC): $(ROOT)/build_flac.sh $(FIRMWARE_SRC)/sdk_flac_alloc.h
	cd $(ROOT) && bash ./build_flac.sh --host

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/flac_backend_test: flac_backend_test.c flac_fixtures.h host_runtime.c host_runtime.h \
	$(RUNTIME) $(FIRMWARE_SRC)/sdk_audio_flac.h $(FIRMWARE_SRC)/sdk_flac_alloc.h \
	$(HOST_LIBFLAC) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ flac_backend_test.c $(RUNTIME) $(HOST_LIBFLAC)

# Instruments the backend, allocation shim, tracker and harness; the pinned
# libFLAC archive keeps its release build configuration.
sanitize: clean
	$(MAKE) CFLAGS='$(CFLAGS) $(SANITIZE_FLAGS)' test

clean:
	rm -rf $(BUILD)
