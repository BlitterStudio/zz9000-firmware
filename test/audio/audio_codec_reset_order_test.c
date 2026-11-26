/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An Amiga reset takes the lent fast-RAM window back from the card pool and
 * hands it to the Amiga before sdk_mailbox_init() drops the audio leases. A
 * codec lease whose decoder memory is in that window must therefore be torn
 * down first: freeing libFLAC blocks after the revocation reads headers the
 * Amiga now owns and falls through to free() on pool memory.
 *
 * The test runs a FLAC and a Vorbis lease mid-stream over the real card pool
 * with a lent range, in the reset handler's order (quiesce, take back, drop
 * the leases) and in the wrong order. Freeing a pool pointer through free() is
 * caught with --wrap=free; the "Amiga" overwrites the revoked range. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "audio_fabric_codec.h"
#include "card_pool.h"
#include "fabric_test_common.h"
#include <sys/wait.h>
#include <unistd.h>
#include "flac_fixtures.h"
#include "host_card_pool.h"
#include "sdk_compression.h"

#define SLOT AUDIO_FABRIC_SLOT_MAILBOX
#define LENT_BYTES (4U << 20)
#define CODEC_FLAGS (SDK_AUDIO_RING_ACQUIRE_FLAG_CODEC | SDK_AUDIO_RING_ACQUIRE_FLAG_IDLE_PAUSED)

int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t s) { (void)s; }
void smp_raw_spin_lock(volatile uint32_t *w) { *w = 1U; }
int smp_raw_spin_try_lock(volatile uint32_t *w)
{
	if (*w)
		return 0;
	*w = 1U;
	return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *w) { *w = 0U; }
#define HOLD_BYTES 3000U

static struct SDKAudioRingProducerLine *const producer =
	(struct SDKAudioRingProducerLine *)g_ring_control_a;
static uint32_t generation, heartbeat;
static uint32_t lent_base;
static uint8_t g_vorbis[64U * 1024U];
static uint32_t g_vorbis_len;
static unsigned pool_frees;

void __real_free(void *ptr);
void __wrap_free(void *ptr)
{
	uintptr_t p = (uintptr_t)ptr;

	if (p >= lent_base && p < (uintptr_t)lent_base + LENT_BYTES) {
		pool_frees++;
		return;
	}
	__real_free(ptr);
}

static void put32(uint8_t *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (24U - 8U * i));
}

static void publish(uint32_t codec, uint64_t write)
{
	static uint32_t line_seq;
	uint32_t seq = (line_seq += 2U);

	put32(producer->sequence, seq | 1U);
	put32(producer->generation, generation);
	put32(producer->write_cursor_hi, (uint32_t)(write >> 32));
	put32(producer->write_cursor_lo, (uint32_t)write);
	put32(producer->heartbeat, ++heartbeat);
	put32(producer->flags, 0U);
	put32(producer->stream_seq, 1U);
	put32(producer->stream_codec, codec);
	put32(producer->stream_gain, 32767U);
	put32(producer->sequence, seq);
}

static uint32_t lease_blocks(unsigned *in_lent)
{
	uint32_t n = 0U;

	*in_lent = 0U;
	for (uint32_t i = 0; i < card_pool.block_count; i++) {
		if (card_pool.blocks[i].owner != SDK_AUDIO_LEASE_DECODE_OWNER)
			continue;
		n++;
		if (card_pool.blocks[i].addr >= lent_base &&
		    card_pool.blocks[i].addr < lent_base + LENT_BYTES)
			(*in_lent)++;
	}
	return n;
}

/* A lease decoding mid-stream with all of its memory in the lent range. */
static void start_lease(uint32_t codec, const uint8_t *data)
{
	struct audio_fabric_ring_grant g;
	unsigned in_lent;
	uint32_t n;

	fabric_reset_state();
	host_card_pool_reset(1U << 20);
	/* Fill the permanent range so the lease lands in the lent one. */
	if (card_pool_alloc(&card_pool, 1U << 20,
	                    CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA_INPUT, 1U)) == 0U) {
		puts("permanent range filler failed");
		exit(1);
	}
	if (card_pool_add_range(&card_pool, lent_base, lent_base + LENT_BYTES, 1) != 1) {
		puts("lent range rejected");
		exit(1);
	}
	card_pool_open_range(&card_pool, 1U);
	pool_frees = 0U;
	heartbeat = 0U;
	memset(g_ring_pcm_a, 0, sizeof(g_ring_pcm_a));
	check(audio_fabric_ring_acquire(SLOT, SDK_AUDIO_METER_IDENTITY_SDK_STREAM, 128U, 0U,
	      CODEC_FLAGS, &g) == AUDIO_FABRIC_LEASE_OK, "acquire", NULL);
	generation = g.generation;
	memcpy(g_ring_pcm_a, data, HOLD_BYTES);
	for (unsigned i = 0; i < 8U; i++) {
		publish(codec, HOLD_BYTES);
		dma_tick(1);
		audio_fabric_isr();
		for (unsigned j = 0; j < 4U; j++)
			audio_fabric_lease_poll();
	}
	n = lease_blocks(&in_lent);
	check(n != 0U && in_lent == n, "lease decoder memory is in the lent range", NULL);
}

