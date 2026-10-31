CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror

ROOT := ../..
FIRMWARE := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS/src
# Module builds and host suites share this backend source inventory.  A staged
# module checkout can override it without changing the test graph.
MINIGL_MODULE_BACKEND ?= $(FIRMWARE)
CPPFLAGS := -I$(FIRMWARE) -DMINIGL_HOST_TEST
RIX_BACKEND := $(MINIGL_MODULE_BACKEND)/minigl_rix_backend.c $(MINIGL_MODULE_BACKEND)/rix_cmd.c $(MINIGL_MODULE_BACKEND)/rix_texture.c $(MINIGL_MODULE_BACKEND)/rix_setup.c $(MINIGL_MODULE_BACKEND)/rix_drv.c
TARGETS := minigl_raster_test minigl_polygon_test minigl_present_test minigl_lighting_test minigl_oracle_test rix_core_test rix_drv_test rix_backend_test

.PHONY: test differential clean

test: $(TARGETS)
	./minigl_raster_test
	./minigl_polygon_test
	./minigl_present_test
	./minigl_lighting_test
	./minigl_oracle_test
	./rix_core_test
	./rix_drv_test
	./rix_backend_test
differential:
	sh ./run_rix_differential.sh



minigl_raster_test: minigl_raster_test.c $(MINIGL_MODULE_BACKEND)/minigl_raster.c $(MINIGL_MODULE_BACKEND)/minigl_lighting.c $(RIX_BACKEND)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@
minigl_polygon_test: minigl_polygon_test.c $(MINIGL_MODULE_BACKEND)/minigl_raster.c $(MINIGL_MODULE_BACKEND)/minigl_lighting.c $(RIX_BACKEND)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

rix_core_test: rix_core_test.c $(MINIGL_MODULE_BACKEND)/rix_cmd.c $(MINIGL_MODULE_BACKEND)/rix_setup.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

rix_drv_test: rix_drv_test.c $(MINIGL_MODULE_BACKEND)/rix_drv.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

rix_backend_test: rix_backend_test.c $(RIX_BACKEND)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@


minigl_present_test: test_minigl_present.c $(MINIGL_MODULE_BACKEND)/minigl_present.c $(MINIGL_MODULE_BACKEND)/minigl_platform.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

minigl_lighting_test: minigl_lighting_test.c $(MINIGL_MODULE_BACKEND)/minigl_lighting.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@


minigl_raster_ref.o: minigl_raster_ref.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 -c $< -o $@

minigl_oracle_test: minigl_oracle_test.c $(MINIGL_MODULE_BACKEND)/minigl_raster.c $(MINIGL_MODULE_BACKEND)/minigl_lighting.c $(MINIGL_MODULE_BACKEND)/minigl_present.c $(MINIGL_MODULE_BACKEND)/minigl_platform.c minigl_raster_ref.o $(RIX_BACKEND)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@


clean:
	rm -f $(TARGETS) minigl_raster_ref.o
