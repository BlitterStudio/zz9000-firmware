CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGET := $(BUILD_DIR)/ethernet_mcast_test

CPPFLAGS += -Istubs -I$(SRC_DIR)

.PHONY: test rx-ring clean

test: $(TARGET) rx-ring
	$(TARGET)

# The firmware's own RX ring and restart functions and the bundled BSP ring code, extracted
# from source and run against a stub GEM (ASan + UBSan).
rx-ring:
	python3 run_rx_bd.py --cc $(CC)
	python3 run_rx_init.py --cc $(CC)
	python3 run_rx_restart.py --cc $(CC)
	python3 run_rx_mac_restart.py --cc $(CC)
	python3 run_rx_offset2_retry.py --cc $(CC)

$(TARGET): ethernet_mcast_test.c $(SRC_DIR)/ethernet_mcast.c \
		$(SRC_DIR)/ethernet.h stubs/xemacps_hw.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ ethernet_mcast_test.c \
		$(SRC_DIR)/ethernet_mcast.c

clean:
	rm -rf $(BUILD_DIR)
