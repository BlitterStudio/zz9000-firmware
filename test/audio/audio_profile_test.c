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
	"61f85f6c77638b0c8be2c9488a4212ad"
	"8f63b5646e4f5bc592bdf9bd568a917b";
static const char normal_program_sha256[] =
	"f92a1dd782cae39edb826c4b16d2d56b"
	"9e93782f8d1ad8b1134186d6723baaa3";
static const char normal_parameter_sha256[] =
	"b65b282b9fc09fadc4238db036f07536"
	"3facc1d0c98fe4960d3f2b6d9e3bd59f";

static int digest_matches_hex(const uint8_t *data, size_t length,
		const char expected[65])
{
	static const char hex[] = "0123456789abcdef";
	uint8_t digest[SDK_SHA256_DIGEST_SIZE];
	uint8_t incremental[SDK_SHA256_DIGEST_SIZE];
	sdk_sha256_context_t context;
	char actual[SDK_SHA256_DIGEST_SIZE * 2U + 1U];
	size_t offset = 0U;
	size_t i;

	CHECK(length <= UINT32_MAX);
	sdk_sha256(data, (uint32_t)length, digest);
	sdk_sha256_init(&context);
	while (offset < length) {
		size_t bytes = length - offset;

		if (bytes > 257U)
			bytes = 257U;
		sdk_sha256_update(&context, data + offset, (uint32_t)bytes);
		offset += bytes;
	}
	sdk_sha256_final(&context, incremental);
	CHECK(memcmp(digest, incremental, sizeof(digest)) == 0);
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
		"dsp/limiter-postvolume.dspproj",
			normal_project_sha256));
}

static void test_production_transport_contract(void)
{
	CHECK(ZZ_AUDIO_CAPTURE_CANDIDATE_BUILD_ID == 0xa206U);
	CHECK(ZZ_AUDIO_CODEC_SERIAL_TDM8_SLOT01 == 0x0c22U);
	CHECK(ZZ_AUDIO_CODEC_MP_CONTROL == 0x444444U);
	CHECK(ZZ_AUDIO_CODEC_CORE_LOADING == 0x0018U);
	CHECK(ZZ_AUDIO_CODEC_CORE_RUNNING == 0x001cU);
	CHECK(ZZ_NUM_AUDIO_PARAMS == 23U);
}

static void test_parameter_map(void)
{
	CHECK(MOD_LIMITTHRESHOLD_DCINPALG1_ADDR == 0U);
	CHECK(MOD_STMIXER1_ALG0_STAGE0_VOLUME_ADDR == 2U);
	CHECK(MOD_STMIXER1_ALG0_STAGE1_VOLUME_ADDR == 3U);
	CHECK(MOD_SEPMIXL_ALG0_STAGE0_VOLONE_ADDR == 4U);
	CHECK(MOD_SEPMIXL_ALG0_STAGE0_VOLTWO_ADDR == 5U);
	CHECK(MOD_SEPMIXR_ALG0_STAGE0_VOLONE_ADDR == 6U);
	CHECK(MOD_SEPMIXR_ALG0_STAGE0_VOLTWO_ADDR == 7U);
	CHECK(MOD_GENFILTER1_ALG0_STAGE0_B0_ADDR == 8U);
	CHECK(MOD_GENFILTER1_ALG0_STAGE0_A2_ADDR == 12U);
	CHECK(MOD_PREFACTOR_ALG0_GAIN1940ALGNS3_ADDR == 13U);
	CHECK(MOD_PREFACTOR_ALG1_GAIN1940ALGNS4_ADDR == 14U);
	CHECK(MOD_EQUALIZER_ALG0_STAGE0_B0_ADDR == 15U);
	CHECK(MOD_VOLUME_ALG0_GAIN1940ALGNS1_ADDR == 65U);
	CHECK(MOD_VOLUME_ALG1_GAIN1940ALGNS2_ADDR == 66U);
}

