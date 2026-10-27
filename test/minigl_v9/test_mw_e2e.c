/*
 * End-to-end integration test for ZZ9000 MiniGL Wire-v9 Transport.
 *
 * Compiles the library's real ZZ9000 transport (amiberry_transport_zz9000.c)
 * against mock AmigaOS exec/expansion/graphics calls and an in-memory card
 * whose registers and memory windows are served by the real firmware mw_service
 * running on a worker thread emulating ARM Core 1.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

#include "mw_types.h"
#include "mw_service.h"
#include "scheduler.h"
#include "amiberry_transport.h"

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/graphics.h>

/* Global mock structures */
static struct Library g_mock_expansion_lib;
static struct GfxBase g_mock_gfx_base;
struct GfxBase *GfxBase = &g_mock_gfx_base;
static struct ConfigDev g_mock_cd;
static uint8_t *g_card_mem = NULL;
static size_t g_card_size = 0;
static volatile uint16_t *g_doorbell_ptr = NULL;
static volatile void *g_active_ctrl_base = NULL;

static volatile int g_core1_running = 0;
static volatile int g_core1_paused = 0;
static pthread_t g_core1_thread;


int mgl_backend_cpu_requested(void)
{
    return 0;
}
/* --- Mock AmigaOS Functions --- */

struct Library *OpenLibrary(STRPTR name, ULONG version)
{
    (void)version;
    if (name && strcmp(name, "expansion.library") == 0) {
        return &g_mock_expansion_lib;
    }
    return NULL;
}

void CloseLibrary(struct Library *lib)
{
    (void)lib;
}

void CacheClearE(APTR addr, ULONG length, ULONG flags)
{
    (void)addr;
    (void)length;
    (void)flags;
    __sync_synchronize();
}

void CopyMem(const void *src, void *dst, ULONG size)
{
    if (src && dst && size > 0) {
        memcpy(dst, src, size);
    }
}

struct ConfigDev *FindConfigDev(struct ConfigDev *old, ULONG manuf, ULONG product)
{
    if (old != NULL)
        return NULL; /* Single board mock */

    if (manuf == g_mock_cd.cd_Rom_er_Manufacturer &&
        product == g_mock_cd.cd_Rom_er_Product) {
        return &g_mock_cd;
    }
    return NULL;
}

void WaitTOF(void)
{
    /* Yield / short sleep emulating vertical blank period */
    usleep(150);
}

/* --- Core 1 Background Worker Thread --- */

static void *core1_worker(void *arg)
{
    (void)arg;
    while (g_core1_running) {
        if (!g_core1_paused) {
            if (g_doorbell_ptr && *g_doorbell_ptr != 0) {
                *g_doorbell_ptr = 0;
                mw_service_doorbell();
            }
            if (mw_service_has_work()) {
                mw_service_poll();
            }
        }
        usleep(40);
    }
    return NULL;
}

static void start_core1(void)
{
    if (!g_core1_running) {
        g_core1_running = 1;
        g_core1_paused = 0;
        int rc = pthread_create(&g_core1_thread, NULL, core1_worker, NULL);
        assert(rc == 0);
    }
}

static void stop_core1(void)
{
    if (g_core1_running) {
        g_core1_running = 0;
        pthread_join(g_core1_thread, NULL);
    }
}

/* --- Card Setup Helpers --- */

static void free_card_memory(void)
{
    mw_service_set_host_vram(NULL, 0);
    if (g_card_mem) {
        munmap(g_card_mem, g_card_size);
        g_card_mem = NULL;
        g_card_size = 0;
    }
}

