/*
 * Host-side builder for the 8 KB ZZ9000 autoboot ROM image.
 *
 * The ARM firmware also populates this DDR window at runtime, but preloading
 * it into BOOT.bin lets FSBL place valid diag code in DDR before the FPGA
 * advertises the autoboot ROM during cold power-up autoconfig.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ZZ9000_proto.sdk/ZZ9000OS/src/bootrom_layout.h"
#include "../ZZ9000_proto.sdk/ZZ9000OS/src/diag-code.h"
#include "../ZZ9000_proto.sdk/ZZ9000OS/src/zzsd-device.h"

#define ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))

static void checked_copy(uint8_t *rom, size_t rom_size, size_t offset,
			 const uint8_t *src, size_t len, const char *name)
{
	if (offset > rom_size || len > rom_size - offset) {
		fprintf(stderr,
			"ERROR: %s does not fit in boot ROM window "
			"(offset=0x%zx len=0x%zx size=0x%zx)\n",
			name, offset, len, rom_size);
		exit(1);
	}
	memcpy(rom + offset, src, len);
}

static int env_enabled(const char *name)
{
	const char *value = getenv(name);

	return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void write_entry_stub(uint8_t *rom, size_t offset, const char *name)
{
	static const uint8_t return_zero[] = {
		0x70, 0x00,	/* moveq #0,d0 */
		0x4e, 0x75,	/* rts */
	};

	checked_copy(rom, ZZSD_ROM_SIZE_BYTES, offset, return_zero,
		     sizeof(return_zero), name);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "Usage: %s OUTPUT.bin\n", argv[0]);
		return 1;
	}

	if (diag_code_len != ARRAY_LEN(diag_code)) {
		fprintf(stderr, "ERROR: diag_code_len mismatch\n");
		return 1;
	}
	if (zzsd_device_len != ARRAY_LEN(zzsd_device)) {
		fprintf(stderr, "ERROR: zzsd_device_len mismatch\n");
		return 1;
	}

	uint8_t rom[ZZSD_ROM_SIZE_BYTES];
	memset(rom, 0, sizeof(rom));

	checked_copy(rom, sizeof(rom), 0,
		     ZZSD_BOOT_ROM_HEADER, sizeof(ZZSD_BOOT_ROM_HEADER),
		     "boot ROM header");
	checked_copy(rom, sizeof(rom), 0x20, diag_code, diag_code_len,
		     "diag code");
	checked_copy(rom, sizeof(rom), ZZSD_DRIVER_OFFSET, zzsd_device,
		     zzsd_device_len, "zzsd.device");

	if (env_enabled("ZZ9000_BOOTROM_DIAG_STUB")) {
		write_entry_stub(rom, ZZSD_DA_DIAG_POINT, "DiagPoint stub");
		write_entry_stub(rom, ZZSD_DA_BOOT_POINT, "BootPoint stub");
		printf("[bootrom] diag stub enabled; zzsd.device autoboot is disabled\n");
	}
	if (env_enabled("ZZ9000_BOOTROM_STUB_DIAG")) {
		write_entry_stub(rom, ZZSD_DA_DIAG_POINT, "DiagPoint stub");
		printf("[bootrom] DiagPoint stub enabled\n");
	}
	if (env_enabled("ZZ9000_BOOTROM_STUB_BOOT")) {
		write_entry_stub(rom, ZZSD_DA_BOOT_POINT, "BootPoint stub");
		printf("[bootrom] BootPoint stub enabled\n");
	}

	FILE *out = fopen(argv[1], "wb");
	if (!out) {
		perror(argv[1]);
		return 1;
	}
	if (fwrite(rom, 1, sizeof(rom), out) != sizeof(rom)) {
		perror("fwrite");
		fclose(out);
		return 1;
	}
	if (fclose(out) != 0) {
		perror("fclose");
		return 1;
	}

	printf("[bootrom] wrote %s (%zu bytes)\n", argv[1], sizeof(rom));
	return 0;
}
