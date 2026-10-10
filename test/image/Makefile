# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -Wno-clobbered -std=c99 -D_GNU_SOURCE
ROOT := ../..
FIRMWARE_SRC := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS/src
LIBWEBP_ROOT := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS/build/deps/libwebp/1.6.0/host
BUILD := build
HOST_LIBWEBP := $(LIBWEBP_ROOT)/lib/libwebpdecoder.a
HOST_DEMUX := $(LIBWEBP_ROOT)/lib/libwebpdemux.a
RUNTIME := $(FIRMWARE_SRC)/sdk_image_stream.c $(FIRMWARE_SRC)/sdk_jpeg.c $(FIRMWARE_SRC)/sdk_surface.c $(FIRMWARE_SRC)/sdk_smp_lock.c \
	$(FIRMWARE_SRC)/sdk_decode_reclaim.c $(FIRMWARE_SRC)/sdk_webp_alloc.c $(FIRMWARE_SRC)/sdk_vorbis_alloc.c \
	$(FIRMWARE_SRC)/card_pool.c ../card_pool/host_card_pool.c
INCLUDES := -I$(FIRMWARE_SRC) -Imock -I../card_pool -I$(LIBWEBP_ROOT)/include
# host_runtime.c observes the WebP arenas' pool allocations through the wrap.
LDLIBS := -Wl,--wrap=card_pool_alloc $(HOST_DEMUX) $(HOST_LIBWEBP) -lpng -ljpeg -lz -lm

.PHONY: test smoke fixtures sanitize clean

test: $(BUILD)/jpeg_png_image_test $(BUILD)/webp_image_test $(BUILD)/webp_animation_test
	$(BUILD)/jpeg_png_image_test
	$(BUILD)/webp_image_test
	$(BUILD)/webp_animation_test

smoke: $(BUILD)/webp_smoke
	@printf '%s\n' 'usage: $(BUILD)/webp_smoke input.webp output.ppm'
fixtures: $(BUILD)/webp_fixture_extract
	@printf '%s\n' 'usage: $(BUILD)/webp_fixture_extract {lossy|alpha|animation} output.webp'

$(BUILD)/webp_fixture_extract: webp_fixture_extract.c webp_fixtures.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ webp_fixture_extract.c

$(HOST_LIBWEBP): $(ROOT)/build_libwebp.sh $(FIRMWARE_SRC)/webp/CMakeLists.txt $(FIRMWARE_SRC)/sdk_webp_alloc.h
	cd $(ROOT) && bash ./build_libwebp.sh --host

# libwebp creates the decoder and demux archives as one unit. Make the
# demux archive wait for the sole producer, and rebuild that unit if only the
# secondary archive was removed between invocations.
$(HOST_DEMUX): $(HOST_LIBWEBP)
	@if test ! -f "$@"; then \
		rm -f "$(HOST_LIBWEBP)"; \
		$(MAKE) "$(HOST_LIBWEBP)"; \
	fi
	@test -f "$@"

$(BUILD):
	mkdir -p $@

$(BUILD)/jpeg_png_image_test: $(ROOT)/util/test_sdk_image_stream.c host_runtime.c $(RUNTIME) $(HOST_LIBWEBP) $(HOST_DEMUX) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(ROOT)/util/test_sdk_image_stream.c host_runtime.c $(RUNTIME) $(LDLIBS)
$(BUILD)/webp_image_test: webp_image_test.c webp_fixtures.h host_runtime.c $(RUNTIME) $(HOST_LIBWEBP) $(HOST_DEMUX) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ webp_image_test.c host_runtime.c $(RUNTIME) $(LDLIBS)
$(BUILD)/webp_animation_test: webp_animation_test.c webp_animation_fixtures.h webp_fixtures.h host_runtime.c $(RUNTIME) $(HOST_LIBWEBP) $(HOST_DEMUX) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ webp_animation_test.c host_runtime.c $(RUNTIME) $(LDLIBS)

$(BUILD)/webp_smoke: webp_image_test.c webp_fixtures.h host_runtime.c $(RUNTIME) $(HOST_LIBWEBP) $(HOST_DEMUX) | $(BUILD)
	$(CC) $(CFLAGS) -DZZ9K_WEBP_SMOKE $(INCLUDES) -o $@ webp_image_test.c host_runtime.c $(RUNTIME) $(LDLIBS)

sanitize: clean
	$(MAKE) CFLAGS='$(CFLAGS) -O1 -fno-omit-frame-pointer -fsanitize=address,undefined' test
clean:
	rm -rf $(BUILD)
