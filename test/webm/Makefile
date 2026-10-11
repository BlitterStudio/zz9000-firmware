CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c99
FIRMWARE_SRC := ../../ZZ9000_proto.sdk/ZZ9000OS/src

# The backend test compiles sdk_video_webm.c against stubs/, which shadows
# the libvpx, libopus and Tremor headers; the test file implements them.
BACKEND_SRCS := $(FIRMWARE_SRC)/sdk_video_webm.c \
	$(FIRMWARE_SRC)/webm_window.c \
	$(FIRMWARE_SRC)/webm_parse.c \
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

test: build/webm_parse_test build/webm_bounds_test build/webm_backend_test
	build/webm_parse_test
	build/webm_bounds_test
	build/webm_backend_test

build/webm_parse_test: webm_parse_test.c ebml_build.h \
	$(FIRMWARE_SRC)/webm_parse.c $(FIRMWARE_SRC)/webm_parse.h
	mkdir -p build
	$(CC) $(CFLAGS) -I$(FIRMWARE_SRC) -o $@ webm_parse_test.c \
		$(FIRMWARE_SRC)/webm_parse.c

build/webm_bounds_test: webm_bounds_test.c ebml_build.h \
	$(FIRMWARE_SRC)/webm_parse.c $(FIRMWARE_SRC)/webm_parse.h
	mkdir -p build
	$(CC) $(CFLAGS) -I$(FIRMWARE_SRC) -o $@ webm_bounds_test.c \
		$(FIRMWARE_SRC)/webm_parse.c

build/webm_backend_test: webm_backend_test.c $(BACKEND_SRCS) $(BACKEND_HDRS)
	mkdir -p build
	$(CC) $(CFLAGS) -DSDK_VIDEO_HOST_TEST=1 -DSDK_MEDIA_HOST_TEST=1 \
		-Istubs -I$(FIRMWARE_SRC) -o $@ webm_backend_test.c \
		$(BACKEND_SRCS)

clean:
	rm -rf build
