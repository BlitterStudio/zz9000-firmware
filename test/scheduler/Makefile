CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

SRC_DIR := ../../ZZ9000_proto.sdk/ZZ9000OS/src
BUILD_DIR := build
TARGET := $(BUILD_DIR)/scheduler_test

CPPFLAGS += -DTASKQ_HOST_TEST -I$(SRC_DIR)

SOURCES := scheduler_test.c $(SRC_DIR)/scheduler.c
DEPS := $(SRC_DIR)/scheduler.h

SMP_LOCK_TARGET := $(BUILD_DIR)/smp_lock_test
SMP_LOCK_CPPFLAGS := -DSMP_LOCK_HOST_TEST -I$(SRC_DIR)
SMP_LOCK_SOURCES := smp_lock_test.c $(SRC_DIR)/sdk_smp_lock.c
SMP_LOCK_DEPS := $(SRC_DIR)/sdk_smp_lock.h
SMP_LOCK_LDLIBS := -lpthread

OFFLOAD_PARAMS_TARGET := $(BUILD_DIR)/offload_params_test
OFFLOAD_PARAMS_CPPFLAGS := -DTASKQ_HOST_TEST -I$(SRC_DIR)
OFFLOAD_PARAMS_SOURCES := offload_params_test.c $(SRC_DIR)/scheduler.c
OFFLOAD_PARAMS_DEPS := $(SRC_DIR)/scheduler.h $(SRC_DIR)/sdk_offload_params.h

.PHONY: test clean smp_lock offload_params

test: $(TARGET)
	$(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

smp_lock: $(SMP_LOCK_TARGET)
	$(SMP_LOCK_TARGET)

$(SMP_LOCK_TARGET): $(SMP_LOCK_SOURCES) $(SMP_LOCK_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(SMP_LOCK_CPPFLAGS) $(CFLAGS) -o $@ $(SMP_LOCK_SOURCES) $(SMP_LOCK_LDLIBS)

offload_params: $(OFFLOAD_PARAMS_TARGET)
	$(OFFLOAD_PARAMS_TARGET)

$(OFFLOAD_PARAMS_TARGET): $(OFFLOAD_PARAMS_SOURCES) $(OFFLOAD_PARAMS_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OFFLOAD_PARAMS_CPPFLAGS) $(CFLAGS) -o $@ $(OFFLOAD_PARAMS_SOURCES)

clean:
	rm -rf $(BUILD_DIR)
