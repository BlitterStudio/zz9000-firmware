/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Source guard for SDK/RTG DDR region reservations.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
	FILE *file;
	long length;
	char *data;

	file = fopen(path, "rb");
	if (!file)
		return 0;
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return 0;
	}
	length = ftell(file);
	if (length < 0) {
		fclose(file);
		return 0;
	}
	if (fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return 0;
	}
	data = (char *)malloc((size_t)length + 1U);
	if (!data) {
		fclose(file);
		return 0;
	}
	if (fread(data, 1U, (size_t)length, file) != (size_t)length) {
		free(data);
		fclose(file);
		return 0;
	}
	data[length] = '\0';
	fclose(file);
	return data;
}

static int expect_contains(const char *source, const char *label,
                           const char *needle)
{
	if (strstr(source, needle))
		return 1;
	printf("%s: missing %s\n", label, needle);
	return 0;
}

static int expect_not_contains(const char *source, const char *label,
                               const char *needle)
{
	if (!strstr(source, needle))
		return 1;
	printf("%s: unexpected %s\n", label, needle);
	return 0;
}

int main(int argc, char **argv)
{
	char *memorymap;
	char *main_source;
	char *dma_acc;
	int ok = 1;

	if (argc != 4) {
		printf("usage: %s <memorymap.h> <main.c> <dma_acc.c>\n",
		       argv[0]);
		return 2;
	}

	memorymap = read_file(argv[1]);
	main_source = read_file(argv[2]);
	dma_acc = read_file(argv[3]);
	if (!memorymap || !main_source || !dma_acc) {
		printf("failed to read input sources\n");
		free(memorymap);
		free(main_source);
		free(dma_acc);
		return 2;
	}

	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define RTG_TEMPLATE_SCRATCH_ADDRESS 0x03400000");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define RTG_TEMPLATE_SCRATCH_SIZE    0x00100000");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define LEGACY_SURFACE_HEAP_ADDRESS");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define LEGACY_SURFACE_HEAP_END");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define SDK_SHARED_HEAP_ADDRESS     0x03000000");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define SDK_SHARED_HEAP_SIZE        0x003F0000");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define SDK_SHARED_HEAP_END");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "SDK_SHARED_HEAP_END > Z3_SCRATCH_ADDR");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define SDK_LOCAL_SURFACE_HEAP_ADDRESS 0x06000000");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define SDK_LOCAL_SURFACE_HEAP_SIZE    0x02000000");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "SDK_LOCAL_SURFACE_HEAP_ADDRESS < LEGACY_SURFACE_HEAP_END");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "SDK_LOCAL_SURFACE_HEAP_END > SDK_LOW_DDR_RESERVED_END");
	ok &= expect_not_contains(memorymap, "memorymap.h",
	                          "#define SDK_LOCAL_SURFACE_HEAP_ADDRESS 0x03400000");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define SDK_HOST_WINDOW_HEAP_ADDRESS 0x005D0000");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define SDK_HOST_WINDOW_HEAP_SIZE    0x00004000");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "(SDK_HOST_WINDOW_HEAP_END - ADDR_ADJ) > 0x00400000");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "SDK_HOST_WINDOW_HEAP_END > (0x003F0000 + ADDR_ADJ)");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define SDK_AUDIO_DIRECT_RING_Z3_SLOTS              2U");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define SDK_AUDIO_DIRECT_RING_Z3_RESERVE_ADDRESS    0x081C0000");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define SDK_AUDIO_DIRECT_RING_Z2_SLOTS              1U");
	ok &= expect_contains(memorymap, "memorymap.h",
	                      "#define SDK_AUDIO_DIRECT_RING_Z2_RESERVE_SIZE       0x0000C000");

	ok &= expect_contains(main_source, "main.c",
	                      "surface_allocator_init(LEGACY_SURFACE_HEAP_ADDRESS, LEGACY_SURFACE_HEAP_SIZE)");
	ok &= expect_not_contains(main_source, "main.c",
	                          "cur_mem_offset = 0x3500000");

	ok &= expect_contains(dma_acc, "dma_acc.c",
	                      "surface_allocator_alloc(sfc_size)");
	ok &= expect_contains(dma_acc, "dma_acc.c",
	                      "not enough legacy surface heap");

	free(memorymap);
	free(main_source);
	free(dma_acc);
	return ok ? 0 : 1;
}