static void reset_sequence(uint32_t codec, const uint8_t *data, int quiesce)
{
	unsigned in_lent;

	start_lease(codec, data);
	if (quiesce) {
		audio_fabric_codec_reset_quiesce();
		check(lease_blocks(&in_lent) == 0U, "quiesce frees every lease block", NULL);
	}
	/* main.c: card_pool_take_back_lent(), then the Amiga uses the window. */
	card_pool_take_back_lent(&card_pool);
	memset((void *)(uintptr_t)lent_base, 0xA5, LENT_BYTES);
	/* sdk_mailbox_init(): audio_fabric_reset() drops the leases. */
	audio_fabric_reset();
}

/* True when tearing the lease down after the take-back touched the revoked
 * window: the child faulted, or counted a pool free() or an ignored free. */
static int wrong_order_detected(uint32_t codec, const uint8_t *data)
{
	int status;
	pid_t pid;

	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		reset_sequence(codec, data, 0);
		_exit(pool_frees != 0U || card_pool.ignored_frees != 0U ? 10 : 0);
	}
	if (pid < 0 || waitpid(pid, &status, 0) != pid)
		return 0;
	return WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) == 10);
}

int main(void)
{
	FILE *f = fopen("../vorbis/fixtures/stereo44k.ogg", "rb");
	void *map = mmap(0, LENT_BYTES + CARD_POOL_PAGE_BYTES, PROT_READ | PROT_WRITE,
	                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT | MAP_NORESERVE, -1, 0);

	if (!f || map == MAP_FAILED) {
		perror("setup");
		return 2;
	}
	g_vorbis_len = (uint32_t)fread(g_vorbis, 1U, sizeof(g_vorbis), f);
	fclose(f);
	lent_base = ((uint32_t)(uintptr_t)map + CARD_POOL_PAGE_BYTES - 1U) &
	            ~(CARD_POOL_PAGE_BYTES - 1U);
	audio_fabric_codec_register(&audio_fabric_stream_codec_ops);

	/* The reset handler's order: nothing touches the revoked window. */
	reset_sequence(SDK_AUDIO_RING_CODEC_FLAC, fx_stereo16_flac, 1);
	check(pool_frees == 0U && card_pool.ignored_frees == 0U,
	      "FLAC lease: quiesce before take-back frees nothing in the revoked window",
	      fmt("%u free() calls, %u ignored", pool_frees, (unsigned)card_pool.ignored_frees));
	reset_sequence(SDK_AUDIO_RING_CODEC_VORBIS, g_vorbis, 1);
	check(pool_frees == 0U && card_pool.ignored_frees == 0U,
	      "Vorbis lease: quiesce before take-back frees nothing in the revoked window",
	      fmt("%u free() calls, %u ignored", pool_frees, (unsigned)card_pool.ignored_frees));

	/* The wrong order is caught. libFLAC dereferences the revoked decoder
	 * (a fault) or frees pool memory through free(); Tremor's regions are
	 * frees the pool ignores. Run in a child: the fault ends it. */
	check(wrong_order_detected(SDK_AUDIO_RING_CODEC_FLAC, fx_stereo16_flac),
	      "wrong order: FLAC teardown after the take-back is detected", NULL);
	check(wrong_order_detected(SDK_AUDIO_RING_CODEC_VORBIS, g_vorbis),
	      "wrong order: Vorbis teardown after the take-back is detected", NULL);

	/* No decoder starts again before the leases are gone, and the reset
	 * lifts the hold: a codec lease can be acquired afterwards. */
	{
		struct audio_fabric_ring_grant g;

		fabric_reset_state();
		audio_fabric_codec_reset_quiesce();
		check(audio_fabric_ring_acquire(SLOT, SDK_AUDIO_METER_IDENTITY_SDK_STREAM, 128U, 0U,
		      CODEC_FLAGS, &g) == AUDIO_FABRIC_LEASE_EBUSY,
		      "no codec lease during the reset", NULL);
		audio_fabric_reset();
		check(audio_fabric_ring_acquire(SLOT, SDK_AUDIO_METER_IDENTITY_SDK_STREAM, 128U, 0U,
		      CODEC_FLAGS, &g) == AUDIO_FABRIC_LEASE_OK,
		      "codec lease after the reset", NULL);
		audio_fabric_reset();
	}
	if (failures == 0)
		puts("audio codec reset order tests passed");
	return failures != 0;
}
