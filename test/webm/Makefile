CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c99
FIRMWARE_SRC := ../../ZZ9000_proto.sdk/ZZ9000OS/src
WEBM_MODULE_SRC := $(FIRMWARE_SRC)/../modules/webm

# The backend test compiles sdk_video_webm.c against stubs/, which shadows
# the libvpx, libopus and Tremor headers; the test file implements them.
BACKEND_SRCS := $(WEBM_MODULE_SRC)/sdk_video_webm.c \
	$(WEBM_MODULE_SRC)/webm_window.c \
	$(WEBM_MODULE_SRC)/webm_parse.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.c \
	$(FIRMWARE_SRC)/sdk_media_profile.c
BACKEND_HDRS := $(FIRMWARE_SRC)/sdk_video_backend.h \
	$(FIRMWARE_SRC)/webm_window.h \
	$(FIRMWARE_SRC)/webm_parse.h \
	$(FIRMWARE_SRC)/sdk_media_timeline.h \
	$(FIRMWARE_SRC)/sdk_media_profile.h \
	stubs/vpx/vpx_decoder.h stubs/vpx/vp8dx.h stubs/opus/opus.h \
	stubs/tremor/ivorbiscodec.h stubs/tremor/codec_internal.h

.PHONY: test clean

test: build/webm_parse_test build/webm_bounds_test build/webm_backend_test \
	build/webm_module_alloc_test
	build/webm_parse_test
	build/webm_bounds_test
	build/webm_backend_test
	build/webm_module_alloc_test

build/webm_parse_test: webm_parse_test.c ebml_build.h \
	$(WEBM_MODULE_SRC)/webm_parse.c $(FIRMWARE_SRC)/webm_parse.h \
	$(WEBM_MODULE_SRC)/webm_window.c $(FIRMWARE_SRC)/webm_window.h
	mkdir -p build
	$(CC) $(CFLAGS) -I$(FIRMWARE_SRC) -o $@ webm_parse_test.c \
		$(WEBM_MODULE_SRC)/webm_parse.c $(WEBM_MODULE_SRC)/webm_window.c

build/webm_bounds_test: webm_bounds_test.c ebml_build.h \
	$(WEBM_MODULE_SRC)/webm_parse.c $(FIRMWARE_SRC)/webm_parse.h
	mkdir -p build
	$(CC) $(CFLAGS) -I$(FIRMWARE_SRC) -o $@ webm_bounds_test.c \
		$(WEBM_MODULE_SRC)/webm_parse.c

build/webm_backend_test: webm_backend_test.c $(BACKEND_SRCS) $(BACKEND_HDRS)
	mkdir -p build
	$(CC) $(CFLAGS) -DSDK_VIDEO_HOST_TEST=1 -DSDK_MEDIA_HOST_TEST=1 \
		-Istubs -I$(FIRMWARE_SRC) -o $@ webm_backend_test.c \
		$(BACKEND_SRCS)

build/webm_module_alloc_test: webm_module_alloc_test.c \
	$(WEBM_MODULE_SRC)/webm_module_alloc.c \
	$(WEBM_MODULE_SRC)/webm_module_alloc.h \
	$(FIRMWARE_SRC)/sdk_vorbis_alloc.c $(FIRMWARE_SRC)/sdk_vorbis_alloc.h
	mkdir -p build
	$(CC) $(CFLAGS) -I$(WEBM_MODULE_SRC) -I$(FIRMWARE_SRC) -o $@ \
		webm_module_alloc_test.c $(WEBM_MODULE_SRC)/webm_module_alloc.c \
		$(FIRMWARE_SRC)/sdk_vorbis_alloc.c

clean:
	rm -rf build
