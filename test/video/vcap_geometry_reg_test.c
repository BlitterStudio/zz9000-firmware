/*
 * Host test for the dedicated capture-geometry register decode.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>

#include "vcap_geometry_reg.h"
#include "zz_config.h"
#include "zz_regs.h"

static unsigned failures;

#define CHECK(expr) do { \
	if (!(expr)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
		++failures; \
	} \
} while (0)

int main(void)
{
	struct vcap_geometry_reg reg = { 0, 0 };
	uint16_t width = 0xffffU;

	/* ABI pins: drivers hard-code these values. */
	CHECK(REG_ZZ_VCAP_GEOMETRY_WIDTH == 0xA8);
	CHECK(REG_ZZ_VCAP_GEOMETRY_COMMIT == 0xAA);
	CHECK(ZZ_CONFIG_KEY_VCAP_GEOMETRY_REGS == 36);
	CHECK(VCAP_GEOMETRY_REGS_ABI == 1U);

	/* A commit with nothing staged is rejected. */
	CHECK(!vcap_geometry_reg_commit(&reg, &width));
	CHECK(width == 0xffffU);

	/* Stage then commit hands over the pair once. */
	vcap_geometry_reg_stage(&reg, 640);
	CHECK(vcap_geometry_reg_commit(&reg, &width));
	CHECK(width == 640);
	width = 0xffffU;
	CHECK(!vcap_geometry_reg_commit(&reg, &width));
	CHECK(width == 0xffffU);

	/* A later stage replaces an uncommitted one: the commit takes the
	 * latest width, never the earlier value. */
	vcap_geometry_reg_stage(&reg, 704);
	vcap_geometry_reg_stage(&reg, 720);
	CHECK(vcap_geometry_reg_commit(&reg, &width));
	CHECK(width == 720);

	/* 0 (automatic) is a real staged value, not "nothing staged". */
	vcap_geometry_reg_stage(&reg, 0);
	width = 0xffffU;
	CHECK(vcap_geometry_reg_commit(&reg, &width));
	CHECK(width == 0);
	CHECK(!vcap_geometry_reg_commit(&reg, &width));

	if (failures) {
		printf("vcap_geometry_reg_test: %u failure(s)\n", failures);
		return 1;
	}
	printf("vcap_geometry_reg_test: OK\n");
	return 0;
}
