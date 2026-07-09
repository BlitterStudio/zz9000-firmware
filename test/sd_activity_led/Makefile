CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
MOCK_DIR := mock
BUILD_DIR := build
TARGET := $(BUILD_DIR)/sd_activity_led_test

CPPFLAGS += -I$(MOCK_DIR) -I$(SRC_DIR)

SOURCES := sd_activity_led_test.c $(SRC_DIR)/sd_activity_led.c
DEPS := $(SRC_DIR)/sd_activity_led.h $(MOCK_DIR)/xgpiops.h $(MOCK_DIR)/xparameters.h $(MOCK_DIR)/xtime_l.h

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

clean:
	rm -rf $(BUILD_DIR)
