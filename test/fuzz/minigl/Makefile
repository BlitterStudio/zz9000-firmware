CC := clang
AR ?= ar
ROOT := ../../..
FIRMWARE := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS/src
MINIGL_MODULE_BACKEND ?= $(FIRMWARE)
CPPFLAGS := -I$(FIRMWARE) -DMINIGL_HOST_TEST
CFLAGS := -std=c99 -Wall -Wextra -Werror -O2 -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -g
FUZZFLAGS := -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g
SECONDS ?= 1200

MW_OBJS := minigl_platform.o mw_parse.o mw_texture.o mw_draw.o mw_ctx.o mw_service.o sdk_palette.o \
           minigl_raster.o minigl_lighting.o minigl_present.o \
           minigl_rix_backend.o rix_cmd.o rix_texture.o rix_setup.o rix_drv.o

TARGETS := fuzz_submit fuzz_submit_rix fuzz_request fuzz_request_rix fuzz_context fuzz_context_rix

.PHONY: all fuzz replay clean corpus

all: libmw_fuzz.a gen_corpus $(TARGETS)

libmw_fuzz.a: $(MW_OBJS)
	$(AR) rcs $@ $^

%.o: $(MINIGL_MODULE_BACKEND)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

gen_corpus: gen_corpus.c $(MINIGL_MODULE_BACKEND)/mw_parse.c $(MINIGL_MODULE_BACKEND)/sdk_palette.c
	$(CC) $(CPPFLAGS) -std=c99 -O2 $^ -lm -o $@

fuzz_submit: fuzz_submit.c libmw_fuzz.a
	$(CC) $(CPPFLAGS) $(FUZZFLAGS) $< libmw_fuzz.a -lm -o $@

fuzz_submit_rix: fuzz_submit.c libmw_fuzz.a
	$(CC) $(CPPFLAGS) -DFUZZ_RASTERIX=1 $(FUZZFLAGS) $< libmw_fuzz.a -lm -o $@

fuzz_request: fuzz_request.c libmw_fuzz.a
	$(CC) $(CPPFLAGS) $(FUZZFLAGS) $< libmw_fuzz.a -lm -o $@

fuzz_request_rix: fuzz_request.c libmw_fuzz.a
	$(CC) $(CPPFLAGS) -DFUZZ_RASTERIX=1 $(FUZZFLAGS) $< libmw_fuzz.a -lm -o $@

fuzz_context: fuzz_context.c libmw_fuzz.a
	$(CC) $(CPPFLAGS) $(FUZZFLAGS) $< libmw_fuzz.a -lm -o $@

fuzz_context_rix: fuzz_context.c libmw_fuzz.a
	$(CC) $(CPPFLAGS) -DFUZZ_RASTERIX=1 $(FUZZFLAGS) $< libmw_fuzz.a -lm -o $@

corpus: gen_corpus
	./gen_corpus

FUZZ_RUNS := $(addprefix fuzz-,$(TARGETS))
fuzz: all corpus $(FUZZ_RUNS)
fuzz-%: % corpus
	@echo "=== Fuzzing $* for $(SECONDS) seconds ==="
	@mkdir -p corpus/$*
	./$* -max_total_time=$(SECONDS) corpus/$*

replay: all corpus
	@for target in $(TARGETS); do \
		echo "=== Replaying corpus for $$target ==="; \
		./$$target -runs=0 corpus/$$target; \
	done

fuzz-%: % corpus
	./$* -max_total_time=$(SECONDS) corpus/$*

replay-%: % corpus
	./$* -runs=0 corpus/$*

clean:
	rm -f *.o libmw_fuzz.a gen_corpus $(TARGETS)