/*
 * SigmaStudio assigns parameters in compiled signal-flow order. Pinning
 * the mixer before every scene block prevents the source graph from
 * regressing to the old AX-only chain where Paula joined after volume;
 * the limiter threshold and decay sit ahead of the master chain at
 * addresses 0 and 1. The image hashes above pin the complete compiled
 * topology.
 */
static void test_master_chain_parameter_order(void)
{
	CHECK(MOD_STMIXER1_ALG0_STAGE1_VOLUME_ADDR <
			MOD_SEPMIXL_ALG0_STAGE0_VOLONE_ADDR);
	CHECK(MOD_SEPMIXR_ALG0_STAGE0_VOLTWO_ADDR <
			MOD_GENFILTER1_ALG0_STAGE0_B0_ADDR);
	CHECK(MOD_GENFILTER1_ALG0_STAGE0_A2_ADDR <
			MOD_PREFACTOR_ALG0_GAIN1940ALGNS3_ADDR);
	CHECK(MOD_PREFACTOR_ALG1_GAIN1940ALGNS4_ADDR <
			MOD_EQUALIZER_ALG0_STAGE0_B0_ADDR);
	CHECK(MOD_EQUALIZER_ALG0_STAGE9_A1_ADDR <
			MOD_VOLUME_ALG0_GAIN1940ALGNS1_ADDR);
}

/*
 * Stereo separation (issue #255): the SepMixL/SepMixR crossfeed sits
 * between the source mixer and the master filter. ax.c writes the four
 * words as one contiguous group in the order own L, cross L, own R,
 * cross R, so pin the addresses, the shipped defaults (identity own,
 * the SigmaStudio knob floor -30 dB cross) and the compiled data flow:
 * the image must feed each output from both channels.
 */
static uint32_t image_word(unsigned address)
{
	const uint8_t *w = &Param_Data_Normal_ADC_IC_1[address * 4U];

	return ((uint32_t)w[0] << 24) | ((uint32_t)w[1] << 16) |
		((uint32_t)w[2] << 8) | (uint32_t)w[3];
}

/*
 * MAC instruction layout of the ADAU1701 program words as emitted by
 * SigmaStudio (5 bytes): bytes 0..1 are the data-RAM address shifted
 * left by three, byte 2 the parameter address, byte 3 is 0x20 or 0x22
 * (first / accumulating multiply) and byte 4 is 0x01. The divider
 * section reuses byte 2 in words whose first byte is not zero, so the
 * scan only accepts words with a zero first byte. Returns the data
 * address read by the one MAC that uses this parameter, or -1 when
 * there is not exactly one.
 */
static int mac_data_address(unsigned param)
{
	int found = -1;
	unsigned i;

	for (i = 0; i + 5U <= PROGRAM_SIZE_NORMAL_ADC_IC_1; i += 5U) {
		const uint8_t *w = &Program_Data_Normal_ADC_IC_1[i];

		if (w[0] != 0x00U || w[4] != 0x01U ||
				(w[3] != 0x20U && w[3] != 0x22U) ||
				w[2] != param)
			continue;
		if (found >= 0)
			return -1;
		found = (int)((((unsigned)w[0] << 8) | w[1]) >> 3);
	}
	return found;
}

