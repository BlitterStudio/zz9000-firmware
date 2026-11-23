/*
 * CLIENT_RELEASE and the AX media pump, driven through the production
 * dispatcher (handle_request) with the real bookkeeping in sdk_mailbox.c.
 *
 * Two openers of zz9k.library share the card: client 1 plays a media
 * session through the pump, client 2 is a tool that opens the library beside
 * it and closes it again. The final CloseLibrary() of client 2 sends
 * CLIENT_RELEASE, and that release must leave client 1's pump and its audio
 * binding alone. A pump stopped there leaves client 1's session audio-bound
 * with nothing retiring it: the output clock never moves again and the
 * player holds its next picture forever. Only the owner's release stops the
 * pump.
 *
 * Only the hardware seams are replaced: the fabric producer calls (counted),
 * the media session module (a one-session ownership table) and the surface
 * allocator.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "audio_fabric.h"
#include "memorymap.h"
#include "scheduler.h"
#include "sdk_aperture_layout.h"
#include "sdk_mailbox.h"
#include "sdk_media_session.h"

struct SDKMailboxEntry {
	uint8_t request_id[4];
	uint8_t opcode[2];
	uint8_t status[2];
	uint8_t flags[2];
	uint8_t payload_len[2];
	uint8_t user_cookie[4];
	uint8_t payload[48];
};

extern uint16_t sdk_mailbox_test_handle_request(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp);

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

#define CLIENT_PLAYER 1U
#define CLIENT_TOOL   2U
#define MEDIA_SESSION 7U

/* ---- hardware and module seams ---- */

static taskq_shared_t g_shared;
taskq_shared_t *scheduler_shared(void) { return &g_shared; }
int scheduler_core1_available(void) { return 1; }
void scheduler_core1_publish(void) {}
int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t saved) { (void)saved; }

uint32_t sdk_aperture_runtime_flags(void) { return SDK_APERTURE_FLAG_VALID; }
int sdk_aperture_runtime_is_zorro3(void) { return 1; }
int sdk_aperture_runtime_is_legacy(void) { return 0; }
uint32_t surface_allocator_total_bytes(void) { return 64U * 1024U * 1024U; }

int sdk_image_stream_release_client(uint16_t owner, uint32_t *close_session)
{
	(void)owner;
	*close_session = 0U;
	return 1;
}
int sdk_video_stream_release_client(uint16_t owner, uint32_t *close_session)
{
	(void)owner;
	*close_session = 0U;
	return 1;
}
int sdk_decompress_stream_release_client(uint16_t owner)
{
	(void)owner;
	return 1;
}

/* The media session module: one session, owned by the player. */
static unsigned g_session_audio_bound;
static unsigned g_release_unbinds;

int sdk_media_session_owned_by(uint32_t session, uint16_t owner_id)
{
	return session == MEDIA_SESSION && owner_id == CLIENT_PLAYER;
}

int sdk_media_session_release_client(uint16_t owner, uint32_t *close_session)
{
	*close_session = 0U;
	if (owner != CLIENT_PLAYER)
		return 1;
	if (g_session_audio_bound) {
		g_session_audio_bound = 0U;
		g_release_unbinds++;
	}
	return 1;
}

