/* Check every descriptor through the firmware discovery lookup and serializer. */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdk_service_catalog.h"
#include "sdk_mailbox.h"
#include "zzapp_proto.h"
struct ExpectedService {
	uint32_t id;
	uint32_t opcode_count;
	const char *name;
};

static const struct ExpectedService expected[] = {
	{ 0x0000U, 6U, "core" },
	{ 0x0100U, 4U, "memory" },
	{ 0x0200U, 6U, "surface" },
	{ 0x0400U, 12U, "image" },
	{ 0x0600U, 7U, "codec" },
	{ 0x0500U, 23U, "audio" },
	{ 0x0800U, 5U, "crypto" },
	{ 0x0900U, 4U, "diag" },
	{ 0x0b00U, 14U, "video" },
	{ 0x0c00U, 12U, "app" },	/* 0x0c00..0x0c0b: KICK is the last */
	{ 0x8200U, 1U, "console" }
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
		/*
		 * The voice opcodes (0x0518..0x051B) are in the firmware but
		 * unadvertised: the audio descriptor keeps its flag clear and
		 * its opcode count at 23 until the engine is qualified on
		 * hardware. Setting the flag and raising the count is the
		 * change that flips this assertion.
		 */
		if (expected[i].id == SDK_SERVICE_AUDIO &&
		    ((read_be32(info.flags) & SDK_SERVICE_FLAG_AUDIO_VOICE) != 0U ||
		     read_be32(info.opcode_count) != 23U)) {
			puts("service discovery: audio advertises the voice engine");
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

/* The mailbox router forwards the whole app opcode range through
 * handle_app_op; both opcode spellings must stay the same number or the
 * firmware's compile-time drift check fails and RELEASE is dropped. */
typedef char app_release_opcode_matches[
	(SDK_OP_APP_RELEASE == ZZAPP_OP_RELEASE &&
	 SDK_OP_APP_LIST == ZZAPP_OP_LIST) ? 1 : -1];
