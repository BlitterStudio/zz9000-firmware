CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c99
FIRMWARE_SRC := ../../ZZ9000_proto.sdk/ZZ9000OS/src

.PHONY: test clean

test: build/media_session_test build/media_overlay_source_test
	build/media_session_test
	build/media_overlay_source_test \
		$(FIRMWARE_SRC)/sdk_media_session.c $(FIRMWARE_SRC)/overlay.c

build/media_session_test: media_session_test.c \
	$(FIRMWARE_SRC)/sdk_media_session.c \
	$(FIRMWARE_SRC)/sdk_media_session.h \
	$(FIRMWARE_SRC)/sdk_mailbox.h \
	$(FIRMWARE_SRC)/sdk_video_stream.h
	mkdir -p build
	$(CC) $(CFLAGS) -I$(FIRMWARE_SRC) -o $@ \
		media_session_test.c $(FIRMWARE_SRC)/sdk_media_session.c

build/media_overlay_source_test: media_overlay_source_test.c
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ media_overlay_source_test.c

clean:
	rm -rf build
