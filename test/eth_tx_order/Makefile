# SPDX-License-Identifier: GPL-3.0-or-later

FIRMWARE = ../../ZZ9000_proto.sdk/ZZ9000OS
SRC = $(FIRMWARE)/src
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -Werror -I$(SRC)

.PHONY: test clean

build/eth_tx_order_test: eth_tx_order_test.c $(SRC)/eth_tx_order.h
	mkdir -p build
	$(CC) $(CFLAGS) $< -o $@

test: build/eth_tx_order_test
	./build/eth_tx_order_test

clean:
	rm -f build/eth_tx_order_test
