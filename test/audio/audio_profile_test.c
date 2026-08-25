#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adau.h"
#include "adau_PARAM.h"
#include "audio_dsp_gain.h"
#include "ax.h"
#include "sdk_crypto.h"

#define CHECK(condition)                                                     \
	do {                                                                  \
		if (!(condition)) {                                             \
			fprintf(stderr, "%s:%d: check failed: %s\n",            \
					__FILE__, __LINE__, #condition);             \
			exit(1);                                              \
		}                                                             \
	} while (0)

static const char normal_project_sha256[] =
	"df62c9f36c1675bc959c94b0cbfb546d"
	"f71921a482df201d361889d517ba2952";
static const char normal_program_sha256[] =
	"bda1406175755779e630fec41863a1897"
	"509199c9bd42e2ae51620dc75e1a80c";
static const char normal_parameter_sha256[] =
	"979c11315dfc59b85d86fa82cd88f34"
	"597f1df49f39ee14ff18b9acb4769d4f0";

static int digest_matches_hex(const uint8_t *data, size_t length,
		const char expected[65])
{
	static const char hex[] = "0123456789abcdef";
	uint8_t digest[SDK_SHA256_DIGEST_SIZE];
	char actual[SDK_SHA256_DIGEST_SIZE * 2U + 1U];
	size_t i;

	CHECK(length <= UINT32_MAX);
	sdk_sha256(data, (uint32_t)length, digest);
	for (i = 0; i < SDK_SHA256_DIGEST_SIZE; ++i) {
		actual[i * 2U] = hex[digest[i] >> 4];
		actual[i * 2U + 1U] = hex[digest[i] & 0x0fU];
	}
	actual[sizeof(actual) - 1U] = '\0';
	return strcmp(actual, expected) == 0;
}

static int file_digest_matches(const char *repository_path,
		const char expected[65])
{
	char test_path[512];
	uint8_t *data;
	long length;
	FILE *file;
	int matches;

	CHECK(snprintf(test_path, sizeof(test_path), "../../%s",
			repository_path) > 0);
	file = fopen(test_path, "rb");
	CHECK(file != NULL);
	CHECK(fseek(file, 0, SEEK_END) == 0);
	length = ftell(file);
	CHECK(length >= 0);
	CHECK(fseek(file, 0, SEEK_SET) == 0);
	data = malloc((size_t)length);
	CHECK(data != NULL);
	CHECK(fread(data, 1U, (size_t)length, file) == (size_t)length);
	CHECK(fclose(file) == 0);
	matches = digest_matches_hex(data, (size_t)length, expected);
	free(data);
	return matches;
}

static void test_normal_image_identity(void)
{
	CHECK(PROGRAM_SIZE_NORMAL_ADC_IC_1 == 5120U);
	CHECK(PARAM_SIZE_NORMAL_ADC_IC_1 == 4096U);
	CHECK(sizeof(Program_Data_Normal_ADC_IC_1) ==
			PROGRAM_SIZE_NORMAL_ADC_IC_1);
	CHECK(sizeof(Param_Data_Normal_ADC_IC_1) ==
			PARAM_SIZE_NORMAL_ADC_IC_1);
	CHECK(digest_matches_hex(Program_Data_Normal_ADC_IC_1,
			sizeof(Program_Data_Normal_ADC_IC_1),
			normal_program_sha256));
	CHECK(digest_matches_hex(Param_Data_Normal_ADC_IC_1,
			sizeof(Param_Data_Normal_ADC_IC_1),
			normal_parameter_sha256));
	CHECK(file_digest_matches(
			"zz9000ax/zz9000ax-mix1-lowpass-eq.dspproj",
			normal_project_sha256));
}

static void test_production_transport_contract(void)
{
	CHECK(ZZ_AUDIO_CAPTURE_CANDIDATE_BUILD_ID == 0xa205U);
	CHECK(ZZ_AUDIO_CODEC_SERIAL_TDM8_SLOT01 == 0x0c22U);
	CHECK(ZZ_AUDIO_CODEC_MP_CONTROL == 0x444444U);
	CHECK(ZZ_AUDIO_CODEC_CORE_LOADING == 0x0018U);
	CHECK(ZZ_AUDIO_CODEC_CORE_RUNNING == 0x001cU);
	CHECK(ZZ_NUM_AUDIO_PARAMS == 23U);
}

static void test_parameter_map(void)
{
	CHECK(MOD_STMIXER1_ALG0_STAGE0_VOLUME_ADDR == 0U);
	CHECK(MOD_STMIXER1_ALG0_STAGE1_VOLUME_ADDR == 1U);
	CHECK(MOD_GENFILTER1_ALG0_STAGE0_B0_ADDR == 2U);
	CHECK(MOD_GENFILTER1_ALG0_STAGE0_A2_ADDR == 6U);
	CHECK(MOD_PREFACTOR_ALG0_GAIN1940ALGNS3_ADDR == 7U);
	CHECK(MOD_PREFACTOR_ALG1_GAIN1940ALGNS4_ADDR == 8U);
	CHECK(MOD_EQUALIZER_ALG0_STAGE0_B0_ADDR == 9U);
	CHECK(MOD_VOLUME_ALG0_GAIN1940ALGNS1_ADDR == 59U);
	CHECK(MOD_VOLUME_ALG1_GAIN1940ALGNS2_ADDR == 60U);
}

/*
 * SigmaStudio assigns parameters in compiled signal-flow order. Pinning
 * the mixer before every scene block prevents the source graph from
 * regressing to the old AX-only chain where Paula joined after volume.
 * The image hashes above pin the complete compiled topology.
 */
static void test_master_chain_parameter_order(void)
{
	CHECK(MOD_STMIXER1_ALG0_STAGE1_VOLUME_ADDR <
			MOD_GENFILTER1_ALG0_STAGE0_B0_ADDR);
	CHECK(MOD_GENFILTER1_ALG0_STAGE0_A2_ADDR <
			MOD_PREFACTOR_ALG0_GAIN1940ALGNS3_ADDR);
	CHECK(MOD_PREFACTOR_ALG1_GAIN1940ALGNS4_ADDR <
			MOD_EQUALIZER_ALG0_STAGE0_B0_ADDR);
	CHECK(MOD_EQUALIZER_ALG0_STAGE9_A1_ADDR <
			MOD_VOLUME_ALG0_GAIN1940ALGNS1_ADDR);
}

static int gain_matches(double actual, double expected)
{
	return fabs(actual - expected) < 1.0e-12;
}

static void test_prefactor_gain_contract(void)
{
	const double minus_12_db = 0.2511886431509580;
	const double plus_12_db = 3.9810717055349722;

	CHECK(audio_adau_prefactor_gain(50) == 1.0);
	CHECK(gain_matches(audio_adau_prefactor_gain(0), minus_12_db));
	CHECK(gain_matches(audio_adau_prefactor_gain(100), plus_12_db));
	CHECK(gain_matches(audio_adau_prefactor_gain(-1), minus_12_db));
	CHECK(gain_matches(audio_adau_prefactor_gain(101), plus_12_db));
	CHECK(MOD_PREFACTOR_ALG0_GAIN1940ALGNS3_FIXPT == 0x00800000U);
	CHECK(MOD_PREFACTOR_ALG1_GAIN1940ALGNS4_FIXPT == 0x00800000U);
}

static void test_readback_comparison(void)
{
	static const uint8_t expected[] = { 0x00, 0x12, 0x34, 0x56 };
	uint8_t actual[] = { 0x00, 0x12, 0x34, 0x56 };

	CHECK(audio_adau_readback_matches(expected, actual,
			sizeof(expected)));
	actual[3] ^= 1U;
	CHECK(!audio_adau_readback_matches(expected, actual,
			sizeof(expected)));
	CHECK(!audio_adau_readback_matches(NULL, actual,
			sizeof(expected)));
}

int main(void)
{
	test_normal_image_identity();
	test_production_transport_contract();
	test_parameter_map();
	test_prefactor_gain_contract();
	test_master_chain_parameter_order();
	test_readback_comparison();
	puts("audio production profile tests passed");
	return 0;
}
