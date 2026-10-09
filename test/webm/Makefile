CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c99
FIRMWARE_SRC := ../../ZZ9000_proto.sdk/ZZ9000OS/src

.PHONY: test clean

test: build/webm_parse_test
	build/webm_parse_test

build/webm_parse_test: webm_parse_test.c $(FIRMWARE_SRC)/webm_parse.c \
	$(FIRMWARE_SRC)/webm_parse.h
	mkdir -p build
	$(CC) $(CFLAGS) -I$(FIRMWARE_SRC) -o $@ webm_parse_test.c \
		$(FIRMWARE_SRC)/webm_parse.c

clean:
	rm -rf build
