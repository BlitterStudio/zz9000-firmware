CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror

ROOT := ../..
FIRMWARE := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS/src
CPPFLAGS := -I$(FIRMWARE) -DMINIGL_HOST_TEST
RIX_BACKEND := $(FIRMWARE)/minigl_rix_backend.c $(FIRMWARE)/rix_cmd.c $(FIRMWARE)/rix_texture.c $(FIRMWARE)/rix_setup.c $(FIRMWARE)/rix_bin.c $(FIRMWARE)/rix_drv.c
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



minigl_raster_test: minigl_raster_test.c $(FIRMWARE)/minigl_raster.c $(FIRMWARE)/minigl_lighting.c $(RIX_BACKEND)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@
minigl_polygon_test: minigl_polygon_test.c $(FIRMWARE)/minigl_raster.c $(FIRMWARE)/minigl_lighting.c $(RIX_BACKEND)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

rix_core_test: rix_core_test.c $(FIRMWARE)/rix_cmd.c $(FIRMWARE)/rix_setup.c $(FIRMWARE)/rix_bin.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

rix_drv_test: rix_drv_test.c $(FIRMWARE)/rix_drv.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

rix_backend_test: rix_backend_test.c $(RIX_BACKEND)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@


minigl_present_test: test_minigl_present.c $(FIRMWARE)/minigl_present.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

minigl_lighting_test: minigl_lighting_test.c $(FIRMWARE)/minigl_lighting.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@


minigl_raster_ref.o: minigl_raster_ref.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 -c $< -o $@

minigl_oracle_test: minigl_oracle_test.c $(FIRMWARE)/minigl_raster.c $(FIRMWARE)/minigl_lighting.c $(FIRMWARE)/minigl_present.c minigl_raster_ref.o $(RIX_BACKEND)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@


clean:
	rm -f $(TARGETS) minigl_raster_ref.o