static void test_separation_image(void)
{
	int left_own;
	int left_cross;
	int right_own;
	int right_cross;

	CHECK(image_word(MOD_SEPMIXL_ALG0_STAGE0_VOLONE_ADDR) ==
			AUDIO_SEPARATION_UNITY_Q23);
	CHECK(image_word(MOD_SEPMIXR_ALG0_STAGE0_VOLONE_ADDR) ==
			AUDIO_SEPARATION_UNITY_Q23);
	CHECK(image_word(MOD_SEPMIXL_ALG0_STAGE0_VOLTWO_ADDR) == 0x00040c37U);
	CHECK(image_word(MOD_SEPMIXR_ALG0_STAGE0_VOLTWO_ADDR) == 0x00040c37U);

	left_own = mac_data_address(MOD_SEPMIXL_ALG0_STAGE0_VOLONE_ADDR);
	left_cross = mac_data_address(MOD_SEPMIXL_ALG0_STAGE0_VOLTWO_ADDR);
	right_own = mac_data_address(MOD_SEPMIXR_ALG0_STAGE0_VOLONE_ADDR);
	right_cross = mac_data_address(MOD_SEPMIXR_ALG0_STAGE0_VOLTWO_ADDR);
	CHECK(left_own >= 0 && left_cross >= 0 &&
			right_own >= 0 && right_cross >= 0);
	/* L' = own*L + cross*R and R' = own*R + cross*L: the "own" gains
	 * read different channels, and each cross gain reads the channel
	 * the other output takes as its own. */
	CHECK(left_own != right_own);
	CHECK(left_cross == right_own);
	CHECK(right_cross == left_own);
}

static void test_separation_gains(void)
{
	unsigned percent;
	uint32_t previous = 0U;
	uint8_t bytes[4];

	CHECK(audio_separation_own_q23(100U) == AUDIO_SEPARATION_UNITY_Q23);
	CHECK(audio_separation_cross_q23(100U) == 0U);
	CHECK(audio_separation_own_q23(0U) == AUDIO_SEPARATION_UNITY_Q23 / 2U);
	CHECK(audio_separation_cross_q23(0U) == AUDIO_SEPARATION_UNITY_Q23 / 2U);
	/* 70 %: own 0.85, cross 0.15 of full scale. */
	CHECK(audio_separation_own_q23(70U) == 7130317U);
	CHECK(audio_separation_cross_q23(70U) == 1258291U);
	CHECK(audio_separation_own_q23(250U) == AUDIO_SEPARATION_UNITY_Q23);

	for (percent = 0U; percent <= AUDIO_SEPARATION_MAX; percent++) {
		uint32_t own = audio_separation_own_q23(percent);
		uint32_t cross = audio_separation_cross_q23(percent);
		int64_t full = (int64_t)AUDIO_SEPARATION_UNITY_Q23;
		int64_t out_l;
		int64_t out_r;

		/* The two gains always sum to unity, so a centred (mono)
		 * signal keeps its level... */
		CHECK(own + cross == AUDIO_SEPARATION_UNITY_Q23);
		CHECK(own >= previous);
		previous = own;
		out_l = ((int64_t)own * full + (int64_t)cross * full) >> 23;
		CHECK(out_l == full);
		/* ...and neither a hard-panned nor an anti-phase pair can
		 * exceed the larger input peak. */
		out_l = ((int64_t)own * full + (int64_t)cross * -full) >> 23;
		out_r = ((int64_t)own * -full + (int64_t)cross * full) >> 23;
		CHECK(out_l <= full && out_l >= -full);
		CHECK(out_r <= full && out_r >= -full);
		/* Hard-panned left only: the opposite side carries the
		 * crossfeed, mono at 0 %, silent at 100 %. */
		out_r = ((int64_t)cross * full) >> 23;
		CHECK(percent != 0U || out_r == ((int64_t)own * full) >> 23);
		CHECK(percent != 100U || out_r == 0);
	}

	audio_q23_to_param_bytes(0x00040c37U, bytes);
	CHECK(bytes[0] == 0x00U && bytes[1] == 0x04U &&
			bytes[2] == 0x0cU && bytes[3] == 0x37U);
	audio_q23_to_param_bytes(AUDIO_SEPARATION_UNITY_Q23, bytes);
	CHECK(bytes[0] == 0x00U && bytes[1] == 0x80U &&
			bytes[2] == 0x00U && bytes[3] == 0x00U);
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
	test_separation_image();
	test_separation_gains();
	test_readback_comparison();
	puts("audio production profile tests passed");
	return 0;
}
