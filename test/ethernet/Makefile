CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGET := $(BUILD_DIR)/ethernet_mcast_test

CPPFLAGS += -Istubs -I$(SRC_DIR)

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): ethernet_mcast_test.c $(SRC_DIR)/ethernet_mcast.c \
		$(SRC_DIR)/ethernet.h stubs/xemacps_hw.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ ethernet_mcast_test.c \
		$(SRC_DIR)/ethernet_mcast.c

clean:
	rm -rf $(BUILD_DIR)