uint16_t sdk_media_session_audio_bind(uint32_t session, uint32_t flags,
	struct SDKMediaSessionAudioResult *result)
{
	(void)flags;
	if (session != MEDIA_SESSION)
		return SDK_STATUS_BAD_HANDLE;
	g_session_audio_bound = 1U;
	memset(result, 0, sizeof(*result));
	result->session = session;
	result->sample_rate = 48000U;
	result->channels = 2U;
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_audio_unbind(uint32_t session, uint32_t flags,
	struct SDKMediaSessionAudioResult *result)
{
	(void)session;
	(void)flags;
	memset(result, 0, sizeof(*result));
	g_session_audio_bound = 0U;
	return SDK_STATUS_OK;
}

int sdk_media_session_audio_source(uint32_t session,
	struct SDKMediaAudioSource *source)
{
	(void)session;
	(void)source;
	return 0;
}

/* No voice session is open in this test. */
int audio_voice_session_active(void) { return 0; }

/* The pump's setup seams. */
int audio_codec_present(void) { return 1; }
enum audio_fabric_ownership audio_fabric_ownership(void)
{
	return AUDIO_FABRIC_IDLE;
}
int audio_fabric_conversion_admissible(uint32_t rate)
{
	(void)rate;
	return 1;
}
void audio_scene_meter_output_identity(uint32_t identity) { (void)identity; }
void audio_pump_preconvert_reset(void *state, uint8_t *ring, uint32_t bytes)
{
	(void)state;
	(void)ring;
	(void)bytes;
}

/* Audio fabric producer seam: counted. */
static unsigned g_attach_calls, g_detach_calls, g_pump_detach_calls;

int audio_fabric_producer_attach(uint32_t slot,
	const struct audio_fabric_producer_ops *ops)
{
	(void)slot;
	(void)ops;
	g_attach_calls++;
	return 1;
}
void audio_fabric_producer_rate_set(uint32_t slot, uint32_t rate)
{
	(void)slot;
	(void)rate;
}
void audio_fabric_producer_gain_set(uint32_t slot, uint16_t gain)
{
	(void)slot;
	(void)gain;
}
void audio_fabric_request_rebuild(uint32_t slot) { (void)slot; }
void audio_fabric_producer_detach(uint32_t slot)
{
	g_detach_calls++;
	if (slot == AUDIO_FABRIC_SLOT_PUMP)
		g_pump_detach_calls++;
}
int audio_fabric_others_live(uint32_t slot)
{
	(void)slot;
	return 0;
}
void audio_fabric_producer_restart(uint32_t slot) { (void)slot; }
int audio_fabric_producer_prime_and_go_live(uint32_t slot)
{
	(void)slot;
	return 1;
}
void audio_fabric_producer_go_live(uint32_t slot) { (void)slot; }

/* ---- request plumbing ---- */

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static struct SDKMailboxEntry g_comp;

static uint16_t call(uint16_t opcode, uint16_t client,
	const uint8_t payload[48], uint16_t payload_len)
{
	struct SDKMailboxEntry req;
	uint16_t flags = (uint16_t)(client << SDK_ENTRY_CLIENT_ID_SHIFT);

	memset(&req, 0, sizeof(req));
	memset(&g_comp, 0, sizeof(g_comp));
	put32(req.request_id, 1U);
	req.opcode[0] = (uint8_t)(opcode >> 8);
	req.opcode[1] = (uint8_t)opcode;
	req.flags[0] = (uint8_t)(flags >> 8);
	req.flags[1] = (uint8_t)flags;
	req.payload_len[0] = (uint8_t)(payload_len >> 8);
	req.payload_len[1] = (uint8_t)payload_len;
	if (payload != NULL)
		memcpy(req.payload, payload, 48U);
	return sdk_mailbox_test_handle_request(&req, &g_comp);
}

static uint16_t release_client(uint16_t client)
{
	return call(SDK_OP_CLIENT_RELEASE, client, NULL, 0U);
}

static uint16_t bind_audio(uint16_t client, uint32_t session)
{
	uint8_t payload[48];

	memset(payload, 0, sizeof(payload));
	put32(payload, session);
	return call(SDK_OP_MEDIA_SESSION_AUDIO_BIND, client, payload, 48U);
}

static void bind_player(void)
{
	g_attach_calls = 0U;
	g_detach_calls = 0U;
	g_pump_detach_calls = 0U;
	g_release_unbinds = 0U;
	CHECK(bind_audio(CLIENT_PLAYER, MEDIA_SESSION) == SDK_STATUS_OK);
	CHECK(g_attach_calls == 1U);
	CHECK(g_session_audio_bound == 1U);
}

/* The tool's release leaves the player's pump bound; the player's own
 * release stops it and unbinds its session. */
static void test_other_client_release_keeps_pump(void)
{
	bind_player();

	CHECK(release_client(CLIENT_TOOL) == SDK_STATUS_OK);
	CHECK(g_pump_detach_calls == 0U);
	CHECK(g_session_audio_bound == 1U);
	/* Still bound to the same session: BIND is idempotent and starts no
	 * second producer. */
	CHECK(bind_audio(CLIENT_PLAYER, MEDIA_SESSION) == SDK_STATUS_OK);
	CHECK(g_attach_calls == 1U);

	CHECK(release_client(CLIENT_PLAYER) == SDK_STATUS_OK);
	CHECK(g_pump_detach_calls == 1U);
	CHECK(g_release_unbinds == 1U);
	CHECK(g_session_audio_bound == 0U);
	/* The pump is free again: the next BIND attaches a fresh producer. */
	CHECK(bind_audio(CLIENT_PLAYER, MEDIA_SESSION) == SDK_STATUS_OK);
	CHECK(g_attach_calls == 2U);
	CHECK(release_client(CLIENT_PLAYER) == SDK_STATUS_OK);
}

int main(void)
{
	/* The audio-stream table lives at a fixed firmware address; the
	 * client-release chain reads it. */
	CHECK(mmap((void *)(uintptr_t)SDK_AUDIO_STREAMS_ADDRESS,
	           SDK_AUDIO_STREAMS_MAX_BYTES, PROT_READ | PROT_WRITE,
	           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
	           0) != MAP_FAILED);
	if (failures != 0)
		return 1;
	taskq_init(&g_shared.queue);
	test_other_client_release_keeps_pump();
	if (failures != 0) {
		printf("media_pump_release_test: %d of %d checks FAILED\n",
		       failures, checks);
		return 1;
	}
	printf("media_pump_release_test: %d checks passed\n", checks);
	return 0;
}
