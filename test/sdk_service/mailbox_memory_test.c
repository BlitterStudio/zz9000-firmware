/* Host regression for SDK host-window diagnostic accounting. */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_WINDOW_BASE 0x005d0000U
#define HOST_WINDOW_SIZE 0x00004000U
#define MAX_BUFFERS 4U

struct HostWindowBuffer {
	uint32_t address;
	uint32_t length;
	int in_use;
};

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1U) & ~(alignment - 1U);
}

static int allocate(struct HostWindowBuffer *buffers, uint32_t length,
		    uint32_t alignment)
{
	uint32_t candidate = align_up(HOST_WINDOW_BASE, alignment);
	uint32_t end = HOST_WINDOW_BASE + HOST_WINDOW_SIZE;
	uint32_t i;

	while (candidate < end && length <= end - candidate) {
		int collision = 0;

		for (i = 0U; i < MAX_BUFFERS; i++) {
			uint32_t block_end;

			if (!buffers[i].in_use)
				continue;
			block_end = buffers[i].address + buffers[i].length;
			if (candidate + length <= buffers[i].address ||
			    candidate >= block_end)
				continue;
			candidate = align_up(block_end, alignment);
			collision = 1;
			break;
		}
		if (!collision) {
			for (i = 0U; i < MAX_BUFFERS; i++) {
				if (!buffers[i].in_use) {
					buffers[i].address = candidate;
					buffers[i].length = length;
					buffers[i].in_use = 1;
					return (int)i;
				}
			}
		}
	}
	return -1;
}

static void stats(const struct HostWindowBuffer *buffers, uint32_t *free_total,
		  uint32_t *largest_free)
{
	uint32_t end = HOST_WINDOW_BASE + HOST_WINDOW_SIZE;
	uint32_t cursor = HOST_WINDOW_BASE;
	uint32_t i;

	*free_total = 0U;
	*largest_free = 0U;
	for (;;) {
		uint32_t next_start = end;
		uint32_t next_end = end;

		for (i = 0U; i < MAX_BUFFERS; i++) {
			if (!buffers[i].in_use || buffers[i].address < cursor ||
			    buffers[i].address >= next_start)
				continue;
			next_start = buffers[i].address;
			next_end = buffers[i].address + buffers[i].length;
		}
		*free_total += next_start - cursor;
		if (next_start - cursor > *largest_free)
			*largest_free = next_start - cursor;
		if (next_start == end)
			return;
		cursor = next_end;
	}
}

static char *read_file(const char *path)
{
	FILE *file;
	long length;
	char *contents;

	file = fopen(path, "rb");
	if (!file)
		return 0;
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
	    fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return 0;
	}
	contents = malloc((size_t)length + 1U);
	if (contents && fread(contents, 1U, (size_t)length, file) !=
		(size_t)length) {
		free(contents);
		contents = 0;
	}
	fclose(file);
	if (contents)
		contents[length] = '\0';
	return contents;
}

static void test_diag_uses_host_window_stats(const char *source_path)
{
	char *source = read_file(source_path);
	char *handler;
	char *handler_end;

	CHECK(source != 0);
	if (!source)
		return;
	CHECK(strstr(source, "return SDK_HOST_WINDOW_Z3_ADDRESS;") != 0);
	CHECK(strstr(source, "return SDK_HOST_WINDOW_HEAP_ADDRESS;") != 0);
	handler = strstr(source, "static uint16_t handle_diag_memory(");
	handler_end = handler ? strstr(handler, "\nstatic uint16_t handle_query_service(") : 0;
	CHECK(handler != 0);
	CHECK(handler_end != 0);
	if (handler && handler_end) {
		*handler_end = '\0';
		CHECK(strstr(handler, "host_window_free_stats(host_arm, host_total,") != 0);
		CHECK(strstr(handler, "heap_free_stats(&free_total, &largest_free)") == 0);
	}
	free(source);
}

static void test_host_window_diagnostic_stats(void)
{
	struct HostWindowBuffer buffers[MAX_BUFFERS] = { { 0 } };
	uint32_t free_total;
	uint32_t largest_free;
	int first;
	int second;

	first = allocate(buffers, 0x1000U, 16U);
	second = allocate(buffers, 0x0800U, 0x2000U);
	CHECK(first >= 0);
	CHECK(second >= 0);
	if (first >= 0 && second >= 0) {
		CHECK(buffers[first].address == HOST_WINDOW_BASE);
		CHECK(buffers[second].address == HOST_WINDOW_BASE + 0x2000U);
	}
	stats(buffers, &free_total, &largest_free);
	CHECK(free_total == 0x2800U);
	CHECK(largest_free == 0x1800U);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		puts("usage: mailbox_memory_test path/to/sdk_mailbox.c");
		return 1;
	}
	test_diag_uses_host_window_stats(argv[1]);
	test_host_window_diagnostic_stats();
	printf("%d checks, %d failures\n", checks, failures);
	return failures != 0;
}
