CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGETS := $(BUILD_DIR)/usb_proxy_protocol_test $(BUILD_DIR)/usb_proxy_recovery_test \
	$(BUILD_DIR)/ehci_lifecycle_test $(BUILD_DIR)/usb_proxy_diag_test \
	$(BUILD_DIR)/ehci_periodic_test $(BUILD_DIR)/usb_event_irq_test \
	$(BUILD_DIR)/ehci_iso_test $(BUILD_DIR)/usb_proxy_iso_test

CPPFLAGS += -Istubs -I$(SRC_DIR)

.PHONY: test clean

test: $(TARGETS)
	$(BUILD_DIR)/usb_proxy_protocol_test
	$(BUILD_DIR)/usb_proxy_recovery_test
	$(BUILD_DIR)/ehci_lifecycle_test
	$(BUILD_DIR)/usb_proxy_diag_test
	$(BUILD_DIR)/ehci_periodic_test
	$(BUILD_DIR)/usb_event_irq_test
	$(BUILD_DIR)/ehci_iso_test
	$(BUILD_DIR)/usb_proxy_iso_test

$(BUILD_DIR)/usb_proxy_protocol_test: usb_proxy_protocol_test.c $(SRC_DIR)/usb_proxy.h stubs/xil_types.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ usb_proxy_protocol_test.c

$(BUILD_DIR)/usb_proxy_recovery_test: usb_proxy_recovery_test.c $(SRC_DIR)/usb_proxy.h stubs/xil_types.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ usb_proxy_recovery_test.c

$(BUILD_DIR)/ehci_lifecycle_test: ehci_lifecycle_test.c $(SRC_DIR)/usb/ehci_lifecycle.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ ehci_lifecycle_test.c

$(BUILD_DIR)/usb_proxy_diag_test: usb_proxy_diag_test.c $(SRC_DIR)/usb_proxy_diag.c $(SRC_DIR)/usb_proxy_diag.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ usb_proxy_diag_test.c $(SRC_DIR)/usb_proxy_diag.c

$(BUILD_DIR)/ehci_periodic_test: ehci_periodic_test.c $(SRC_DIR)/usb/ehci_periodic.h $(SRC_DIR)/usb_proxy_periodic_ring.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ ehci_periodic_test.c

$(BUILD_DIR)/usb_event_irq_test: usb_event_irq_test.c $(SRC_DIR)/usb_event_irq.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ usb_event_irq_test.c

$(BUILD_DIR)/ehci_iso_test: ehci_iso_test.c $(SRC_DIR)/usb/ehci-iso.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ ehci_iso_test.c

$(BUILD_DIR)/usb_proxy_iso_test: usb_proxy_iso_test.c $(SRC_DIR)/usb_proxy_iso.c \
		$(SRC_DIR)/usb_proxy_iso.h $(SRC_DIR)/usb/ehci-iso.h \
		stubs/xil_cache.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ usb_proxy_iso_test.c \
		$(SRC_DIR)/usb_proxy_iso.c

clean:
	rm -rf $(BUILD_DIR)
