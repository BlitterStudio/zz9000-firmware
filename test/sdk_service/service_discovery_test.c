/* Check every descriptor through the firmware discovery lookup and serializer. */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdk_service_catalog.h"

struct ExpectedService {
	uint32_t id;
	uint32_t opcode_count;
	const char *name;
};

static const struct ExpectedService expected[] = {
	{ 0x0000U, 6U, "core" },
	{ 0x0100U, 4U, "memory" },
	{ 0x0200U, 6U, "surface" },
	{ 0x0400U, 8U, "image" },
	{ 0x0600U, 7U, "codec" },
	{ 0x0500U, 21U, "audio" },
	{ 0x0800U, 5U, "crypto" },
	{ 0x0900U, 4U, "diag" },
	{ 0x0b00U, 14U, "video" }
};

static uint32_t read_be32(const volatile uint8_t *src)
{
	return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
	       ((uint32_t)src[2] << 8) | src[3];
}

int main(void)
{
	volatile struct SDKServiceInfoPayload info;
	const struct SDKServiceDescriptor *service;
	size_t i;

	if (sizeof(expected) / sizeof(expected[0]) !=
	    sizeof(sdk_services) / sizeof(sdk_services[0])) {
		puts("service discovery: catalog size changed");
		return 1;
	}

	for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
		service = find_service(expected[i].id);
		if (!service) {
			printf("service discovery: missing 0x%04x\n",
			       (unsigned)expected[i].id);
			return 1;
		}
		sdk_service_write_info(&info, service, service->capability_bits,
				       service->flags, 48U);
		if (read_be32(info.service_id) != expected[i].id ||
		    read_be32(info.opcode_base) != expected[i].id ||
		    read_be32(info.opcode_count) != expected[i].opcode_count ||
		    strcmp((const char *)info.name, expected[i].name) != 0) {
			printf("service discovery: invalid descriptor for 0x%04x\n",
			       (unsigned)expected[i].id);
			return 1;
		}
	}

	if (find_service(0xffffU) != 0) {
		puts("service discovery: unknown service unexpectedly found");
		return 1;
	}

	puts("service discovery: all descriptors passed");
	return 0;
}
