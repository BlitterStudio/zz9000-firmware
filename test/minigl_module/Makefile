CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O2
ROOT := ../..
FIRMWARE := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS

.PHONY: test host clean

HOST_LIB := libminigl_module_host.a

test: minigl_module_test
	./minigl_module_test

host: $(HOST_LIB)

$(HOST_LIB): module_host.o
	$(AR) rcs $@ $^

module_host.o: module_host.c module_host.h
	$(CC) $(CFLAGS) -DMINIGL_MODULE -DMINIGL_HOST_TEST -I$(FIRMWARE)/src -c $< -o $@

<<<<<<< HEAD
minigl_module_test: minigl_module_test.c $(FIRMWARE)/modules/minigl/minigl_module.c $(FIRMWARE)/src/minigl_platform.c $(HOST_LIB)
	$(CC) $(CFLAGS) -DMINIGL_MODULE -DMINIGL_HOST_TEST -I$(FIRMWARE)/src $< $(FIRMWARE)/modules/minigl/minigl_module.c $(FIRMWARE)/src/minigl_platform.c $(HOST_LIB) -o $@
=======
minigl_module_test: minigl_module_test.c $(FIRMWARE)/modules/minigl/minigl_module.c $(HOST_LIB)
	$(CC) $(CFLAGS) -DMINIGL_MODULE -DMINIGL_HOST_TEST -I$(FIRMWARE)/src $< $(FIRMWARE)/modules/minigl/minigl_module.c $(HOST_LIB) -o $@
>>>>>>> mod/app-gl

clean:
	rm -f module_host.o $(HOST_LIB) minigl_module_test
