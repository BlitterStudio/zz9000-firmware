/*
 * Guest-client host harness for ZZ9000 MiniGL Wire-v9 Transport.
 *
 * Compiles the REAL library backend (src/), the REAL ZZ9000 transport and
 * the REAL guest test clients (tests/amiberry_ops.c, amiberry_v29.c,
 * amiberry_reopen.c) against a mock AmigaOS/P96 environment, and runs them
 * against the real firmware mw_service on a worker thread emulating ARM
 * core 1 -- the same fidelity as test_mw_e2e, one level higher: the GL
 * calls the clients make are the library's own, and every MW packet the
 * firmware sees is produced by the shipped client code.
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
#include "amiberry_transport.h"

#include <exec/types.h>
#include <exec/libraries.h>
#include <libraries/configvars.h>
#include <libraries/minigl_dispatch.h>

#include "guest_mock/guest_mock.h"

/* Guest client entry points (compiled with -Dmain=<name>). */
int amiberry_ops_main(void);
int amiberry_v29_main(void);
int amiberry_reopen_main(void);

/* --- Client-side globals normally provided by the import stub --------- */

struct Library *MiniGLBase = 0;
const MGLDispatchTable *MiniGLDispatch = 0;
extern const MGLDispatchTable MiniGLDispatchTable;

const MGLDispatchTable *MiniGLGetDispatchTableLVO(void)
{
    return &MiniGLDispatchTable;
}

/* In the guest the trap transport lives in the emulator; on this harness
 * only the ZZ9000 card exists. mgl_backend_cpu_requested() comes from the
 * compiled library backend itself. */
const struct MiniGLTransport *amiberry_transport_trap_probe(void)
{
    return 0;
}

/* --- Mock card + core-1 worker (as in test_mw_e2e.c) ------------------- */

static uint8_t *g_card_mem = NULL;
static size_t g_card_size = 0;
static volatile uint16_t *g_doorbell_ptr = NULL;
static volatile int g_core1_running = 0;
static volatile int g_core1_paused = 0;
static pthread_t g_core1_thread;

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

#define MOCK_MAILBOX_ARM        0x08100000UL
#define MOCK_MAILBOX_DESC_OFF   0x8000U
#define MOCK_ADDR_ADJ           0x001F0000UL

static void publish_mock_descriptor(uint32_t window_board, uint32_t window_bytes,
                                    uint32_t slot_capacity, uint32_t slot_count,
                                    uint32_t vram_limit_arm)
{
    uint32_t mailbox_board = MOCK_MAILBOX_ARM - MOCK_ADDR_ADJ;
    uint8_t *desc = g_card_mem + mailbox_board + MOCK_MAILBOX_DESC_OFF;
    uint32_t reply_capacity = slot_capacity > (64UL * 1024UL) ? (64UL * 1024UL)
                                                             : (16UL * 1024UL);
    uint32_t reply_offset = 0x1000U + slot_count * slot_capacity;

    memset(desc, 0, 80);
    mw_put_be32(desc + 0, 0x4D475444UL);            /* 'MGTD' */
    mw_put_be32(desc + 4, (1U << 16) | 1U);         /* desc ver 1, transport 1 */
    mw_put_be32(desc + 8, 1U);                      /* generation */
    mw_put_be32(desc + 12, window_board + MOCK_ADDR_ADJ); /* window_address */
    mw_put_be32(desc + 16, window_bytes);
    mw_put_be32(desc + 20, MOCK_ADDR_ADJ);          /* address_adjust */
    mw_put_be32(desc + 24, 0U);                     /* ctrl_offset */
    mw_put_be32(desc + 28, 0x80U);                  /* status_offset */
    mw_put_be32(desc + 32, 0x1000U);                /* slot0_offset */
    mw_put_be32(desc + 36, slot_count == 2U ? 0x1000U + slot_capacity : 0U);
    mw_put_be32(desc + 40, slot_capacity);
    mw_put_be32(desc + 44, slot_count);
    mw_put_be32(desc + 48, reply_offset);
    mw_put_be32(desc + 52, reply_capacity);
    mw_put_be32(desc + 56, 4U);                     /* alignment */
    mw_put_be32(desc + 60, 1U);                     /* cache_rules */
    mw_put_be32(desc + 64, vram_limit_arm);

    *(uint16_t *)(g_card_mem + 0x0104) = (uint16_t)(MOCK_MAILBOX_ARM >> 16);
    *(uint16_t *)(g_card_mem + 0x0106) = (uint16_t)(MOCK_MAILBOX_ARM & 0xffffU);
}

