/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_fabric_codec.h"
#include "fabric_test_common.h"
#include "flac_fixtures.h"
#include "host_card_pool.h"
#include "host_runtime.h"
#include "zz9k/abi.h"

#define ABI_MATCH(name) \
	_Static_assert(SDK_AUDIO_RING_##name == ZZ9K_AUDIO_RING_##name, #name)
ABI_MATCH(ACQUIRE_FLAG_CODEC);
ABI_MATCH(CONTRACT_CODEC_STREAM);
ABI_MATCH(CODEC_PCM);
ABI_MATCH(CODEC_MP3);
ABI_MATCH(CODEC_FLAC);
ABI_MATCH(CODEC_VORBIS);
ABI_MATCH(CODEC_NONE);
ABI_MATCH(STREAM_FLAG_APPEND);
ABI_MATCH(STREAM_FLAG_END);
ABI_MATCH(DECODE_IDLE);
ABI_MATCH(DECODE_RUNNING);
ABI_MATCH(DECODE_DRAINED);
ABI_MATCH(DECODE_ERROR);
ABI_MATCH(DECODE_ERR_CORRUPT);
#undef ABI_MATCH

#define SLOT AUDIO_FABRIC_SLOT_MAILBOX
#define FW_OFFSET SDK_AUDIO_RING_CONTROL_LINE_SIZE
#define CODEC_FLAGS (SDK_AUDIO_RING_ACQUIRE_FLAG_CODEC | SDK_AUDIO_RING_ACQUIRE_FLAG_IDLE_PAUSED)

static struct SDKAudioRingProducerLine *const producer =
	(struct SDKAudioRingProducerLine *)g_ring_control_a;
static const struct SDKAudioRingFirmwareLine *const fw =
	(const struct SDKAudioRingFirmwareLine *)(g_ring_control_a + FW_OFFSET);
static uint32_t generation, heartbeat;

static void put32(uint8_t *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (uint8_t)(value >> (24U - 8U * i));
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t consumed(void)
{
	return ((uint64_t)get32(fw->consumed_cursor_hi) << 32) |
	       get32(fw->consumed_cursor_lo);
}

static void publish(uint32_t serial, uint32_t codec, uint64_t start,
	uint64_t write, uint32_t flags, uint32_t stream_flags, uint32_t gain)
{
	uint32_t seq = get32(producer->sequence) + 2U;

	put32(producer->sequence, seq | 1U);
	put32(producer->generation, generation);
	put32(producer->write_cursor_hi, (uint32_t)(write >> 32));
	put32(producer->write_cursor_lo, (uint32_t)write);
	put32(producer->heartbeat, ++heartbeat);
	put32(producer->flags, flags);
	put32(producer->stream_seq, serial);
	put32(producer->stream_codec, codec);
	put32(producer->stream_start_hi, (uint32_t)(start >> 32));
	put32(producer->stream_start_lo, (uint32_t)start);
	put32(producer->stream_flags, stream_flags);
	put32(producer->stream_gain, gain);
	put32(producer->sequence, seq);
}

static void begin(uint32_t rate, uint32_t flags)
{
	struct audio_fabric_ring_grant g;

	fabric_reset_state();
	memset(g_fabric_tx, 0, sizeof(g_fabric_tx));
	g_dma_count = 0U;
	heartbeat = 0U;
	check(host_runtime_live_blocks() == 0U && host_card_pool_used() == 0U,
	      "no heap left from previous lease", NULL);
	check(audio_fabric_ring_acquire(SLOT, SDK_AUDIO_METER_IDENTITY_SDK_STREAM,
		128U, rate, flags, &g) == AUDIO_FABRIC_LEASE_OK, "acquire", NULL);
	generation = g.generation;
}

static void write_bytes(uint64_t start, const uint8_t *data, uint32_t count)
{
	uint32_t off = (uint32_t)(start % RING_TEST_CAPACITY);
	uint32_t first = RING_TEST_CAPACITY - off;

	if (first > count)
		first = count;
	memcpy(g_ring_pcm_a + off, data, first);
	if (count > first)
		memcpy(g_ring_pcm_a, data + first, count - first);
}

/* Read the period the model DMA is playing, then retire it. Give the main
 * loop several calls between formatter interrupts, as on the card. */
static unsigned play_tick(void)
{
	const uint8_t *period = g_fabric_tx + g_dma_count % AUDIO_TX_BUFFER_SIZE;
	unsigned sounded = 0U;

	for (uint32_t i = 0; i < TICK_BYTES; ++i)
		sounded |= period[i] != 0U;
	dma_tick(1);
	audio_fabric_isr();
	for (unsigned i = 0; i < 8; i++)
		audio_fabric_lease_poll();
	return sounded;
}

static void decode_file(uint32_t codec, const uint8_t *data, uint32_t size,
	uint32_t rate, uint32_t channels, uint32_t gain)
{
	const uint64_t start = RING_TEST_CAPACITY - 7U;
	uint32_t offset = 0U;
	unsigned sounded = 0U;
	unsigned last_sounded = 0U;
	unsigned i;

	begin(0U, CODEC_FLAGS);
	for (i = 1U; i < 175U; i++) {
		uint32_t chunk = size - offset;

		if (chunk > 997U)
			chunk = 997U;
		if (chunk != 0U)
			write_bytes(start + offset, data + offset, chunk);
		offset += chunk;
		publish(1U, codec, start, start + offset, 0U,
			offset == size ? SDK_AUDIO_RING_STREAM_FLAG_END : 0U, gain);
		if (play_tick()) {
			sounded++;
			last_sounded = i;
		}
		if (get32(fw->decode_state) == SDK_AUDIO_RING_DECODE_DRAINED ||
		    get32(fw->decode_state) == SDK_AUDIO_RING_DECODE_ERROR)
			break;
	}
	check(get32(fw->status) == SDK_AUDIO_RING_STATUS_OK &&
	      get32(fw->stream_seq) == 1U && get32(fw->decode_state) == SDK_AUDIO_RING_DECODE_DRAINED,
	      "codec drains", fmt("codec %u state %u error %u", codec,
		get32(fw->decode_state), get32(fw->decode_error)));
	check(consumed() == start + size, "input credit reaches EOS", NULL);
	check(get32(fw->decoded_rate) == rate && get32(fw->decoded_channels) == channels,
	      "native source status", fmt("codec %u: %u Hz, %u channels", codec,
		get32(fw->decoded_rate), get32(fw->decoded_channels)));
	check(gain ? sounded > 1U && i - last_sounded <= 3U : sounded == 0U,
	      "DMA playout before DRAINED (including mute)", fmt("%u ticks, %u sounded, last %u", i, sounded, last_sounded));
	check(audio_fabric_ring_release(SLOT, generation) == AUDIO_FABRIC_LEASE_OK,
	      "released codec lease", NULL);
}

static void test_loop(void)
{
	/* Two short PCM streams with different signs. APPEND must play both
	 * in order, even if the first is still decoding when APPEND lands. */
	static int16_t a[(TICK_BYTES / 2U) * 2U];
	static int16_t b[(TICK_BYTES / 2U) * 2U];
	unsigned before = 0, after = 0, gaps = 0;
	const uint32_t tail = SDK_AUDIO_RING_STREAM_FLAG_END;

	begin(0U, CODEC_FLAGS);
	for (uint32_t i = 0; i < sizeof(a) / sizeof(a[0]); i++) {
		a[i] = 1000;
		b[i] = -1000;
	}
	write_bytes(0U, (const uint8_t *)a, sizeof(a));
	publish(1U, SDK_AUDIO_RING_CODEC_PCM, 0U, sizeof(a), 0U, 0U, 32767U);
	audio_fabric_isr();
	check(get32(fw->stream_seq) == 1U, "start acknowledged", NULL);
	write_bytes(sizeof(a), (const uint8_t *)b, sizeof(b));
	for (unsigned i = 0; i < 24U; i++) {
		int16_t sample;

		memcpy(&sample, g_fabric_tx + g_dma_count % AUDIO_TX_BUFFER_SIZE + 128U, 2U);
		if (sample > 500) {
			check(after == 0U, "old audio plays before loop restart", NULL);
			before++;
		} else if (sample < -500) {
			after++;
		} else if (before != 0U && after == 0U) {
			gaps++;
		}
		publish(2U, SDK_AUDIO_RING_CODEC_PCM, sizeof(a), sizeof(a) + sizeof(b), 0U,
			tail | SDK_AUDIO_RING_STREAM_FLAG_APPEND, 32767U);
		play_tick();
		if (get32(fw->decode_state) == SDK_AUDIO_RING_DECODE_DRAINED)
			break;
	}
	check(get32(fw->stream_seq) == 2U && get32(fw->decode_state) == SDK_AUDIO_RING_DECODE_DRAINED,
	      "appended stream promoted and drained", NULL);
	check(before >= 2U && after >= 2U && gaps <= 1U,
	      "append preserves queued audio", fmt("%u + periods, %u - periods, %u gap", before, after, gaps));
}

static void test_flush(uint32_t flags, uint32_t rate)
{
	static int16_t pcm[12U * TICK_BYTES / 2U];
	uint32_t pos;

	begin(rate, flags);
	for (uint32_t i = 0; i < sizeof(pcm) / sizeof(pcm[0]); i++)
		pcm[i] = 7000;
	write_bytes(0U, (const uint8_t *)pcm, sizeof(pcm));
	for (unsigned i = 0; i < 6U; i++) {
		publish(1U, SDK_AUDIO_RING_CODEC_PCM, 0U, sizeof(pcm), 0U, 0U, 32767U);
		play_tick();
	}
	check(count_nonzero_periods(g_fabric_tx) >= 3U, "old audio queued", NULL);
	publish(2U, SDK_AUDIO_RING_CODEC_NONE, sizeof(pcm), sizeof(pcm),
		SDK_AUDIO_RING_PRODUCER_FLAG_PAUSED, 0U, 32767U);
	play_tick();
	pos = g_dma_count % AUDIO_TX_BUFFER_SIZE;
	for (unsigned i = 1; i < AUDIO_FABRIC_FILL_AHEAD_DEFAULT_PERIODS; i++) {
		const uint8_t *period = g_fabric_tx + (pos + i * TICK_BYTES) % AUDIO_TX_BUFFER_SIZE;
		unsigned any = 0U;

		for (uint32_t j = 0; j < TICK_BYTES; j++)
			any |= period[j];
		check(any == 0U, "flush drops queued periods", fmt("offset %u flags %u", i, flags));
	}
	check(get32(fw->stream_seq) == 2U && consumed() == sizeof(pcm) &&
	      get32(fw->decode_state) == SDK_AUDIO_RING_DECODE_IDLE,
	      "flush acknowledged and credited", NULL);
	for (unsigned i = 0; i < 10U; i++)
		play_tick();
	check(get32(fw->status) == SDK_AUDIO_RING_STATUS_OK,
	      "idle paused lease does not expire", NULL);
}

static void test_error(int eof_before_append)
{
	/* Correct magic, short STREAMINFO. Exercise both sides of the race
	 * between the first decoder error and the ISR accepting APPEND. */
	static const uint8_t broken[] = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34};

	begin(0U, CODEC_FLAGS);
	write_bytes(0U, broken, sizeof(broken));
	publish(1U, SDK_AUDIO_RING_CODEC_FLAC, 0U, sizeof(broken), 0U,
		eof_before_append ? SDK_AUDIO_RING_STREAM_FLAG_END : 0U, 32767U);
	audio_fabric_isr();
	if (eof_before_append)
		audio_fabric_lease_poll();
	publish(2U, SDK_AUDIO_RING_CODEC_FLAC, sizeof(broken), sizeof(broken), 0U,
		SDK_AUDIO_RING_STREAM_FLAG_APPEND, 32767U);
	for (unsigned i = 0; i < 8U; i++) {
		publish(2U, SDK_AUDIO_RING_CODEC_FLAC, sizeof(broken), sizeof(broken), 0U,
			SDK_AUDIO_RING_STREAM_FLAG_APPEND, 32767U);
		play_tick();
	}
	check(get32(fw->status) == SDK_AUDIO_RING_STATUS_OK &&
	      get32(fw->stream_seq) == 2U && get32(fw->decode_state) == SDK_AUDIO_RING_DECODE_ERROR &&
	      get32(fw->decode_error) == SDK_AUDIO_RING_DECODE_ERR_CORRUPT,
	      "corrupt stream fails the appended serial without revoking the lease",
	      fmt("state %u err %u", get32(fw->decode_state), get32(fw->decode_error)));
	write_bytes(sizeof(broken), fx_stereo16_flac, sizeof(fx_stereo16_flac));
	for (unsigned i = 0; i < 16U; i++) {
		publish(3U, SDK_AUDIO_RING_CODEC_FLAC, sizeof(broken),
			sizeof(broken) + sizeof(fx_stereo16_flac), 0U,
			SDK_AUDIO_RING_STREAM_FLAG_END, 32767U);
		play_tick();
	}
	check(get32(fw->stream_seq) == 3U && get32(fw->decode_state) == SDK_AUDIO_RING_DECODE_DRAINED,
	      "can play again after error", NULL);
}

static uint32_t load(const char *path, uint8_t *data, uint32_t capacity)
{
	FILE *f = fopen(path, "rb");
	uint32_t n;

	if (!f) {
		perror(path);
		exit(1);
	}
	n = (uint32_t)fread(data, 1U, capacity, f);
	if (n == 0U || n == capacity || ferror(f)) {
		fprintf(stderr, "invalid fixture: %s\n", path);
		exit(1);
	}
	fclose(f);
	return n;
}

int main(void)
{
	static uint8_t data[RING_TEST_CAPACITY];
	uint32_t len;

	host_runtime_reset();
	host_card_pool_reset(8U * 1024U * 1024U);
	audio_fabric_codec_register(&audio_fabric_stream_codec_ops);
	len = load("fixtures/tone.mp3", data, sizeof(data));
	decode_file(SDK_AUDIO_RING_CODEC_MP3, data, len, 44100U, 2U, 32767U);
	decode_file(SDK_AUDIO_RING_CODEC_FLAC, fx_mono8_flac, sizeof(fx_mono8_flac), 22050U, 1U, 32767U);
	len = load("../vorbis/fixtures/stereo44k.ogg", data, sizeof(data));
	decode_file(SDK_AUDIO_RING_CODEC_VORBIS, data, len, 44100U, 2U, 32767U);
	decode_file(SDK_AUDIO_RING_CODEC_VORBIS, data, len, 44100U, 2U, 0U);
	test_loop();
	test_flush(CODEC_FLAGS, 0U);
	test_flush(SDK_AUDIO_RING_ACQUIRE_FLAG_IDLE_PAUSED, 0U);
	test_flush(SDK_AUDIO_RING_ACQUIRE_FLAG_IDLE_PAUSED | SDK_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE, 44100U);
	test_error(0);
	test_error(1);
	audio_fabric_reset();
	check(host_runtime_live_blocks() == 0U && host_card_pool_used() == 0U,
	      "reset reaps decoder heap", NULL);
	if (failures == 0)
		puts("audio codec ring tests passed");
	return failures != 0;
}
