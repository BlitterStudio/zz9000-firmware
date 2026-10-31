CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O2
ROOT := ../..
FIRMWARE := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS

.PHONY: test clean

test: minigl_module_test
	./minigl_module_test

minigl_module_test: minigl_module_test.c $(FIRMWARE)/modules/minigl/minigl_module.c
	$(CC) $(CFLAGS) -DMINIGL_MODULE -DMINIGL_HOST_TEST -I$(FIRMWARE)/src $^ -o $@

clean:
	rm -f minigl_module_test