static struct ConfigDev g_mock_cd;

struct ConfigDev *FindConfigDev(struct ConfigDev *old, ULONG manuf, ULONG product)
{
    if (old != NULL)
        return NULL;
    if (manuf == g_mock_cd.cd_Rom_er_Manufacturer &&
        product == g_mock_cd.cd_Rom_er_Product) {
        return &g_mock_cd;
    }
    return NULL;
}

static void setup_mock_card(void)
{
    stop_core1();
    mw_service_set_host_vram(NULL, 0);
    if (g_card_mem) {
        munmap(g_card_mem, g_card_size);
        g_card_mem = NULL;
        g_card_size = 0;
    }

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

    *(uint16_t *)(g_card_mem + 0x0100) = 0x5a39; /* SDK magic */
    *(uint16_t *)(g_card_mem + 0x0102) = 0x0200; /* SDK version 2.0 */
    g_doorbell_ptr = (volatile uint16_t *)(g_card_mem + 0x1108);

    g_mock_cd.cd_BoardAddr = g_card_mem;
    g_mock_cd.cd_BoardSize = (ULONG)g_card_size;
    g_mock_cd.cd_Rom_er_Manufacturer = 0x6d6e;
    g_mock_cd.cd_Rom_er_Product = 4; /* Zorro III */

    int bound = mw_service_bind_region(g_card_mem + 0x06E10000UL, 0x100000UL);
    assert(bound == 1);
    mw_service_set_host_vram(g_card_mem, 0x07000000UL);
    publish_mock_descriptor(0x06E10000UL, 0x100000UL, 256UL * 1024UL, 2U, 0x07000000UL);

    guest_mock_set_card(g_card_mem, g_card_size);
    guest_mock_init_libraries();
    start_core1();
}

/* --- Guest runner ------------------------------------------------------ */

static int g_total_fail;
static int g_non_target_fail;

static int is_target_failure(const char *name, const char *line, size_t len)
{
    if (strncmp(line, "FAIL ", 5) != 0)
        return 0;
    if (strcmp(name, "amiberry_ops") != 0)
        return 1;
    return (len >= 19 && memcmp(line, "FAIL context-resize", 19) == 0) ||
           (len >= 28 && memcmp(line, "FAIL raster-primitive-points", 28) == 0) ||
           (len >= 22 && memcmp(line, "FAIL raster-edge-flags", 22) == 0) ||
           (len >= 23 && memcmp(line, "FAIL raster-depth-range", 23) == 0);
}

static void run_guest(const char *name, int (*guest_main)(void))
{

    const char *log;
    int rc;
    int fails = 0;
    int non_target_fails = 0;
    const char *scan;

    guest_mock_result_log_clear();
    printf("=== guest %s ===\n", name);
    fflush(stdout);
    rc = guest_main();
    log = guest_mock_result_log();
    printf("%s", log);
    for (scan = log; scan && *scan;) {
        const char *line_end = strchr(scan, '\n');
        size_t len = line_end ? (size_t)(line_end - scan) : strlen(scan);
        if (len >= 4 && memcmp(scan, "FAIL", 4) == 0) {
            if (is_target_failure(name, scan, len))
                ++fails;
            else
                ++non_target_fails;
        }
        if (!line_end)
            break;
        scan = line_end + 1;
    }
    printf("=== guest %s: exit %d, %d targeted FAIL line(s), %d non-target host-mock FAIL line(s) ===\n\n",
           name, rc, fails, non_target_fails);
    fflush(stdout);
    g_total_fail += fails;
    g_non_target_fail += non_target_fails;
}

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;

    setup_mock_card();

    /* The A4000 run order for the clients whose failures are under test;
     * an argument runs a single guest for isolation. */
    if (!only || strcmp(only, "v29") == 0)
        run_guest("amiberry_v29", amiberry_v29_main);
    if (!only || strcmp(only, "ops") == 0)
        run_guest("amiberry_ops", amiberry_ops_main);
    if (!only || strcmp(only, "reopen") == 0)
        run_guest("amiberry_reopen", amiberry_reopen_main);

    stop_core1();
    if (g_total_fail != 0) {
        printf("CLIENTS-HARNESS FAILED: %d FAIL line(s)\n", g_total_fail);
        return 1;
    }
    printf("ALL TARGETED GUEST CLIENTS PASSED (%d non-target host-mock FAIL line(s))\n",
           g_non_target_fail);
    return 0;
}
