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
	{ 0x0500U, 28U, "audio" },
	{ 0x0800U, 5U, "crypto" },
	{ 0x0900U, 4U, "diag" },
	{ 0x0b00U, 15U, "video" },	/* 0x0b00..0x0b0e: OUTPUT_CONFIG is the last */
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
		/* The static audio descriptor never carries the voice flags:
		 * they depend on the bus (checked after the loop). */
		if (expected[i].id == SDK_SERVICE_AUDIO &&
		    (service->flags & (SDK_SERVICE_FLAG_AUDIO_VOICE |
				       SDK_SERVICE_FLAG_AUDIO_VOICE_STEREO)) != 0U) {
			puts("service discovery: static audio flags carry voice");
			return 1;
		}
	}

	/* The voice engine and its stereo formats are advertised on Zorro III
	 * only; the other flag that follows the bus is the memory media input
	 * flag. */
	for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
		uint32_t z3_flags, z2_flags;

		service = find_service(expected[i].id);
		sdk_service_write_info(&info, service, service->capability_bits,
			service->flags | sdk_service_bus_flags(service->service_id, 1),
			48U);
		z3_flags = read_be32(info.flags);
		sdk_service_write_info(&info, service, service->capability_bits,
			service->flags | sdk_service_bus_flags(service->service_id, 0),
			48U);
		z2_flags = read_be32(info.flags);
		if (z2_flags != service->flags) {
			printf("service discovery: Zorro II changed flags of 0x%04x\n",
			       (unsigned)expected[i].id);
			return 1;
		}
		if (expected[i].id == SDK_SERVICE_AUDIO) {
			const uint32_t voice = SDK_SERVICE_FLAG_AUDIO_VOICE |
				SDK_SERVICE_FLAG_AUDIO_VOICE_STEREO;

			if ((z3_flags & voice) != voice ||
			    (z2_flags & voice) != 0U ||
			    (z3_flags & ~voice) != service->flags) {
				puts("service discovery: voice flags are not Zorro III only");
				return 1;
			}
		} else if (expected[i].id == SDK_SERVICE_MEMORY) {
			if (z3_flags != (service->flags |
					 SDK_SERVICE_FLAG_MEMORY_MEDIA_INPUT)) {
				puts("service discovery: memory media input flag wrong");
				return 1;
			}
		} else if (expected[i].id == SDK_SERVICE_SURFACE) {
			/* The palette query flag follows the bus: the CLUT shadow
			 * is filled on Zorro III only. */
			if ((service->flags &
			     SDK_SERVICE_FLAG_SURFACE_PALETTE_QUERY) != 0U ||
			    (z2_flags & SDK_SERVICE_FLAG_SURFACE_PALETTE_QUERY) != 0U ||
			    z3_flags != (service->flags |
					 SDK_SERVICE_FLAG_SURFACE_PALETTE_QUERY)) {
				puts("service discovery: palette query flag is not Zorro III only");
				return 1;
			}
		} else if (z3_flags != service->flags) {
			printf("service discovery: Zorro III changed flags of 0x%04x\n",
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

/* The mailbox router forwards the whole app opcode range through
 * handle_app_op; both opcode spellings must stay the same number or the
 * firmware's compile-time drift check fails and RELEASE is dropped. */
typedef char app_release_opcode_matches[
	(SDK_OP_APP_RELEASE == ZZAPP_OP_RELEASE &&
	 SDK_OP_APP_LIST == ZZAPP_OP_LIST) ? 1 : -1];
