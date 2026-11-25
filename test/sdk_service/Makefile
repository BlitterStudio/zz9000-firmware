# SPDX-License-Identifier: GPL-3.0-or-later

FIRMWARE = ../../ZZ9000_proto.sdk/ZZ9000OS
SRC = $(FIRMWARE)/src
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -Werror -I$(SRC)

.PHONY: test clean

TARGETS := build/service_discovery_test build/mailbox_memory_test \
	build/client_release_test build/internal_completion_test \
	build/voice_handlers_test build/media_pump_release_test
build/service_discovery_test: service_discovery_test.c $(SRC)/sdk_service_catalog.h
	mkdir -p build
	$(CC) $(CFLAGS) $< -o $@

build/mailbox_memory_test: mailbox_memory_test.c $(SRC)/sdk_mailbox.c
	mkdir -p build
	$(CC) $(CFLAGS) $< -o $@


build/client_release_test: client_release_test.c \
	$(SRC)/sdk_client_release.h $(SRC)/scheduler.c
	mkdir -p build
	$(CC) $(CFLAGS) -DTASKQ_HOST_TEST -I$(SRC) -o $@ client_release_test.c \
		$(SRC)/scheduler.c

build/internal_completion_test: internal_completion_test.c \
	$(SRC)/sdk_internal_completion.h
	mkdir -p build
	$(CC) $(CFLAGS) -DTASKQ_HOST_TEST -I$(SRC) -o $@ internal_completion_test.c

# The voice handlers through the production dispatcher: sdk_mailbox.c and the
# real session and engine modules, with the hardware seams (allocator, host
# window, aperture, fabric producer calls) supplied by the test. Like
# test/modules, symbols outside the exercised paths stay unresolved.
MOCKS = ../modules/mock
build/voice_handlers_test: voice_handlers_test.c $(SRC)/sdk_mailbox.c \
	$(SRC)/scheduler.c $(SRC)/audio_voice_session.c \
	$(SRC)/audio_voice_session.h $(SRC)/audio_voice.c \
	$(SRC)/sdk_mailbox.h $(MOCKS)/mailbox_test_decls.h
	mkdir -p build
	$(CC) -O2 -g -Wall -Wextra -static -ffunction-sections \
		-fdata-sections -DTASKQ_HOST_TEST -DSDK_MAILBOX_HOST_TEST \
		-include $(MOCKS)/mailbox_test_decls.h \
		-Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all \
		-I$(MOCKS) -I$(SRC) -o $@ voice_handlers_test.c \
		$(SRC)/sdk_mailbox.c $(SRC)/scheduler.c \
		$(SRC)/audio_voice_session.c $(SRC)/audio_voice.c

# CLIENT_RELEASE against the AX media pump, the same way: sdk_mailbox.c
# through the dispatcher, the media session module and the fabric producer
# calls supplied by the test.
build/media_pump_release_test: media_pump_release_test.c $(SRC)/sdk_mailbox.c \
	$(SRC)/scheduler.c $(SRC)/sdk_media_session.h \
	$(SRC)/sdk_mailbox.h $(MOCKS)/mailbox_test_decls.h
	mkdir -p build
	$(CC) -O2 -g -Wall -Wextra -static -ffunction-sections \
		-fdata-sections -DTASKQ_HOST_TEST -DSDK_MAILBOX_HOST_TEST \
		-include $(MOCKS)/mailbox_test_decls.h \
		-Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all \
		-I$(MOCKS) -I$(SRC) -o $@ media_pump_release_test.c \
		$(SRC)/sdk_mailbox.c $(SRC)/scheduler.c

test: $(TARGETS)
	./build/service_discovery_test
	./build/mailbox_memory_test $(SRC)/sdk_mailbox.c
	./build/client_release_test
	./build/internal_completion_test
	./build/voice_handlers_test
	./build/media_pump_release_test

clean:
	rm -f $(TARGETS)
