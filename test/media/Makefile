CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c99
FIRMWARE_SRC := ../../ZZ9000_proto.sdk/ZZ9000OS/src

.PHONY: test clean

test: build/media_session_test build/video_stream_test \
	build/media_overlay_source_test
	build/media_session_test
	build/video_stream_test
	build/media_overlay_source_test \
		$(FIRMWARE_SRC)/sdk_media_session.c $(FIRMWARE_SRC)/overlay.c

build/media_session_test: media_session_test.c \
	$(FIRMWARE_SRC)/sdk_media_session.c \
	$(FIRMWARE_SRC)/sdk_media_session.h \
	$(FIRMWARE_SRC)/sdk_mailbox.h \
	$(FIRMWARE_SRC)/overlay_path.h \
	$(FIRMWARE_SRC)/sdk_media_profile.h \
	$(FIRMWARE_SRC)/sdk_media_profile.c \
	$(FIRMWARE_SRC)/sdk_video_stream.h
	mkdir -p build
	$(CC) $(CFLAGS) -DSDK_MEDIA_HOST_TEST=1 -I$(FIRMWARE_SRC) -o $@ \
		media_session_test.c $(FIRMWARE_SRC)/sdk_media_session.c \
		$(FIRMWARE_SRC)/sdk_media_profile.c

build/video_stream_test: video_stream_test.c mock/xil_cache.h \
	$(FIRMWARE_SRC)/sdk_video_stream.c \
	$(FIRMWARE_SRC)/sdk_video_stream.h \
	$(FIRMWARE_SRC)/sdk_video_backend.h \
	$(FIRMWARE_SRC)/sdk_mailbox.h
	mkdir -p build
	$(CC) $(CFLAGS) -DSDK_VIDEO_HOST_TEST=1 -Imock \
		-I$(FIRMWARE_SRC) -o $@ video_stream_test.c \
		$(FIRMWARE_SRC)/sdk_video_stream.c

build/media_overlay_source_test: media_overlay_source_test.c
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ media_overlay_source_test.c

clean:
	rm -rf build
