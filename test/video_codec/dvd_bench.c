#include "sdk_video_backend.h"
#include "sdk_video_yuy2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "dvd_pal_25p_fixture.inc"
#include "dvd_pal_tff_fixture.inc"
#include "dvd_ntsc_30_fixture.inc"
#include "dvd_ntsc_film_32_fixture.inc"
#include "dvd_ac3_51_fixture.inc"

static double now_sec(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec * 1e-6;
}

static void bench(const char *label, const uint8_t *stream, uint32_t len, uint32_t audio_codec, int iters) {
	const struct SDKVideoDecoderOps *ops = sdk_video_mpeg2_backend_ops();
	uint8_t pcm_ring[64 * 1024];
	uint32_t total_frames = 0;
	uint64_t total_pcm = 0;

	double t0 = now_sec();

	for (int it = 0; it < iters; it++) {
		void *dec = ops->create(0U);
		if (audio_codec != SDK_VIDEO_MEDIA_AUDIO_NONE) {
			struct SDKVideoMediaConfig cfg = {0};
			cfg.audio_codec = audio_codec;
			cfg.pcm_ring = pcm_ring;
			cfg.pcm_ring_capacity = sizeof(pcm_ring);
			cfg.pcm_low_water_bytes = 4096;
			cfg.pcm_high_water_bytes = sizeof(pcm_ring) - 4096;
			ops->configure_media(dec, &cfg);
		}
		uint32_t accepted = 0;
		ops->write(dec, stream, len, 0, &accepted);
		ops->write(dec, NULL, 0, 1, &accepted);

		struct SDKVideoDecodedFrame frame;
		while (ops->decode(dec, &frame) == SDK_VIDEO_BACKEND_FRAME) {
			total_frames++;
		}
		if (audio_codec != SDK_VIDEO_MEDIA_AUDIO_NONE) {
			struct SDKVideoMediaInfo ainfo;
			if (ops->get_media_info(dec, &ainfo))
				total_pcm += ainfo.pcm_produced;
		}
		ops->destroy(dec);
	}
	double t1 = now_sec();
	double sec = t1 - t0;
	double fps = (double)total_frames / sec;
	double mbps = (double)len * iters / (1024.0 * 1024.0) / sec;
	printf("%-32s | %4d iters | %6.3f s | %7.1f fps | %6.2f MiB/s | %6u frames | %8llu bytes PCM\n",
	       label, iters, sec, fps, mbps, total_frames, (unsigned long long)total_pcm);
}

int main(void) {
	printf("=== Software Decode Throughput (NON-PHYSICAL HOST TEST) ===\n");
	printf("Target cell                      | Iters      | Time     | Decode rate | Throughput  | Decoded frames | Decoded audio\n");
	printf("---------------------------------+------------+----------+-------------+-------------+----------------+-----------------\n");
	bench("PAL 720x576 25p (MP2)", zz9k_dvd_pal_25p_fixture, zz9k_dvd_pal_25p_fixture_len, SDK_VIDEO_MEDIA_AUDIO_MP2, 500);
	bench("PAL 720x576 50i TFF (AC-3)", zz9k_dvd_pal_tff_fixture, zz9k_dvd_pal_tff_fixture_len, SDK_VIDEO_MEDIA_AUDIO_AC3, 500);
	bench("NTSC 720x480 30p (LPCM)", zz9k_dvd_ntsc_30_fixture, zz9k_dvd_ntsc_30_fixture_len, SDK_VIDEO_MEDIA_AUDIO_LPCM, 500);
	bench("NTSC film 720x480 24p 3:2", zz9k_dvd_ntsc_film_32_fixture, zz9k_dvd_ntsc_film_32_fixture_len, SDK_VIDEO_MEDIA_AUDIO_NONE, 500);
	bench("AC-3 5.1 Downmix to Stereo", zz9k_dvd_ac3_51_fixture, zz9k_dvd_ac3_51_fixture_len, SDK_VIDEO_MEDIA_AUDIO_AC3, 1000);
	return 0;
}