static void setup_mock_card(int is_z3)
{
    stop_core1();
    free_card_memory();

    if (is_z3) {
        g_card_size = 128UL * 1024UL * 1024UL;
#ifdef MAP_32BIT
        g_card_mem = mmap(NULL, g_card_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#else
        g_card_mem = mmap(NULL, g_card_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
        assert(g_card_mem != MAP_FAILED);
        memset(g_card_mem, 0, g_card_size);

        /* SDK registers */
        *(uint16_t *)(g_card_mem + 0x0100) = 0x5a39; /* SDK magic */
        *(uint16_t *)(g_card_mem + 0x0102) = 0x0200; /* SDK version 2.0 */
        g_doorbell_ptr = (volatile uint16_t *)(g_card_mem + 0x1108);

        g_mock_cd.cd_BoardAddr = g_card_mem;
        g_mock_cd.cd_BoardSize = (ULONG)g_card_size;
        g_mock_cd.cd_Rom_er_Manufacturer = 0x6d6e;
        g_mock_cd.cd_Rom_er_Product = 4; /* Zorro III */

        g_active_ctrl_base = g_card_mem + 0x06E10000UL;
        int bound = mw_service_bind_region(g_card_mem + 0x06E10000UL, 0x100000UL);
        assert(bound == 1);
        mw_service_set_host_vram(g_card_mem, 0x07000000UL);
    } else {
        g_card_size = 4UL * 1024UL * 1024UL;
#ifdef MAP_32BIT
        g_card_mem = mmap(NULL, g_card_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#else
        g_card_mem = mmap(NULL, g_card_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
        assert(g_card_mem != MAP_FAILED);
        memset(g_card_mem, 0, g_card_size);

        /* SDK registers */
        *(uint16_t *)(g_card_mem + 0x0100) = 0x5a39; /* SDK magic */
        *(uint16_t *)(g_card_mem + 0x0102) = 0x0200; /* SDK version 2.0 */
        g_doorbell_ptr = (volatile uint16_t *)(g_card_mem + 0x0108);

        g_mock_cd.cd_BoardAddr = g_card_mem;
        g_mock_cd.cd_BoardSize = (ULONG)g_card_size;
        g_mock_cd.cd_Rom_er_Manufacturer = 0x6d6e;
        g_mock_cd.cd_Rom_er_Product = 3; /* Zorro II */
        uint32_t ctrl_offset = (uint32_t)(g_card_size - 0x40000UL);
        g_active_ctrl_base = g_card_mem + ctrl_offset;
        int bound = mw_service_bind_region(g_card_mem + ctrl_offset, 0x40000UL);
        assert(bound == 1);
        mw_service_set_host_vram(g_card_mem, ctrl_offset + 0x001F0000UL);
    }

    start_core1();
}

/* Helper to build a wire-v9 packet with CLEAR and PRESENT */
static uint32_t build_clear_present_packet(uint8_t *buf, float r, float g, float b, float a)
{
    mw_put_be32(buf, MW_MAGIC);
    mw_put_be32(buf + 4, MW_VERSION);
    mw_put_be32(buf + 12, 2); /* 2 commands */

    /* Command 0: MW_CLEAR */
    mw_put_be32(buf + 16, MW_CLEAR);
    mw_put_be32(buf + 20, 24);
    mw_put_be32(buf + 24, 3); /* Color and depth clear */
    mw_put_bef(buf + 28, r);
    mw_put_bef(buf + 32, g);
    mw_put_bef(buf + 36, b);
    mw_put_bef(buf + 40, a);
    mw_put_bef(buf + 44, 1.0f);

    /* Command 1: MW_PRESENT */
    mw_put_be32(buf + 48, MW_PRESENT);
    mw_put_be32(buf + 52, 0);

    uint32_t total = 16 + (8 + 24) + (8 + 0); /* 56 bytes */
    mw_put_be32(buf + 8, total);
    return total;
}

/* --- Test Suites --- */

static void test_probe_and_handshake(void)
{
    setup_mock_card(1); /* Zorro III */

    const struct MiniGLTransport *transport = amiberry_transport_zz9000_probe();
    assert(transport != NULL);
    assert(strcmp(transport->name, "ZZ9000 Hardware Transport") == 0);
    assert(transport->max_packet_bytes == 256UL * 1024UL);

    ULONG query_ver = transport->query();
    assert(query_ver == 9UL);

    /* Test negative probe cases */
    transport->term();

    /* 1. Invalid SDK magic register */
    *(uint16_t *)(g_card_mem + 0x0100) = 0x0000;
    assert(amiberry_transport_zz9000_probe() == NULL);
    *(uint16_t *)(g_card_mem + 0x0100) = 0x5a39;

    /* 2. Unsupported SDK version (major < 2) */
    *(uint16_t *)(g_card_mem + 0x0102) = 0x0100;
    assert(amiberry_transport_zz9000_probe() == NULL);
    *(uint16_t *)(g_card_mem + 0x0102) = 0x0200;

    /* 3. Missing Wire-v9 capability bit */
    volatile ZZ9KMiniGLStatusV9 *st = (volatile ZZ9KMiniGLStatusV9 *)(g_card_mem + 0x06E10080UL);
    st->capabilities = 0;
    assert(amiberry_transport_zz9000_probe() == NULL);
    st->capabilities = (1U << 18);

    /* Re-probe succeeds */
    transport = amiberry_transport_zz9000_probe();
    assert(transport != NULL);
    assert(transport->query() == 9UL);
    transport->term();

    printf("PASS: test_probe_and_handshake\n");
}

static void test_create_and_destroy(void)
{
    setup_mock_card(1);
    const struct MiniGLTransport *transport = amiberry_transport_zz9000_probe();
    assert(transport != NULL);

    /* Valid create */
    ULONG handle = transport->create(64, 48);
    assert(handle != 0);
    assert((handle >> 16) >= 1);       /* Generation */
    assert((handle & 0xffffU) >= 1);   /* Slot */

    /* Invalid creations */
    assert(transport->create(0, 48) == 0);
    assert(transport->create(64, 0) == 0);
    assert(transport->create(5000, 48) == 0);

    /* Destroy */
    transport->destroy(handle);

    /* Double destroy is harmless */
    transport->destroy(handle);

    transport->term();
    printf("PASS: test_create_and_destroy\n");
}

static void test_submit_flow_control(void)
{
    setup_mock_card(1);
    const struct MiniGLTransport *transport = amiberry_transport_zz9000_probe();
    assert(transport != NULL);

    ULONG handle = transport->create(64, 48);
    assert(handle != 0);

    uint8_t packet[256];
    uint32_t len = build_clear_present_packet(packet, 0.0f, 1.0f, 0.0f, 1.0f);

    /* 1. Pause Core 1 to test ring backpressure */
    g_core1_paused = 1;

    /* Submit Slot 0 */
    ULONG st0 = transport->submit(handle, packet, len);
    assert(st0 == 0);

    /* Submit Slot 1 (both slots now occupied) */
    ULONG st1 = transport->submit(handle, packet, len);
    assert(st1 == 0);

    /* Submit 3rd packet while Core 1 is paused: must hit backpressure and timeout */
    ULONG st2 = transport->submit(handle, packet, len);
    assert(st2 == MINIGL_BRIDGE_ERROR_CAPACITY);

    /* 2. Unpause Core 1: slots drain */
    g_core1_paused = 0;
    usleep(2000); /* Allow Core 1 to process queued slots */

    /* Submissions now succeed */
    for (int i = 0; i < 10; ++i) {
        ULONG st = transport->submit(handle, packet, len);
        assert(st == 0);
    }

    transport->destroy(handle);
    transport->term();
    printf("PASS: test_submit_flow_control\n");
}

static void trigger_firmware_reset(void)
{
    g_core1_paused = 1;
    usleep(500); /* Emulate Core 0 parking Core 1 before reset */
    mw_service_reset();
    g_core1_paused = 0;
}

static void trigger_core1_fault(void)
{
    g_core1_paused = 1;
    usleep(500); /* Emulate watchdog catching faulted Core 1 */
    mw_service_on_core1_fault();
    g_core1_paused = 0;
}

static void test_lost_on_generation_change(void)
{
    setup_mock_card(1);
    const struct MiniGLTransport *transport = amiberry_transport_zz9000_probe();
    assert(transport != NULL);

    ULONG handle = transport->create(64, 48);
    assert(handle != 0);

    uint8_t packet[256];
    uint32_t len = build_clear_present_packet(packet, 1.0f, 0.0f, 0.0f, 1.0f);
    assert(transport->submit(handle, packet, len) == 0);

    /* 1. Test LOST after firmware reset */
    trigger_firmware_reset();
    /* Next submit must return MINIGL_BRIDGE_ERROR_CONTEXT_LOST */
    ULONG submit_st = transport->submit(handle, packet, len);
    assert(submit_st == MINIGL_BRIDGE_ERROR_CONTEXT_LOST);

    /* Next readback must return MINIGL_BRIDGE_ERROR_CONTEXT_LOST */
    uint8_t pixels[64 * 48 * 4];
    ULONG read_st = transport->readback(handle, pixels, sizeof(pixels));
    assert(read_st == MINIGL_BRIDGE_ERROR_CONTEXT_LOST);

    /* Next request must return MINIGL_BRIDGE_ERROR_CONTEXT_LOST */
    uint8_t req[4] = {0, 0, 0, 4};
    uint8_t reply[64];
    ULONG desc[2] = {(ULONG)(uintptr_t)reply, 64};
    ULONG req_st = transport->request(handle, req, 4, desc);
    assert(req_st == MINIGL_BRIDGE_ERROR_CONTEXT_LOST);

    /* Re-probing binds to the new session generation */
    transport->term();
    transport = amiberry_transport_zz9000_probe();
    assert(transport != NULL);
    assert(transport->query() == 9UL);

    ULONG new_handle = transport->create(64, 48);
    assert(new_handle != 0);
    assert(transport->submit(new_handle, packet, len) == 0);
    /* 2. Test LOST after Core 1 fault */
    trigger_core1_fault();
    assert(transport->submit(new_handle, packet, len) == MINIGL_BRIDGE_ERROR_CONTEXT_LOST);

    transport->term();
    printf("PASS: test_lost_on_generation_change\n");
}

static void test_request_with_reply(void)
{
    setup_mock_card(1);
    const struct MiniGLTransport *transport = amiberry_transport_zz9000_probe();
    assert(transport != NULL);

    ULONG handle = transport->create(64, 48);
    assert(handle != 0);

    /* Request MW_PLUGIN_VERSION (opcode 4) */
    uint8_t req[4];
    mw_put_be32(req, MW_PLUGIN_VERSION);

#ifdef MAP_32BIT
    uint8_t *reply_buf = mmap(NULL, 1024, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#else
    uint8_t *reply_buf = malloc(1024);
#endif
    assert(reply_buf != NULL);
    memset(reply_buf, 0, 1024);

    ULONG desc[2] = {(ULONG)(uintptr_t)reply_buf, 64};
    ULONG st = transport->request(handle, req, sizeof(req), desc);
    assert(st == 0);
    assert(strcmp((char *)reply_buf, "zz9000-minigl-v29-v9") == 0);

    /* Request with capacity too small */
    memset(reply_buf, 0, 1024);
    ULONG small_desc[2] = {(ULONG)(uintptr_t)reply_buf, 5};
    st = transport->request(handle, req, sizeof(req), small_desc);
    assert(st == MINIGL_BRIDGE_ERROR_CAPACITY);

#ifdef MAP_32BIT
    munmap(reply_buf, 1024);
#else
    free(reply_buf);
#endif

    transport->destroy(handle);
    transport->term();
    printf("PASS: test_request_with_reply\n");
}

static void test_readback(void)
{
    setup_mock_card(1);
    const struct MiniGLTransport *transport = amiberry_transport_zz9000_probe();
    assert(transport != NULL);

    ULONG handle = transport->create(16, 16);
    assert(handle != 0);

    /* Clear to solid blue (0, 0, 1, 1) and present */
    uint8_t packet[256];
    uint32_t len = build_clear_present_packet(packet, 0.0f, 0.0f, 1.0f, 1.0f);
    assert(transport->submit(handle, packet, len) == 0);

#ifdef MAP_32BIT
    uint8_t *pixels = mmap(NULL, 16 * 16 * 4, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#else
    uint8_t *pixels = malloc(16 * 16 * 4);
#endif
    assert(pixels != NULL);
    memset(pixels, 0, 16 * 16 * 4);

    ULONG st = transport->readback(handle, pixels, 16 * 16 * 4);
    assert(st == 0);

    for (int i = 0; i < 16 * 16; ++i) {
        assert(pixels[i * 4 + 0] == 0);   /* R */
        assert(pixels[i * 4 + 1] == 0);   /* G */
        assert(pixels[i * 4 + 2] == 255); /* B */
        assert(pixels[i * 4 + 3] == 255); /* A */
    }

#ifdef MAP_32BIT
    munmap(pixels, 16 * 16 * 4);
#else
    free(pixels);
#endif

    transport->destroy(handle);
    transport->term();
    printf("PASS: test_readback\n");
}

static void test_z2_packet_capacity(void)
{
    setup_mock_card(0); /* Zorro II */

    const struct MiniGLTransport *transport = amiberry_transport_zz9000_probe();
    assert(transport != NULL);
    assert(transport->max_packet_bytes == 64UL * 1024UL);
    assert(transport->query() == 9UL);

    ULONG handle = transport->create(16, 16);
    assert(handle != 0);

    /* Small valid packet (56 bytes) */
    uint8_t packet[256];
    uint32_t len = build_clear_present_packet(packet, 1.0f, 1.0f, 0.0f, 1.0f);
    assert(transport->submit(handle, packet, len) == 0);

    /* Packet exactly 64 KiB */
    uint8_t *exact_pkt = malloc(65536);
    assert(exact_pkt != NULL);
    memset(exact_pkt, 0, 65536);
    mw_put_be32(exact_pkt, MW_MAGIC);
    mw_put_be32(exact_pkt + 4, MW_VERSION);
    mw_put_be32(exact_pkt + 8, 65536);
    mw_put_be32(exact_pkt + 12, 1); /* 1 command */
    mw_put_be32(exact_pkt + 16, MW_PRESENT);
    mw_put_be32(exact_pkt + 20, 65536 - 24); /* Large payload */

    assert(transport->submit(handle, exact_pkt, 65536) == 0);

    /* Packet exceeding 64 KiB: must be rejected with MINIGL_BRIDGE_ERROR_INVALID */
    uint8_t *oversized_pkt = malloc(65540);
    assert(oversized_pkt != NULL);
    memset(oversized_pkt, 0, 65540);
    mw_put_be32(oversized_pkt, MW_MAGIC);
    mw_put_be32(oversized_pkt + 4, MW_VERSION);
    mw_put_be32(oversized_pkt + 8, 65540);
    mw_put_be32(oversized_pkt + 12, 1);
    mw_put_be32(oversized_pkt + 16, MW_PRESENT);
    mw_put_be32(oversized_pkt + 20, 65540 - 24);

    ULONG over_st = transport->submit(handle, oversized_pkt, 65540);
    assert(over_st == MINIGL_BRIDGE_ERROR_INVALID);

    free(exact_pkt);
    free(oversized_pkt);

    transport->destroy(handle);
    transport->term();
    printf("PASS: test_z2_packet_capacity\n");
}
static void test_back_to_back_and_pipeline(void)
{
    setup_mock_card(1);
    const struct MiniGLTransport *transport = amiberry_transport_zz9000_probe();
    assert(transport != NULL);

    ULONG handle = transport->create(64, 48);
    assert(handle != 0);

    /* 1. Issue request immediately after submit */
    uint8_t packet[256];
    uint32_t len = build_clear_present_packet(packet, 0.5f, 0.5f, 0.5f, 1.0f);
    assert(transport->submit(handle, packet, len) == 0);

    uint8_t req[4];
    mw_put_be32(req, MW_PLUGIN_VERSION);

#ifdef MAP_32BIT
    uint8_t *reply1 = mmap(NULL, 1024, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    uint8_t *reply2 = mmap(NULL, 1024, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#else
    uint8_t *reply1 = malloc(1024);
    uint8_t *reply2 = malloc(1024);
#endif
    assert(reply1 && reply2);
    memset(reply1, 0, 1024);
    memset(reply2, 0, 1024);

    ULONG desc1[2] = {(ULONG)(uintptr_t)reply1, 64};
    ULONG st = transport->request(handle, req, sizeof(req), desc1);
    assert(st == 0);
    assert(strcmp((char *)reply1, "zz9000-minigl-v29-v9") == 0);

    /* 2. Issue two back-to-back requests */
    ULONG desc2[2] = {(ULONG)(uintptr_t)reply2, 64};
    ULONG st_a = transport->request(handle, req, sizeof(req), desc1);
    ULONG st_b = transport->request(handle, req, sizeof(req), desc2);
    assert(st_a == 0);
    assert(st_b == 0);
    assert(strcmp((char *)reply1, "zz9000-minigl-v29-v9") == 0);
    assert(strcmp((char *)reply2, "zz9000-minigl-v29-v9") == 0);

    /* 3. Explicit check that firmware never stores to host block */
    g_core1_paused = 1;
    usleep(200);
    volatile ZZ9KMiniGLControlBlockV9 *ctrl =
        (volatile ZZ9KMiniGLControlBlockV9 *)g_active_ctrl_base;
    uint8_t host_block_copy[64];
    memcpy(host_block_copy, (const void *)((const uint8_t *)ctrl + 64), 64);
    mw_service_poll();
    assert(memcmp(host_block_copy, (const void *)((const uint8_t *)ctrl + 64), 64) == 0);
    g_core1_paused = 0;
#ifdef MAP_32BIT
    munmap(reply1, 1024);
    munmap(reply2, 1024);
#else
    free(reply1);
    free(reply2);
#endif

    transport->destroy(handle);
    transport->term();
    printf("PASS: test_back_to_back_and_pipeline\n");
}
static void test_mw_e2e_blit_and_capture(void)
{
    setup_mock_card(1); /* Zorro III */
    const struct MiniGLTransport *transport = amiberry_transport_zz9000_probe();
    assert(transport != NULL);

    ULONG handle = transport->create(32, 32);
    assert(handle != 0);

    /* 1. Clear to Red (1.0f, 0.0f, 0.0f, 1.0f) and PRESENT */
    uint8_t packet[256];
    uint32_t pkt_len = build_clear_present_packet(packet, 1.0f, 0.0f, 0.0f, 1.0f);
    assert(transport->submit(handle, packet, pkt_len) == 0);

    /* Setup mock VRAM span in card memory */
    /* Place span at board offset 0x00020000 (128 KiB into board, well within VRAM) */
    uint32_t card_offset = 0x00020000UL;
    uint8_t *mock_vram = g_card_mem + card_offset;
    uint32_t width = 32, height = 32;
    uint32_t format = 6; /* RGBFB_B8G8R8A8 (4 bpp) */
    uint32_t bpp = 4;
    uint32_t pitch = width * bpp; /* 128 bytes */
    uint32_t span_total = height * pitch; /* 4096 bytes */
    memset(mock_vram, 0, span_total);

    /* 2. Build MW_BLIT request */
    uint8_t blit_req[40];
    memset(blit_req, 0, sizeof(blit_req));
    mw_put_be32(blit_req, MW_BLIT);
    mw_put_be32(blit_req + 4, format);
    mw_put_be32(blit_req + 8, pitch);
    mw_put_be32(blit_req + 12, 0); /* frame_x = 0 */
    mw_put_be32(blit_req + 16, 0); /* frame_y = 0 */
    mw_put_be32(blit_req + 20, 1); /* count = 1 rect */
    mw_put_be32(blit_req + 24, 0); /* rx0 = 0 */
    mw_put_be32(blit_req + 28, 0); /* ry0 = 0 */
    mw_put_be32(blit_req + 32, width);  /* rx1 = 32 */
    mw_put_be32(blit_req + 36, height); /* ry1 = 32 */

    ULONG desc[2] = {(ULONG)(uintptr_t)mock_vram, span_total};
    ULONG st = transport->blit(handle, blit_req, sizeof(blit_req), desc);
    assert(st == 0);

    /* Verify mock VRAM content: Red in format 6 (A8R8G8B8: A=255, R=255, G=0, B=0) */
    for (uint32_t i = 0; i < width * height; ++i) {
        assert(mock_vram[i * 4 + 0] == 255); /* A */
        assert(mock_vram[i * 4 + 1] == 255); /* R */
        assert(mock_vram[i * 4 + 2] == 0);   /* G */
        assert(mock_vram[i * 4 + 3] == 0);   /* B */
    }

    /* 3. Negative blit cases:
     * - Off-card span address (e.g. host heap) returns MINIGL_BRIDGE_ERROR_UNAVAILABLE
     * - Past VRAM limit returns MINIGL_BRIDGE_ERROR_UNAVAILABLE
     * - Invalid format returns MINIGL_BRIDGE_ERROR_INVALID
     */
    uint8_t heap_buf[4096];
    ULONG bad_desc1[2] = {(ULONG)(uintptr_t)heap_buf, span_total};
    assert(transport->blit(handle, blit_req, sizeof(blit_req), bad_desc1) == MINIGL_BRIDGE_ERROR_UNAVAILABLE);

    ULONG bad_desc2[2] = {(ULONG)(uintptr_t)(g_card_mem + 0x07000000UL), span_total};
    assert(transport->blit(handle, blit_req, sizeof(blit_req), bad_desc2) == MINIGL_BRIDGE_ERROR_UNAVAILABLE);

    mw_put_be32(blit_req + 4, 99); /* Invalid format */
    assert(transport->blit(handle, blit_req, sizeof(blit_req), desc) == MINIGL_BRIDGE_ERROR_INVALID);
    mw_put_be32(blit_req + 4, format); /* Restore format */

    /* 4. MW_CAPTURE Fast Path:
     * - Overwrite back buffer with Green WITHOUT present
     * - Call MW_CAPTURE on matching span -> fast path restores front buffer (Red)
     * - Present and readback -> verifies Red was restored!
     */
    uint8_t scribble[256];
    /* Clear to Green (0, 1, 0, 1) without present */
    mw_put_be32(scribble, MW_MAGIC);
    mw_put_be32(scribble + 4, MW_VERSION);
    mw_put_be32(scribble + 8, 40);
    mw_put_be32(scribble + 12, 1);
    mw_put_be32(scribble + 16, MW_CLEAR);
    mw_put_be32(scribble + 20, 24);
    mw_put_be32(scribble + 24, 3);
    mw_put_bef(scribble + 28, 0.0f);
    mw_put_bef(scribble + 32, 1.0f); /* Green */
    mw_put_bef(scribble + 36, 0.0f);
    mw_put_bef(scribble + 40, 1.0f);
    mw_put_bef(scribble + 44, 1.0f);
    assert(transport->submit(handle, scribble, 40) == 0);

    /* Build MW_CAPTURE request (identical to blit_req but opcode is MW_CAPTURE) */
    uint8_t cap_req[40];
    memcpy(cap_req, blit_req, sizeof(cap_req));
    mw_put_be32(cap_req, MW_CAPTURE);

    st = transport->blit(handle, cap_req, sizeof(cap_req), desc);
    assert(st == 0);

    /* Submit PRESENT */
    uint8_t pres_pkt[32];
    mw_put_be32(pres_pkt, MW_MAGIC);
    mw_put_be32(pres_pkt + 4, MW_VERSION);
    mw_put_be32(pres_pkt + 8, 24);
    mw_put_be32(pres_pkt + 12, 1);
    mw_put_be32(pres_pkt + 16, MW_PRESENT);
    mw_put_be32(pres_pkt + 20, 0);
    assert(transport->submit(handle, pres_pkt, 24) == 0);

    /* Read back: should be Red (front buffer restored by fast-path capture) */
    uint8_t out_pixels[32 * 32 * 4];
    assert(transport->readback(handle, out_pixels, sizeof(out_pixels)) == 0);
    for (uint32_t i = 0; i < width * height; ++i) {
        assert(out_pixels[i * 4 + 0] == 255); /* R */
        assert(out_pixels[i * 4 + 1] == 0);   /* G */
        assert(out_pixels[i * 4 + 2] == 0);   /* B */
        assert(out_pixels[i * 4 + 3] == 255); /* A */
    }

    /* 5. MW_CAPTURE Slow Path (span decode):
     * - Write Blue pixels into mock VRAM span (BGRA: B=255, G=0, R=0, A=255)
     * - Redraw front buffer to invalidate fast-path snapshot
     * - Call MW_CAPTURE -> decodes mock VRAM into back buffer
     * - Present and readback -> verifies Blue was decoded from span!
     */
    for (uint32_t i = 0; i < width * height; ++i) {
        mock_vram[i * 4 + 0] = 255; /* A */
        mock_vram[i * 4 + 1] = 0;   /* R */
        mock_vram[i * 4 + 2] = 0;   /* G */
        mock_vram[i * 4 + 3] = 255; /* B */
    }

    /* Redraw front buffer to White and present (invalidates fast path) */
    uint8_t white_pkt[256];
    uint32_t white_len = build_clear_present_packet(white_pkt, 1.0f, 1.0f, 1.0f, 1.0f);
    assert(transport->submit(handle, white_pkt, white_len) == 0);

    /* Capture from mock VRAM (slow path decode) */
    st = transport->blit(handle, cap_req, sizeof(cap_req), desc);
    assert(st == 0);

    /* Present the decoded back buffer */
    assert(transport->submit(handle, pres_pkt, 24) == 0);

    /* Read back: should be Blue (decoded from mock VRAM) */
    assert(transport->readback(handle, out_pixels, sizeof(out_pixels)) == 0);
    for (uint32_t i = 0; i < width * height; ++i) {
        assert(out_pixels[i * 4 + 0] == 0);   /* R */
        assert(out_pixels[i * 4 + 1] == 0);   /* G */
        assert(out_pixels[i * 4 + 2] == 255); /* B */
        assert(out_pixels[i * 4 + 3] == 255); /* A */
    }

    /* 6. Multi-rect clip BLIT */
    /* Draw Red, then BLIT with 2 rects: left half (0..16) and right half (20..32) */
    uint8_t red_pkt[256];
    uint32_t red_len = build_clear_present_packet(red_pkt, 1.0f, 0.0f, 0.0f, 1.0f);
    assert(transport->submit(handle, red_pkt, red_len) == 0);

    memset(mock_vram, 0, span_total);
    uint8_t multi_req[56];
    memset(multi_req, 0, sizeof(multi_req));
    mw_put_be32(multi_req, MW_BLIT);
    mw_put_be32(multi_req + 4, format);
    mw_put_be32(multi_req + 8, pitch);
    mw_put_be32(multi_req + 12, 0);
    mw_put_be32(multi_req + 16, 0);
    mw_put_be32(multi_req + 20, 2); /* 2 rects */
    /* Rect 0: 0, 0 to 16, 32 */
    mw_put_be32(multi_req + 24, 0);
    mw_put_be32(multi_req + 28, 0);
    mw_put_be32(multi_req + 32, 16);
    mw_put_be32(multi_req + 36, 32);
    /* Rect 1: 20, 0 to 32, 32 */
    mw_put_be32(multi_req + 40, 20);
    mw_put_be32(multi_req + 44, 0);
    mw_put_be32(multi_req + 48, 32);
    mw_put_be32(multi_req + 52, 32);

    st = transport->blit(handle, multi_req, sizeof(multi_req), desc);
    assert(st == 0);

    /* Verify: columns 0..15 are Red, 16..19 are 0, 20..31 are Red */
    for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
            uint8_t *px = mock_vram + y * pitch + x * 4;
            if (x < 16 || x >= 20) {
                assert(px[0] == 255 && px[1] == 255 && px[2] == 0 && px[3] == 0);
            } else {
                assert(px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 0);
            }
        }
    }

    /* 7. Odd pitch BLIT in RGB565PC (format 4, 2 bpp) */
    uint32_t odd_pitch = 32 * 2 + 3; /* 67 bytes */
    uint32_t odd_total = 32 * odd_pitch;
    uint8_t *odd_vram = g_card_mem + 0x00030000UL;
    memset(odd_vram, 0, odd_total);

    uint8_t odd_req[40];
    memset(odd_req, 0, sizeof(odd_req));
    mw_put_be32(odd_req, MW_BLIT);
    mw_put_be32(odd_req + 4, 4); /* RGBFB_R5G6B5PC */
    mw_put_be32(odd_req + 8, odd_pitch);
    mw_put_be32(odd_req + 12, 0);
    mw_put_be32(odd_req + 16, 0);
    mw_put_be32(odd_req + 20, 1);
    mw_put_be32(odd_req + 24, 0);
    mw_put_be32(odd_req + 28, 0);
    mw_put_be32(odd_req + 32, 32);
    mw_put_be32(odd_req + 36, 32);

    ULONG odd_desc[2] = {(ULONG)(uintptr_t)odd_vram, odd_total};
    st = transport->blit(handle, odd_req, sizeof(odd_req), odd_desc);
    assert(st == 0);

    /* Verify Red in RGB565PC: r=31, g=0, b=0 -> 0xf800 -> LE: byte0=0x00, byte1=0xf8 */
    for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
            uint8_t *px = odd_vram + y * odd_pitch + x * 2;
            assert(px[0] == 0x00 && px[1] == 0xf8);
        }
    }

    transport->destroy(handle);
    transport->term();
    printf("PASS: test_mw_e2e_blit_and_capture\n");
}


static void test_scheduler_core1_initialization_preserves_wire_v9(void)
{
    taskq_shared_t scheduler_shared;
    volatile ZZ9KMiniGLControlBlockV9 *ctrl;
    volatile ZZ9KMiniGLStatusV9 *status;
    uint8_t control_host_bytes[64];
    uint8_t status_host_bytes[64];

    setup_mock_card(1);
    stop_core1();

    ctrl = (volatile ZZ9KMiniGLControlBlockV9 *)g_active_ctrl_base;
    status = (volatile ZZ9KMiniGLStatusV9 *)((uint8_t *)g_active_ctrl_base +
                                             sizeof(*ctrl));

    /*
     * The scheduler's worker startup republishes the firmware-owned half of
     * this window. Preserve the host-owned halves so a second service at this
     * DDR base cannot silently replace the Wire-v9 control/status blocks.
     */
    memset((uint8_t *)ctrl + 64, 0xa5, sizeof(control_host_bytes));
    memset((uint8_t *)status + 64, 0x5a, sizeof(status_host_bytes));
    memcpy(control_host_bytes, (const uint8_t *)ctrl + 64,
           sizeof(control_host_bytes));
    memcpy(status_host_bytes, (const uint8_t *)status + 64,
           sizeof(status_host_bytes));

    memset(&scheduler_shared, 0, sizeof(scheduler_shared));
    scheduler_shared.core1_current_slot = -1;
    scheduler_core1_host_test_init(&scheduler_shared);

    assert(scheduler_shared.core1_alive == 1);
    assert(ctrl->magic == MW_SERVICE_MAGIC);
    assert(ctrl->transport_version == MW_SERVICE_TRANSPORT_VER);
    assert(ctrl->wire_version == MW_SERVICE_WIRE_VER);
    assert(ctrl->slot_capacity == MW_DEFAULT_SLOT_CAPACITY_Z3);
    assert(ctrl->slot_count == MW_DEFAULT_SLOT_COUNT_Z3);
    assert(status->capabilities == (1U << 18));
    assert(memcmp(control_host_bytes, (const uint8_t *)ctrl + 64,
                  sizeof(control_host_bytes)) == 0);
    assert(memcmp(status_host_bytes, (const uint8_t *)status + 64,
                  sizeof(status_host_bytes)) == 0);

    start_core1();
    printf("PASS: test_scheduler_core1_initialization_preserves_wire_v9\n");
}


int main(void)
{
    printf("Starting ZZ9000 MiniGL Wire-v9 Transport E2E Integration Tests...\n");

    test_probe_and_handshake();
    test_scheduler_core1_initialization_preserves_wire_v9();
    test_create_and_destroy();
    test_submit_flow_control();
    test_lost_on_generation_change();
    test_request_with_reply();
    test_readback();
    test_z2_packet_capacity();
    test_back_to_back_and_pipeline();
    test_mw_e2e_blit_and_capture();

    stop_core1();
    free_card_memory();

    printf("ALL ZZ9000 MiniGL Transport E2E Integration Tests PASSED!\n");
    return 0;
}
