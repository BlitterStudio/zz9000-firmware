CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections
SRC := ../../ZZ9000_proto.sdk/ZZ9000OS/src
TARGET := build/module_loader_test

.PHONY: test clean
test: $(TARGET)
	$(TARGET)
$(TARGET): module_loader_test.c $(SRC)/zzm_loader.c $(SRC)/zzm_manifest.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Wl,--gc-sections -Imock -I$(SRC) -o $@ $^
clean:
	rm -rf build
