/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "card_pool.h"
#include "host_card_pool.h"
#include "memorymap.h"
#include "surface_allocator.h"
#include "sdk_crypto.h"
#include "sdk_mailbox.h"
#include "zzm_manifest.h"
#include "zzm_loader.h"
#include "zzm_minigl_host.h"
#include "zzm_runtime.h"
#include "ff.h"
#include "aar/app.h"
#include "aar/ext_zz9000.h"
#include "zzapp_services.h"
#include "mw_ctx.h"
#include "minigl_platform.h"
#include "rix_drv.h"
#include "minigl_rix_backend.h"
#include "rix_cmd.h"
#include "mw_service.h"
#include "minigl_raster.h"


extern int zzm_module_entry(const zzm_services_t *services);
extern void zzm_module_exit(void);
extern mw_ctx *amiberry_transport_inproc_ctx(void);
extern int zzapp_gl_set_module_host(const zzm_app_gl_host_service_t *host);
void zzapp_gl_test_set_p96(uint8_t *pixels, uint32_t bytes);
void zzapp_gl_test_reset_invalidates(void);
int minigl_raster_test_level_size(uint32_t id, uint16_t level,
                                  uint16_t *width, uint16_t *height);
uint32_t zzapp_gl_test_invalidates(void);
int zzapp_gl_test_copy_bound(const uint32_t *src, int rgb565,
                             uint32_t w, uint32_t h, uint32_t pitch,
                             uint32_t format, uint32_t card, uint32_t layout);
/* Wire-level transport helpers (pointer-compatible signatures; the real
 * declarations live in the frontend's private header). */
extern struct AmiberryContext *amiberry_context(void *context);
extern uint8_t *amiberry_command(struct AmiberryContext *context,
                                 uint32_t opcode, uint32_t payload);
extern uint8_t amiberry_flush(struct AmiberryContext *context);
extern void *mini_CurrentContext;


static int reject_module_open;
static int admit_reset_on_register_ops;
static int admit_reset_on_aperture_query;
static unsigned reset_admissions;
static unsigned aperture_queries;
#define TEST_MODULE_IMAGE_BYTES (48U * 1024U)
#define TEST_MODULE_BSS_BYTES (128U * 1024U)
static uint8_t module_file[ZZM_V3_HEADER_SIZE + TEST_MODULE_IMAGE_BYTES];
static uint32_t module_offset;
static unsigned module_open_calls;
static unsigned module_close_calls;
static unsigned module_read_calls;
static unsigned module_read_fail_call;
static uint32_t module_max_read_request;
static int track_module_load;
static unsigned module_flush_calls;
static unsigned module_icache_calls;
static uint32_t module_max_flush;
static uint32_t module_max_icache;
static unsigned module_load_passes;
static uint64_t now_us;
static uint32_t display_width = 640U;
static uint32_t display_height = 480U;
static uint32_t display_pitch = 640U * 4U;
static uint32_t display_format = 2U;
static uint32_t display_front = 0x08000000U;
static uint32_t display_back = 0x08000000U;
static uint32_t display_layout = 5U;
static int display_latest;
static const uint32_t mailbox_pages[3] = { 0x08002100U, 0x08004100U, 0x08006100U };
static uint32_t mailbox_spare, mailbox_pending;
static uint32_t mailbox_presented[16];
static unsigned mailbox_presents, mailbox_cleans, mailbox_invalidates;
static int mailbox_model, mailbox_leased, mailbox_vblank_on_yield;
static uint32_t mailbox_stale_address;
static uint16_t mailbox_current_pixel;
static int flip_idle = 1;
static int hold_flip;
static unsigned wait_yields;
static int g_yield_runs_worker;
static unsigned flip_waits;
/* Set while an app GL call is inside the module (host api_enter/leave):
 * device accesses made then are counted. */
static int g_in_gl_call;
static unsigned g_gl_call_device_ops;
static int g_test_rix_claim_fails;
/* Everything the module logged through the runtime's log service. */
static char g_log[16384];
static size_t g_log_used;


int smp_cpu_id(void) { return 1; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1U; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word) return 0;
    *word = 1U;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0U; }

void Xil_DCacheFlushRange(uint32_t address, uint32_t length)
{
    if (track_module_load && address >= ZZM_MINIGL_SLOT_ADDRESS &&
        address < ZZM_MINIGL_SLOT_ADDRESS + ZZM_MINIGL_SLOT_SIZE) {
        module_flush_calls++;
        if (length > module_max_flush)
            module_max_flush = length;
    }
}
void Xil_DCacheInvalidateRange(uint32_t address, uint32_t length)
{ (void)address; (void)length; }
void Xil_ICacheInvalidateRange(uint32_t address, uint32_t length)
{
    if (track_module_load && address >= ZZM_MINIGL_SLOT_ADDRESS &&
        address < ZZM_MINIGL_SLOT_ADDRESS + ZZM_MINIGL_SLOT_SIZE) {
        module_icache_calls++;
        if (length > module_max_icache)
            module_max_icache = length;
    }
}
/* MEM -> ST1 envelopes the FrameStreamingCore has accepted but not yet
 * executed.  Normally they run at their TLF; a test can hold them (the GPU is
 * still fetching) until a deadline, or forever (a wedged GPU). */
#define TEST_FETCH_QUEUE 64U
static uint32_t g_fetch_address[TEST_FETCH_QUEUE];
static uint32_t g_fetch_bytes[TEST_FETCH_QUEUE];
static unsigned g_fetch_count;
static int g_fetch_hold;
static int g_fetch_wedged;
static uint64_t g_fetch_release_us;
static void test_rix_run_fetches(void);
void XTime_GetTime(uint64_t *time)
{
    *time = now_us++;
    if (g_fetch_hold && !g_fetch_wedged && now_us >= g_fetch_release_us) {
        g_fetch_hold = 0;
        test_rix_run_fetches();
    }
}
int sdk_aperture_runtime_is_zorro3(void)
{
    ++aperture_queries;
    if (admit_reset_on_aperture_query) {
        admit_reset_on_aperture_query = 0;
        ++reset_admissions;
        sdk_mailbox_begin_amiga_reset();
    }
    return 1;
}
int zzm_test_printf(const char *format, ...)
{
    va_list args;
    va_list copy;
    int result;

    va_start(args, format);
    va_copy(copy, args);
    result = vfprintf(stderr, format, args);
    if (g_log_used < sizeof(g_log) - 1U) {
        int added = vsnprintf(g_log + g_log_used, sizeof(g_log) - g_log_used,
                              format, copy);

        if (added > 0)
            g_log_used += (size_t)added < sizeof(g_log) - g_log_used ?
                          (size_t)added : sizeof(g_log) - g_log_used - 1U;
    }
    va_end(copy);
    va_end(args);
    return result;
}

static void test_capture_rix_log(const char *fmt, ...)
{
    va_list args;
    int added;

    va_start(args, fmt);
    if (g_log_used < sizeof(g_log) - 1U) {
        added = vsnprintf(g_log + g_log_used, sizeof(g_log) - g_log_used,
                          fmt, args);
        if (added > 0)
            g_log_used += (size_t)added < sizeof(g_log) - g_log_used ?
                          (size_t)added : sizeof(g_log) - g_log_used - 1U;
    }
    va_end(args);
}

static int test_rix_caps(zzm_graphics_hw_caps_t *caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->has_rasterix_ef = 1U;
    caps->rasterix_csr_abi_major = 2U;
    caps->pl330_available = 1U;
    return 1;
}

/* Whether `address` still lies in a live (unreleased) card-pool block. */
static int test_card_pool_holds(uint32_t address)
{
    uint32_t index;

    for (index = 0U; index < card_pool.block_count; ++index)
        if (address >= card_pool.blocks[index].addr &&
            address - card_pool.blocks[index].addr < card_pool.blocks[index].size)
            return 1;
    return 0;
}

/* The parser keeps its state across envelopes: a command may span them. */
static struct {
    uint32_t skip;
    int reg;
    uint32_t color_address;
    uint32_t clear_color;
    uint32_t watch_address;
    int watch_hit;
} g_fsc = { 0U, -1, 0U, 0U, 0U, 0 };

static uint32_t g_test_rix_status = RIX_DRV_STATUS_IDLE;
/* While held lists exist, STATUS first reports idle this many times (the
 * fetcher between bursts) and then busy with reads outstanding. */
static unsigned g_test_rix_idle_first;
static unsigned g_test_rix_soft_resets;
static int g_test_rix_has_byte_swap;
static uint32_t g_test_rix_control;
static int test_rix_csr_read(uint32_t offset, uint32_t *value)
{
    if (!value)
        return 0;
    if (g_in_gl_call)
        ++g_gl_call_device_ops;
    if (offset == RIX_DRV_CSR_IDENTITY)
        *value = RIX_DRV_IDENTITY_VALUE;
    else if (offset == RIX_DRV_CSR_VERSION)
        *value = RIX_DRV_VERSION_VALUE;
    else if (offset == RIX_DRV_CSR_STATUS && g_fetch_count != 0U) {
        if (g_test_rix_idle_first != 0U) {
            --g_test_rix_idle_first;
            *value = RIX_DRV_STATUS_IDLE;
        } else {
            *value = RIX_DRV_STATUS_BUSY | (2U << 16);
        }
    } else if (offset == RIX_DRV_CSR_STATUS)
        *value = g_test_rix_status;
    else if (offset == RIX_DRV_CSR_CONTROL)
        *value = g_test_rix_control;
    else
        *value = 0U;
    return 1;
}

static int test_rix_csr_write(uint32_t offset, uint32_t value)
{
    if (g_in_gl_call)
        ++g_gl_call_device_ops;
    if (offset == RIX_DRV_CSR_CONTROL) {
        if (!g_test_rix_has_byte_swap)
            value &= ~RIX_DRV_CONTROL_COLOR_SWAP;
        assert(!((value ^ g_test_rix_control) & RIX_DRV_CONTROL_COLOR_SWAP) ||
               g_fetch_count == 0U);
        g_test_rix_control = value;
    }
    if (offset == RIX_DRV_CSR_CONTROL &&
        (value & RIX_DRV_CONTROL_SOFT_RESET) != 0U) {
        /* Only a wedged GPU may be reset while it still owns a fetch. */
        assert(g_fetch_count == 0U || g_fetch_wedged);
        g_fetch_count = 0U;
        g_fsc.skip = 0U;
        g_fsc.reg = -1;
        ++g_test_rix_soft_resets;
    }
    return 1;
}

static unsigned g_test_rix_full_reads;
/* Every TDFV read reports a full FIFO until cleared: the app's own swap and
 * GL-call feed steps then leave the frame queued. */
#define TEST_FIFO_FULL_READS 1000000U
static int test_rix_fifo_read(uint32_t offset, uint32_t *value)
{
    if (!value)
        return 0;
    if (g_in_gl_call)
        ++g_gl_call_device_ops;
    if (offset == RIX_DRV_FIFO_TDFV) {
        /* A temporarily full FIFO models back-pressure: try_feed must block
         * until the engine drains, which is the worker-context wait path. */
        if (g_test_rix_full_reads != 0U) {
            --g_test_rix_full_reads;
            *value = 2U;
        } else {
            *value = 508U;
        }
    } else {
        *value = 0U;
    }
    return 1;
}

static unsigned long g_test_rix_tlf_bytes;
static uint32_t g_test_rix_packet[2];
static uint32_t g_test_rix_packet_words;
static unsigned g_test_rix_triangles;

static void test_rix_fsc_word(uint32_t word)
{
    if (g_fsc.skip != 0U) {
        --g_fsc.skip;
        return;
    }
    if (g_fsc.reg >= 0) {
        if ((uint32_t)g_fsc.reg == RIX_REG_COLOR_BUFFER_ADDR)
            g_fsc.color_address = word;
        else if ((uint32_t)g_fsc.reg == RIX_REG_CLEAR_COLOR)
            g_fsc.clear_color = word;
        g_fsc.reg = -1;
        return;
    }
    switch (word >> 28U) {
    case 1U:
        g_fsc.reg = (int)(word & 0xffU);
        break;
    case 2U:
        if ((word & (RIX_FRAMEBUFFER_MEMSET | RIX_FRAMEBUFFER_COLOR)) ==
            (RIX_FRAMEBUFFER_MEMSET | RIX_FRAMEBUFFER_COLOR) &&
            g_fsc.color_address != 0U) {
            if (g_fsc.color_address == g_fsc.watch_address)
                g_fsc.watch_hit = 1;
            uint16_t pixel = (uint16_t)(((g_fsc.clear_color >> 27U) << 11U) |
                (((g_fsc.clear_color >> 18U) & 63U) << 5U) |
                ((g_fsc.clear_color >> 11U) & 31U));
            if (mailbox_leased) {
                assert(g_fsc.color_address != display_front);
                assert(g_fsc.color_address != mailbox_pending);
            }
            if (g_test_rix_control & RIX_DRV_CONTROL_COLOR_SWAP)
                pixel = __builtin_bswap16(pixel);
            *(uint16_t *)(uintptr_t)g_fsc.color_address = pixel;
        }
        break;
    case 3U:
        ++g_test_rix_triangles;
        g_fsc.skip = (word & 0x3ffU) / sizeof(uint32_t);
        break;
    case 4U:
        g_fsc.skip = 64U;
        break;
    case 5U:
        g_fsc.skip = word & 0x7ffffU;
        break;
    default:
        break;
    }
}

static void test_rix_run_fetches(void)
{
    unsigned entry;

    for (entry = 0U; entry < g_fetch_count; ++entry) {
        const uint32_t *words = (const uint32_t *)(uintptr_t)g_fetch_address[entry];
        uint32_t index;

        for (index = 0U; index < g_fetch_bytes[entry] / sizeof(uint32_t); ++index)
            test_rix_fsc_word(words[index]);
    }
    g_fetch_count = 0U;
}

static void test_rix_run_memfetch(uint32_t address, uint32_t bytes)
{
    assert(g_fetch_count < TEST_FETCH_QUEUE);
    g_fetch_address[g_fetch_count] = address;
    g_fetch_bytes[g_fetch_count] = bytes;
    ++g_fetch_count;
    if (!g_fetch_hold)
        test_rix_run_fetches();
}
static int test_rix_write(uint32_t offset, uint32_t value)
{
    if (g_in_gl_call)
        ++g_gl_call_device_ops;
    if (offset == RIX_DRV_FIFO_TDFR) {
        g_test_rix_packet_words = 0U;
    } else if (offset == RIX_DRV_FIFO_TDFD) {
        if (g_test_rix_packet_words < 2U)
            g_test_rix_packet[g_test_rix_packet_words] = value;
        ++g_test_rix_packet_words;
    } else if (offset == RIX_DRV_FIFO_TLF) {
        g_test_rix_tlf_bytes += value;
        if (g_test_rix_packet_words == 2U &&
            (g_test_rix_packet[0] & 0xf0000000U) ==
                RIX_DRV_MEMFETCH_ENVELOPE) {
            assert(value == 2U * sizeof(uint32_t));
            test_rix_run_memfetch(g_test_rix_packet[1],
                                  g_test_rix_packet[0] & 0x0fffffffU);
        } else {
            minigl_raster_test_retire_fences();
        }
        g_test_rix_packet_words = 0U;
    }
    return 1;
}

static int test_rix_write_words(const uint32_t *words, uint32_t count)
{
    (void)words;
    (void)count;
    return 1;
}

static unsigned g_test_rix_claims;
static int test_rix_claim(uint32_t owner, uint32_t *channel)
{
    (void)owner;
    if (g_test_rix_claim_fails)
        return 0;
    *channel = 6U;
    ++g_test_rix_claims;
    return 1;
}

static unsigned g_test_rix_start_calls = 0;
static int g_test_rix_start_result = RIX_DRV_DMA_COMPLETE;
static int g_test_rix_complete_result = RIX_DRV_DMA_COMPLETE;

static int test_rix_start(uint32_t channel, uint32_t source, uint32_t bytes)
{
    (void)channel;
    (void)source;
    (void)bytes;
    ++g_test_rix_start_calls;
    if (g_in_gl_call)
        ++g_gl_call_device_ops;
    if (g_test_rix_start_result == RIX_DRV_DMA_COMPLETE)
        minigl_raster_test_retire_fences();
    return g_test_rix_start_result;
}

static int test_rix_complete(uint32_t channel)
{
    (void)channel;
    if (g_in_gl_call)
        ++g_gl_call_device_ops;
    if (g_test_rix_complete_result == RIX_DRV_DMA_COMPLETE)
        minigl_raster_test_retire_fences();
    return g_test_rix_complete_result;
}

static int test_rix_abort(uint32_t channel)
{
    (void)channel;
    return RIX_DRV_DMA_COMPLETE;
}

static void test_rix_barrier(void)
{
}

static const zzm_graphics_hw_service_t test_rix_service = {
    ZZM_SERVICE_GRAPHICS_HW_V3, sizeof(test_rix_service), test_rix_caps,
    test_rix_csr_read, test_rix_csr_write, test_rix_fifo_read, test_rix_write,
    test_rix_write_words, test_rix_claim, test_rix_start, test_rix_start,
    test_rix_complete, 0, 0, test_rix_barrier, test_rix_complete,
    test_rix_start, test_rix_abort, 0
};
static const zzm_graphics_hw_service_t old_rix_service = {
    ZZM_SERVICE_GRAPHICS_HW_V3,
    offsetof(zzm_graphics_hw_service_t, barrier) + sizeof(test_rix_service.barrier),
    test_rix_caps, test_rix_csr_read, test_rix_csr_write, test_rix_fifo_read, test_rix_write,
    test_rix_write_words, test_rix_claim, test_rix_start, test_rix_start,
    test_rix_complete, 0, 0, test_rix_barrier, 0, 0, 0, 0
};
static zzm_graphics_hw_caps_t graphics_caps = {
    1U, 2U, 0U, 1U, 1U
};
void zzapp_api_enter(void) { g_in_gl_call = 1; }
void zzapp_api_leave(void) { g_in_gl_call = 0; }
uint32_t zzapp_current_generation(void) { return 1U; }
int zzapp_display_bound(uint32_t *width, uint32_t *height, uint32_t *pitch,
                        uint32_t *format, uint32_t *front, uint32_t *back)
{
    *width = display_width;
    *height = display_height;
    *pitch = display_pitch;
    *format = display_format;
    *front = display_front;
    *back = display_back;
    return 0;
}
int zzapp_display_layout(uint32_t *layout)
{
    *layout = display_layout;
    return 0;
}
/* What the client's copy of the frame shows: the first RGB565 pixel of the
 * presented buffer, taken at each flip (a frame is visible once flipped). */
static const uint8_t *g_flip_src;
static uint8_t g_shown[2];
static unsigned g_flips;
int zzapp_svc_display_flip(uint32_t surface_id)
{
    (void)surface_id;
    ++g_flips;
    if (g_flip_src)
        memcpy(g_shown, g_flip_src, sizeof(g_shown));
    if (hold_flip)
        flip_idle = 0;
    return 0;
}

int zzapp_svc_display_flip_async(uint32_t surface_id)
{
    return zzapp_svc_display_flip(surface_id);
}

int zzapp_display_flip_idle(void)
{
    return flip_idle;
}
/* The host's registered flip wait: the flip completes while the app is
 * parked in it. */
int zzapp_display_wait_flip_idle(void)
{
    ++flip_waits;
    flip_idle = 1;
    return 0;
}
int zzapp_display_latest(void) { return display_latest; }

int zzapp_display_mailbox_buffers(uint32_t buffers[3], uint32_t *bytes)
{
    if (!mailbox_model)
        return AAR_E_INVAL;
    memcpy(buffers, mailbox_pages, sizeof(mailbox_pages));
    *bytes = display_height * display_pitch;
    return AAR_OK;
}

int zzapp_display_mailbox_owned(uint32_t *back, uint32_t *spare)
{
    assert(mailbox_model);
    *back = display_back;
    *spare = mailbox_spare;
    return AAR_OK;
}

int zzapp_display_mailbox_lease(int acquire)
{
    assert(mailbox_model && mailbox_leased != acquire);
    if (!acquire)
        assert(g_fetch_count == 0U);
    mailbox_leased = acquire;
    return AAR_OK;
}

int zzapp_display_mailbox_present(uint32_t address, uint32_t flags)
{
    uint32_t next = mailbox_pending;

    assert(mailbox_leased && address && address != display_front &&
           address != mailbox_pending);
    assert(address == display_back || address == mailbox_spare);
    if (address == mailbox_spare)
        mailbox_spare = display_back;
    if (!next)
        next = mailbox_spare;
    assert(next && next != address);
    display_back = next;
    mailbox_spare = 0U;
    mailbox_pending = address;
    assert(mailbox_presents < sizeof(mailbox_presented) / sizeof(mailbox_presented[0]));
    mailbox_presented[mailbox_presents++] = address;
    if (!(flags & ZZAPP_DISPLAY_PRESENT_NO_DIRTY))
        ++mailbox_cleans;
    return AAR_OK;
}

int zzapp_display_invalidate(uint32_t address, uint32_t bytes)
{
    if (!mailbox_leased || (address != mailbox_pages[0] &&
        address != mailbox_pages[1] && address != mailbox_pages[2]))
        return AAR_E_INVAL;
    assert(bytes == display_pitch * display_height);
    ++mailbox_invalidates;
    if (mailbox_stale_address == address) {
        *(uint16_t *)(uintptr_t)address = mailbox_current_pixel;
        mailbox_stale_address = 0U;
    }
    return AAR_OK;
}

static void test_mailbox_vblank(void)
{
    assert(mailbox_pending && !mailbox_spare);
    mailbox_spare = display_front;
    display_front = mailbox_pending;
    mailbox_pending = 0U;
}
int zzapp_services_stop_requested(void) { return 0; }
void zzapp_host_yield_wait(void)
{
    ++wait_yields;
    flip_idle = 1;
    if (mailbox_vblank_on_yield) {
        mailbox_vblank_on_yield = 0;
        test_mailbox_vblank();
    }
    if (g_yield_runs_worker) {
        g_yield_runs_worker = 0;
        (void)zzm_minigl_service_tick(0);
    }
}

int zzm_registry_register_ops(uint32_t module_id, const void *ops, uint32_t bytes)
{
    if (module_id != ZZM_MODULE_MINIGL || ops == NULL || bytes == 0U)
        return 0;
    if (admit_reset_on_register_ops) {
        admit_reset_on_register_ops = 0;
        ++reset_admissions;
        sdk_mailbox_begin_amiga_reset();
    }
    return 1;
}

int zzm_registry_register_service(uint32_t module_id, uint32_t id,
                                  uint32_t version, const void *interface,
                                  uint32_t bytes)
{
    return module_id == ZZM_MODULE_MINIGL && id != 0U && version != 0U &&
           interface != NULL && bytes != 0U;
}

FRESULT f_open(FIL *file, const char *path, unsigned char mode)
{
    (void)mode;
    if (strstr(path, "/MINIGL.ZZM") == NULL || reject_module_open)
        return FR_NO_FILE;
    if (track_module_load)
        module_open_calls++;
    file->size = sizeof(module_file);
    module_offset = 0U;
    return FR_OK;
}
FRESULT f_lseek(FIL *file, FSIZE_t offset)
{
    if (offset > file->size) return FR_NOT_READY;
    module_offset = offset;
    return FR_OK;
}
FRESULT f_read(FIL *file, void *buffer, UINT bytes, UINT *read)
{
    if (track_module_load) {
        module_read_calls++;
        if (bytes > module_max_read_request)
            module_max_read_request = bytes;
        if (module_read_fail_call == module_read_calls) {
            *read = 0U;
            return FR_NOT_READY;
        }
    }
    if (module_offset > file->size || bytes > file->size - module_offset) {
        *read = 0U;
        return FR_NOT_READY;
    }
    memcpy(buffer, module_file + module_offset, bytes);
    module_offset += bytes;
    *read = bytes;
    return FR_OK;
}
FRESULT f_close(FIL *file)
{
    (void)file;
    if (track_module_load)
        module_close_calls++;
    return FR_OK;
}

void sdk_sha256_init(sdk_sha256_context_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state[0] = 2166136261U;
}

void sdk_sha256_update(sdk_sha256_context_t *ctx, const uint8_t *data,
                       uint32_t bytes)
{
    uint32_t i;

    for (i = 0U; i < bytes; i++) {
        ctx->state[0] = (ctx->state[0] ^ data[i]) * 16777619U;
        ctx->state[1] += (uint32_t)data[i] + (uint32_t)ctx->bit_length;
        ctx->bit_length += 8U;
    }
}

void sdk_sha256_final(sdk_sha256_context_t *ctx, uint8_t digest[32])
{
    uint32_t i;

    for (i = 0U; i < 32U; i++) {
        uint32_t value = ctx->state[i & 1U] ^
            (uint32_t)(ctx->bit_length >> ((i & 7U) * 8U));
        digest[i] = (uint8_t)(value >> ((i & 3U) * 8U));
    }
    memset(ctx, 0, sizeof(*ctx));
}

void sdk_sha256(const uint8_t *data, uint32_t bytes, uint8_t digest[32])
{
    sdk_sha256_context_t ctx;

    sdk_sha256_init(&ctx);
    sdk_sha256_update(&ctx, data, bytes);
    sdk_sha256_final(&ctx, digest);
}

static void emit_trampoline(uint8_t *code, const void *target)
{
    uintptr_t address = (uintptr_t)target;
    code[0] = 0x48U; code[1] = 0xb8U;
    memcpy(code + 2U, &address, sizeof(address));
    code[10] = 0xffU; code[11] = 0xe0U;
}

static void prepare_module_file(void)
{
    zzm_v3_header_t *header = (zzm_v3_header_t *)module_file;

    memset(module_file, 0, sizeof(module_file));
    memcpy(header->magic, ZZM_V3_MAGIC, 4U);
    header->header_size = ZZM_V3_HEADER_SIZE;
    header->format_version = ZZM_V3_FORMAT_VERSION;
    header->module_id = ZZM_MODULE_MINIGL;
    header->abi_major = ZZM_V3_ABI_MAJOR;
    header->slot_load_address = ZZM_MINIGL_SLOT_ADDRESS;
    header->image_size = TEST_MODULE_IMAGE_BYTES;
    header->bss_size = TEST_MODULE_BSS_BYTES;
    header->init_entry = ZZM_MINIGL_SLOT_ADDRESS;
    header->fini_entry = ZZM_MINIGL_SLOT_ADDRESS + 16U;
    header->load_policy = ZZM_LOAD_ON_DEMAND;
    header->idle_unload_ms = ZZM_DEFAULT_IDLE_UNLOAD_MS;
    header->image_flags = ZZM_IMAGE_NEEDS_CORE1 | ZZM_IMAGE_NEEDS_HOST_VISIBLE |
                          ZZM_IMAGE_NEEDS_VFP_NEON;
    header->owner_class_mask = ZZM_OWNER_CLASS_MINIGL_CONTEXT |
                               ZZM_OWNER_CLASS_MINIGL_APP_CONTEXT |
                               ZZM_OWNER_CLASS_MINIGL_TRANSPORT |
                               ZZM_OWNER_CLASS_MINIGL_RIX;
    header->hardware_requirement = ZZM_HARDWARE_RASTERIX_EF_OPTIONAL;
    header->required_caps.wire_version = 9U;
    header->required_caps.transport_descriptor_major = 1U;
    header->required_caps.host_window_layout_major = ZZM_HOST_WINDOW_LAYOUT_MAJOR;
    header->resident_quota = 65536U;
    header->per_context_quota = CARD_POOL_LIMIT_MINIGL;
    header->worker_ops_version = ZZM_MINIGL_WORKER_OPS_V1;
    memcpy(header->name, "MINIGL", 7U);
    emit_trampoline(module_file + ZZM_V3_HEADER_SIZE, zzm_module_entry);
    emit_trampoline(module_file + ZZM_V3_HEADER_SIZE + 16U, zzm_module_exit);
    sdk_sha256(module_file + ZZM_V3_HEADER_SIZE, header->image_size,
               header->image_sha256);
}

static void setup_host_memory(void)
{
    void *mapped;

    mapped = mmap((void *)(uintptr_t)0x08000000U, 0x00200000U,
                  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                  -1, 0);
    assert(mapped == (void *)(uintptr_t)0x08000000U);
    mapped = mmap((void *)(uintptr_t)ZZM_MINIGL_SLOT_ADDRESS,
                  ZZM_MINIGL_SLOT_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(mapped == (void *)(uintptr_t)ZZM_MINIGL_SLOT_ADDRESS);
    host_card_pool_reset(CARD_POOL_LIMIT_MINIGL + (64U << 20));
    surface_allocator_init(HOST_VISIBLE_HEAP_ADDRESS, HOST_VISIBLE_HEAP_SIZE);
}
#define TEST_V3_READ_HASH_PASS_BYTES (12U * 1024U)
#define TEST_V3_MEMORY_PASS_BYTES (64U * 1024U)

static void assert_load_work_bounded(void)
{
    uint32_t bytes = zzm_loader_test_work_bytes();

    switch (zzm_loader_test_work_kind()) {
    case ZZM_LOADER_TEST_WORK_NONE:
        assert(bytes == 0U);
        break;
    case ZZM_LOADER_TEST_WORK_READ_HASH:
        assert(bytes <= TEST_V3_READ_HASH_PASS_BYTES);
        break;
    case ZZM_LOADER_TEST_WORK_BSS_ZERO:
    case ZZM_LOADER_TEST_WORK_DCACHE_FLUSH:
    case ZZM_LOADER_TEST_WORK_ICACHE_INVALIDATE:
    case ZZM_LOADER_TEST_WORK_POISON:
        assert(bytes <= TEST_V3_MEMORY_PASS_BYTES);
        break;
    }
}

static int acquire_module_bounded(void)
{
    zzm_status_t status;
    int result;

    module_open_calls = 0U;
    module_close_calls = 0U;
    module_read_calls = 0U;
    module_max_read_request = 0U;
    module_flush_calls = 0U;
    module_icache_calls = 0U;
    module_max_flush = 0U;
    module_max_icache = 0U;
    module_load_passes = 0U;
    track_module_load = 1;
    do {
        result = zzm_loader_acquire(ZZM_MODULE_MINIGL);
        module_load_passes++;
        assert(module_load_passes < 128U);
        assert_load_work_bounded();
        if (result == ZZM_LOAD_IN_PROGRESS) {
            assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
            assert(status.state == ZZM_STATE_LOADING);
            assert(status.ref_count == 0U);
            assert(!zzm_minigl_worker_registered());
        }
    } while (result == ZZM_LOAD_IN_PROGRESS);
    track_module_load = 0;
    assert(module_max_read_request <= TEST_V3_READ_HASH_PASS_BYTES);
    assert(module_max_flush <= TEST_V3_MEMORY_PASS_BYTES);
    assert(module_max_icache <= TEST_V3_MEMORY_PASS_BYTES);
    return result;
}

static uint16_t mailbox_acquire_bounded(uint32_t *final_result_len)
{
    uint8_t result_payload[sizeof(uint32_t)];
    uint32_t result_len;
    uint16_t result;
    unsigned passes = 0U;

    do {
        result_len = 0U;
        result = sdk_mailbox_minigl_acquire_on_owner(result_payload,
                                                      &result_len);
        assert(++passes < 128U);
        assert_load_work_bounded();
    } while (result == SDK_TASK_IN_PROGRESS);
    if (final_result_len)
        *final_result_len = result_len;
    return result;
}

static void assert_module_poisoned(void)
{
    const zzm_v3_header_t *header = (const zzm_v3_header_t *)module_file;
    const uint32_t *word = (const uint32_t *)(uintptr_t)ZZM_MINIGL_SLOT_ADDRESS;
    uint32_t words = (header->image_size + header->bss_size) / sizeof(*word);
    uint32_t i;

    for (i = 0U; i < words; i++)
        assert(word[i] == ZZM_POISON_WORD);
}

static void prepare_available_module(void)
{
    zzm_status_t status;

    reject_module_open = 0;
    module_read_fail_call = 0U;
    prepare_module_file();
    zzm_loader_boot_load();
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_AVAILABLE);
    assert(status.ref_count == 0U);
}

static void assert_cancelled_acquire_cleanup(void)
{
    zzm_status_t status;
    uint32_t generation;

    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_AVAILABLE);
    assert(status.ref_count == 0U);
    assert(!zzm_minigl_worker_registered());
    assert(!zzm_minigl_transport_acquire(&generation));
    assert_module_poisoned();
}

static void test_reset_during_final_module_init(void)
{
    uint32_t result_len = ~(uint32_t)0;

    prepare_available_module();
    reset_admissions = 0U;
    aperture_queries = 0U;
    admit_reset_on_register_ops = 1;
    assert(mailbox_acquire_bounded(&result_len) == SDK_STATUS_IO_ERROR);
    assert(result_len == 0U);
    assert(reset_admissions == 1U);
    assert(aperture_queries == 0U);
    sdk_mailbox_finish_amiga_reset();
}

static void test_reset_during_transport_acquire(void)
{
    uint32_t result_len = ~(uint32_t)0;
    uint32_t allocated_before;

    prepare_available_module();
    allocated_before = surface_allocator_used_bytes();
    reset_admissions = 0U;
    admit_reset_on_aperture_query = 1;
    assert(mailbox_acquire_bounded(&result_len) == SDK_STATUS_IO_ERROR);
    assert(result_len == 0U);
    assert(reset_admissions == 1U);
    assert(surface_allocator_used_bytes() == allocated_before);
    assert_cancelled_acquire_cleanup();
    sdk_mailbox_finish_amiga_reset();
}

static void test_reset_releases_public_acquire_reference(void)
{
    zzm_status_t status;

    prepare_available_module();
    assert(mailbox_acquire_bounded(NULL) == SDK_STATUS_OK);
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_LOADED);
    assert(status.ref_count == 1U);

    sdk_mailbox_begin_amiga_reset();
    assert(zzm_minigl_begin_reset() == ZZM_MINIGL_RESET_QUIESCENT);
    sdk_mailbox_minigl_reset_release_all();
    sdk_mailbox_finish_amiga_reset();
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_AVAILABLE);
    assert(status.ref_count == 0U);

    assert(mailbox_acquire_bounded(NULL) == SDK_STATUS_OK);
    assert(sdk_mailbox_minigl_release_on_owner());
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.ref_count == 0U);

    assert(mailbox_acquire_bounded(NULL) == SDK_STATUS_OK);
    assert(sdk_mailbox_minigl_release_on_owner());
    sdk_mailbox_begin_amiga_reset();
    assert(zzm_minigl_begin_reset() == ZZM_MINIGL_RESET_QUIESCENT);
    sdk_mailbox_minigl_reset_release_all();
    sdk_mailbox_finish_amiga_reset();
    assert(!sdk_mailbox_minigl_release_on_owner());
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_AVAILABLE);
    assert(status.ref_count == 0U);
}

static void release_transport_from_worker_poll(void)
{
    assert(zzm_minigl_transport_release());
    zzm_loader_release(ZZM_MODULE_MINIGL);
}

static void test_reset_cancels_resumable_load(void)
{
    zzm_status_t status;
    uint32_t generation;

    reject_module_open = 0;
    module_read_fail_call = 0U;
    prepare_module_file();
    zzm_loader_boot_load();
    {
        uint8_t result_payload[sizeof(uint32_t)];
        uint32_t result_len = 0U;

        assert(sdk_mailbox_minigl_acquire_on_owner(
                   result_payload, &result_len) == SDK_TASK_IN_PROGRESS);
        assert(sdk_mailbox_minigl_acquire_on_owner(
                   result_payload, &result_len) == SDK_TASK_IN_PROGRESS);
        assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
        assert(status.state == ZZM_STATE_LOADING);
        assert(status.ref_count == 0U);

        sdk_mailbox_begin_amiga_reset();
        assert(sdk_mailbox_minigl_acquire_on_owner(
                   result_payload, &result_len) == SDK_STATUS_IO_ERROR);
    }
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_AVAILABLE);
    assert(status.ref_count == 0U);
    assert(!zzm_minigl_worker_registered());
    assert(!zzm_minigl_transport_acquire(&generation));
    assert_module_poisoned();

    sdk_mailbox_finish_amiga_reset();
    assert(mailbox_acquire_bounded(NULL) == SDK_STATUS_OK);
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_LOADED);
    assert(status.ref_count == 1U);
    assert(zzm_minigl_begin_reset() == ZZM_MINIGL_RESET_QUIESCENT);
    sdk_mailbox_minigl_reset_release_all();
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.ref_count == 0U);
}

/* The first RGB565 pixel (big endian) of seven distinct clear colours. */
static const struct {
    float rgb[3];
    uint8_t pixel[2];
} swap_colours[7] = {
    { { 1.0f, 0.0f, 0.0f }, { 0xF8U, 0x00U } },
    { { 0.0f, 1.0f, 0.0f }, { 0x07U, 0xE0U } },
    { { 0.0f, 0.0f, 1.0f }, { 0x00U, 0x1FU } },
    { { 1.0f, 1.0f, 1.0f }, { 0xFFU, 0xFFU } },
    { { 1.0f, 0.0f, 1.0f }, { 0xF8U, 0x1FU } },
    { { 0.0f, 1.0f, 1.0f }, { 0x07U, 0xFFU } },
    { { 1.0f, 1.0f, 0.0f }, { 0xFFU, 0xE0U } },
};

/* AAR spec 8.5 (conformance C-GL.5): when swap n returns, the frame of swap
 * n - 2 (or a later one) is visible to the client.  Seven frames with distinct
 * clear colours make that exact.  The binding rotates three colour targets, so
 * a context drawing the front target (the front end's default for the
 * in-process context) puts each frame's draws into the previous frame's
 * buffer and the buffer a swap seals stays at its initial fill: the client
 * then sees an older colour for ever.  The GPU and the flip may be slow.
 * Frames 8 to 10 name the front buffer, which must still draw into the frame
 * being built. */
static void test_gl_swap_presents(const zzm_minigl_ops_v1_t *ops, void *context,
                                  int slow_gpu, int slow_flip)
{
    uint8_t frame[64];
    const void *gl = NULL;
    const aar_gl *draw;
    unsigned n;
    unsigned k;

    memset(frame, 0, sizeof(frame));
    assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                        ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
    display_width = 4U;
    display_height = 2U;
    display_front = 0x08000000U;
    display_back = 0x08100000U;
    display_latest = 0;
    display_format = 1U;
    display_layout = 1U;
    display_pitch = 8U;
    zzapp_gl_test_set_p96(frame, sizeof(frame));
    g_flip_src = frame;
    memset(g_shown, 0, sizeof(g_shown));
    hold_flip = slow_flip;
    flip_idle = 1;
    assert(ops->app_bind_flags(context, 1U, 0U, &gl) == 0 && gl != NULL);
    draw = (const aar_gl *)gl;
    for (n = 1U; n <= 10U; n++) {
        /* Frames 8 to 10 name the front buffer and repeat one colour. */
        const unsigned colour = n <= 7U ? n - 1U : 0U;

        if (n == 8U)
            draw->DrawBuffer(0x0404U);
        draw->ClearColor(swap_colours[colour].rgb[0], swap_colours[colour].rgb[1],
                         swap_colours[colour].rgb[2], 1.0f);
        draw->Clear(0x4000U);
        if (slow_gpu) {
            g_fetch_hold = 1;
            g_fetch_release_us = now_us + 40U;
        }
        assert(ops->app_swap(context) == 0);
        if (n < 3U)
            continue;
        for (k = n - 2U; k <= n; k++) {
            const unsigned shown = k <= 7U ? k - 1U : 0U;

            if (memcmp(g_shown, swap_colours[shown].pixel, sizeof(g_shown)) == 0)
                break;
        }
        assert(k <= n);
    }
    ops->app_unbind(context);
    g_fetch_hold = 0;
    hold_flip = 0;
    flip_idle = 1;
    g_flip_src = NULL;
    zzapp_gl_test_set_p96(NULL, 0U);
}

static void expect_clip_pixel(const aar_gl *draw, int x, int y, int red)
{
    uint8_t pixel[4] = { 1U, 2U, 3U, 4U };

    draw->ReadPixels(x, y, 1, 1, 0x1908U, 0x1401U, pixel); /* RGBA8 */
    assert(pixel[0] == (red ? 255U : 0U));
    assert(pixel[1] == 0U && pixel[2] == 0U && pixel[3] == 255U);
}

static void test_app_user_clipping(const aar_gl *draw)
{
    const float vertices[8] = { -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f };
    const uint16_t indices[6] = { 0U, 1U, 2U, 0U, 2U, 3U };
    const double right[4] = { 1.0, 0.0, 0.0, 0.0 };
    const double upper[4] = { 0.0, 1.0, 0.0, 0.0 };
    const uint32_t plane0 = 0x3000U, plane5 = 0x3005U;
    int32_t max_planes = -1, enabled = -1;
    int x_middle = (int)display_width * 9 / 16;
    int x_right = (int)display_width * 3 / 4;
    int y_lower = (int)display_height / 4;
    int y_upper = (int)display_height * 3 / 4;
    int y_point = (int)display_height * 7 / 8;

    assert(draw->ClipPlane != NULL);
    draw->GetIntegerv(0x0d32U, &max_planes); /* GL_MAX_CLIP_PLANES */
    assert(max_planes == 6 && !draw->IsEnabled(plane0));

    /* The right half-space starts at NDC x = +1/4, even after the
     * modelview used to define the plane goes away. */
    draw->Translatef(0.25f, 0.0f, 0.0f);
    draw->ClipPlane(plane0, right);
    draw->LoadIdentity();
    draw->ClipPlane(plane5, upper);
    draw->Enable(plane0);
    draw->Enable(plane5);
    draw->GetIntegerv(plane5, &enabled);
    assert(draw->IsEnabled(plane0) && enabled == 1);
    draw->ClipPlane(plane5 + 1U, upper);
    assert(draw->GetError() == 0x0500U); /* GL_INVALID_ENUM */
    draw->Begin(4U);
    draw->ClipPlane(plane0, upper);
    draw->Disable(plane0);
    draw->End();
    assert(draw->GetError() == 0x0502U); /* GL_INVALID_OPERATION */

    draw->ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    draw->Clear(0x4000U);
    draw->Color3f(1.0f, 0.0f, 0.0f);
    draw->VertexPointer(2, 0x1406U, 0, vertices);
    draw->EnableClientState(0x8074U);
    draw->DrawElements(4U, 6, 0x1403U, indices);
    expect_clip_pixel(draw, x_middle, y_upper, 0);
    expect_clip_pixel(draw, x_right, y_lower, 0);
    expect_clip_pixel(draw, x_right, y_upper, 1);

    /* A segment can cross the plane even when its first endpoint is
     * clipped. Points outside the same half-space must not rasterize. */
    draw->Clear(0x4000U);
    draw->LineWidth(3.0f);
    draw->Begin(1U); /* GL_LINES */
    draw->Vertex2f(-0.75f, 0.5f);
    draw->Vertex2f(0.75f, 0.5f);
    draw->End();
    expect_clip_pixel(draw, x_middle, y_upper, 0);
    expect_clip_pixel(draw, x_right, y_upper, 1);
    draw->LineWidth(1.0f);
    draw->PointSize(3.0f);
    draw->Begin(0U); /* GL_POINTS */
    draw->Vertex2f(0.125f, 0.75f);
    draw->Vertex2f(0.5f, 0.75f);
    draw->End();
    expect_clip_pixel(draw, x_middle, y_point, 0);
    expect_clip_pixel(draw, x_right, y_point, 1);
    draw->PointSize(1.0f);

    /* A clipped polygon's outline includes the new edge. Point mode
     * also draws the new vertex, not just surviving input vertices. */
    draw->Disable(plane5);
    for (uint32_t mode = 0x1b00U; mode <= 0x1b01U; ++mode) {
        int x_clip = (int)display_width * 5 / 8;
        int y_sample = mode == 0x1b00U ? y_lower : (int)display_height / 2;

        draw->Clear(0x4000U);
        draw->PolygonMode(0x0408U, mode); /* GL_FRONT_AND_BACK */
        draw->PointSize(3.0f);
        draw->LineWidth(3.0f);
        draw->Begin(4U);
        draw->Vertex2f(-0.75f, -0.5f);
        draw->Vertex2f(0.75f, -0.5f);
        draw->Vertex2f(0.75f, 0.5f);
        draw->End();
        expect_clip_pixel(draw, x_clip, y_sample, 1);
        expect_clip_pixel(draw, x_middle, y_sample, 0);
    }

    /* Smooth point mode uses the clipped vertex color. Flat point mode
     * still uses the original triangle's provoking vertex. */
    draw->PolygonMode(0x0408U, 0x1b00U); /* GL_POINT */
    for (uint32_t shade = 0x1d00U; shade <= 0x1d01U; ++shade) {
        uint8_t pixel[4] = { 0 };
        const int smooth = shade == 0x1d01U;

        draw->ShadeModel(shade);
        draw->Clear(0x4000U);
        draw->Begin(4U);
        draw->Color3f(1.0f, 0.0f, 0.0f);
        draw->Vertex2f(0.0f, -0.5f);
        draw->Color3f(0.0f, 1.0f, 0.0f);
        draw->Vertex2f(0.5f, -0.5f);
        draw->Color3f(0.0f, 0.0f, 1.0f);
        draw->Vertex2f(0.5f, 0.5f);
        draw->End();
        draw->ReadPixels(x_right, y_lower, 1, 1, 0x1908U, 0x1401U, pixel);
        assert(pixel[0] == 0U && pixel[1] == (smooth ? 255U : 0U) &&
               pixel[2] == (smooth ? 0U : 255U));
        draw->ReadPixels((int)display_width * 5 / 8, y_lower,
                         1, 1, 0x1908U, 0x1401U, pixel);
        if (smooth)
            assert(pixel[0] >= 127U && pixel[0] <= 128U &&
                   pixel[1] >= 127U && pixel[1] <= 128U && pixel[2] == 0U);
        else
            assert(pixel[0] == 0U && pixel[1] == 0U && pixel[2] == 255U);
    }
    draw->Color3f(1.0f, 0.0f, 0.0f);

    draw->PolygonMode(0x0408U, 0x1b02U); /* GL_FILL */
    draw->PointSize(1.0f);
    draw->LineWidth(1.0f);

    draw->Disable(plane0);
    draw->Disable(plane5);
    draw->DrawElements(4U, 6, 0x1403U, indices);
    expect_clip_pixel(draw, x_middle, y_upper, 1);
    expect_clip_pixel(draw, x_right, y_lower, 1);
    draw->DisableClientState(0x8074U);
    draw->VertexPointer(2, 0x1406U, 0, NULL);
    assert(draw->GetError() == 0U);
}

static void test_rix_user_clipping(const aar_gl *draw)
{
    const double right[4] = { 1.0, 0.0, 0.0, 0.0 };
    unsigned before = g_test_rix_triangles;

    /* One outside and two inside: clipping must send two triangles to
     * RasterIX, not draw the original triangle or go CPU. */
    draw->ClipPlane(0x3000U, right);
    draw->Enable(0x3000U);
    draw->Begin(4U);
    draw->Vertex2f(-0.25f, -0.78f);
    draw->Vertex2f(0.25f, -0.78f);
    draw->Vertex2f(0.25f, -0.76f);
    draw->End();
    draw->Finish();
    assert(g_test_rix_triangles - before == 2U);

    /* The first packed strip triangle is inside. The next shares two
     * vertices and must clip them using eye coordinates, not projection. */
    {
        struct AmiberryContext *guest = amiberry_context(mini_CurrentContext);
        const double left[4] = { -1.0, 0.0, 0.0, 0.0 };
        const float packed[] = {
            1.0f, 0.0f, 0.0f, 1.0f, /* constant RGBA */
            0.0f, 0.0f, 0.0f, 1.0f, /* texture coordinates 0 */
            0.0f, 0.0f, 0.0f, 1.0f, /* texture coordinates 1 */
            0.0f, 0.0f, 1.0f,       /* normal */
            -0.125f, -0.78f, 0.0f,
            -0.25f, -0.78f, 0.0f,
            -0.125f, -0.76f, 0.0f,
             0.25f, -0.76f, 0.0f
        };
        uint8_t *payload;

        draw->ClipPlane(0x3000U, left);
        before = g_test_rix_triangles;
        payload = amiberry_command(guest, MW_DRAW_PACKED, 12U + sizeof(packed));
        assert(payload != NULL);
        mw_put_be32(payload, 5U); /* GL_TRIANGLE_STRIP */
        mw_put_be32(payload + 4U, 4U);
        mw_put_be32(payload + 8U, 0x001U); /* XYZ and per-draw constants */
        for (unsigned i = 0U; i < sizeof(packed) / sizeof(packed[0]); ++i)
            mw_put_bef(payload + 12U + 4U * i, packed[i]);
        assert(amiberry_flush(guest));
        draw->Finish();
        assert(g_test_rix_triangles - before == 3U);
    }
    draw->Disable(0x3000U);
}

static void test_direct_mailbox(const zzm_minigl_ops_v1_t *ops, void *context)
{
    const zzm_app_gl_host_service_t *host = zzm_runtime_services()->get_service(
        ZZM_SERVICE_APP_GL_HOST, ZZM_SERVICE_APP_GL_HOST_V4_1);
    zzm_app_gl_host_service_t old_host = *host;
    const uint32_t front = mailbox_pages[0], back = mailbox_pages[1], spare = mailbox_pages[2];
    const uint32_t direct = AAR_ZZ9000_GL_BIND_DIRECT_PRESENT;
    const void *gl = NULL;
    const aar_gl *draw;
    mw_ctx *mw;
    unsigned yields, index;
    uint32_t active;
    uint8_t rgba[4];

    display_width = 4U;
    display_height = 4U;
    display_pitch = 8U;
    display_format = AAR_FMT_RGB565;
    display_layout = AAR_LAYOUT_RGB565_BE;
    display_front = front;
    display_back = back;
    mailbox_spare = spare;
    display_latest = mailbox_model = 1;
    assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                        ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));

    /* With a full mailbox table but no readback bit, BE stays on copy. */
    assert(ops->app_bind_flags(context, 1U, direct, &gl) == AAR_OK);
    assert(!amiberry_transport_inproc_ctx()->present_select && !mailbox_leased);
    ops->app_unbind(context);

    /* A short V4.0 table still binds, even on hardware that supports BE. */
    g_test_rix_has_byte_swap = 1;
    old_host.version = ZZM_SERVICE_APP_GL_HOST_V4;
    old_host.size = offsetof(zzm_app_gl_host_service_t, mailbox_buffers);
    assert(zzapp_gl_set_module_host(&old_host) == AAR_OK);
    assert(ops->app_bind_flags(context, 1U, direct, &gl) == AAR_OK);
    assert(!amiberry_transport_inproc_ctx()->present_select && !mailbox_leased);
    ops->app_unbind(context);
    assert(zzapp_gl_set_module_host(host) == AAR_OK);

    /* A tightly packed frame that ends partway through a cache line cannot
     * be leased for direct CPU/GPU handoff. */
    display_height = 2U;
    assert(ops->app_bind_flags(context, 1U, direct, &gl) == AAR_OK);
    assert(!amiberry_transport_inproc_ctx()->present_select && !mailbox_leased);
    ops->app_unbind(context);
    display_height = 4U;

    assert(ops->app_bind_flags(context, 1U, direct, &gl) == AAR_OK);
    draw = gl;
    mw = amiberry_transport_inproc_ctx();
    assert(mw->direct_color_be && mailbox_leased);
    assert((uintptr_t)minigl_raster_color_buffer() == back);
    draw->ClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    draw->Clear(0x4000U);
    assert(ops->app_swap(context) == AAR_OK);
    assert(mailbox_pending == back && mailbox_presents == 1U);
    assert(memcmp((void *)(uintptr_t)back, swap_colours[0].pixel, 2U) == 0);
    assert((uintptr_t)minigl_raster_color_buffer() == spare);

    /* Pending frame A is taken while B's GPU list is still held. The swap
     * can give back the freed front without waiting for B's GPU writes. */
    g_fetch_hold = mailbox_vblank_on_yield = 1;
    g_fetch_release_us = UINT64_MAX;
    yields = wait_yields;
    draw->ClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    draw->Clear(0x4000U);
    assert(ops->app_swap(context) == AAR_OK);
    assert(wait_yields == yields + 1U && g_fetch_count != 0U);
    assert(mailbox_presents == 1U && display_front == back && !mailbox_pending);
    assert((uintptr_t)minigl_raster_color_buffer() == front);

    /* Fast GPU: finish B and C; both make it to the mailbox in order. */
    g_fetch_hold = 0;
    test_rix_run_fetches();
    draw->ClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    draw->Clear(0x4000U);
    assert(ops->app_swap(context) == AAR_OK);
    assert(mailbox_presents == 3U && mailbox_pending == front);
    assert(mailbox_presented[1] == spare && mailbox_presented[2] == front);
    assert(memcmp((void *)(uintptr_t)spare, swap_colours[1].pixel, 2U) == 0);
    assert(memcmp((void *)(uintptr_t)front, swap_colours[2].pixel, 2U) == 0);
    assert(!mailbox_cleans && !mailbox_invalidates);
    test_mailbox_vblank();

    /* A CPU read invalidates when needed and leaves the draw target direct.
     * Make the CPU's old view different from what the GPU wrote. */
    active = (uint32_t)(uintptr_t)minigl_raster_color_buffer();
    draw->ClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    draw->Clear(0x4000U);
    draw->Finish();
    assert(memcmp((void *)(uintptr_t)active, swap_colours[0].pixel, 2U) == 0);
    mailbox_stale_address = active;
    mailbox_current_pixel = *(uint16_t *)(uintptr_t)active;
    *(uint16_t *)(uintptr_t)active = 0U;
    memset(rgba, 0, sizeof(rgba));
    draw->ReadPixels(0, display_height - 1U, 1, 1, 0x1908U, 0x1401U, rgba);
    assert(rgba[0] == 255U && rgba[1] == 0U && rgba[2] == 0U && rgba[3] == 255U);
    assert(mailbox_invalidates == 1U && !mailbox_stale_address);
    assert(mw->color_direct[mw->state.target.back]);
    assert(ops->app_swap(context) == AAR_OK);
    assert(mailbox_invalidates == 1U && !mailbox_cleans);
    test_mailbox_vblank();

    /* A CPU-only draw sees the current direct pixels and demotes only its
     * own target. Its swap copies the shadow into that display page. */
    index = mw->state.target.back;
    active = (uint32_t)(uintptr_t)mw->direct_color_buffers[index];
    draw->ClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    draw->Clear(0x4000U);
    draw->Finish();
    mw->state.force_cpu = 1U;
    draw->Clear(0x4000U);
    draw->Finish();
    assert(!mw->color_direct[index] && mailbox_invalidates == 2U);
    mw->state.force_cpu = 0U;
    assert(ops->app_swap(context) == AAR_OK);
    assert(mailbox_cleans == 1U && mailbox_pending == active);
    assert(memcmp((void *)(uintptr_t)active, swap_colours[1].pixel, 2U) == 0);
    test_mailbox_vblank();

    /* An in-flight frame plus an owned spare must not block swap. Unbind
     * drains it before calling the host's lease-release callback. */
    g_fetch_hold = 1;
    g_fetch_release_us = UINT64_MAX;
    yields = wait_yields;
    draw->Clear(0x4000U);
    assert(ops->app_swap(context) == AAR_OK);
    assert(g_fetch_count && wait_yields == yields);
    g_fetch_release_us = now_us + 4U;
    ops->app_unbind(context);
    assert(!mailbox_leased && !g_fetch_count && !mailbox_pending);
    assert(card_pool.block_count == 0U);

    mailbox_model = display_latest = 0;
    g_test_rix_has_byte_swap = 0;
}

/* A colour clear: one command, 48 bytes. */
static void write_clear_packet(uint8_t packet[48])
{
    memset(packet, 0, 48U);
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 8, 48U);
    mw_put_be32(packet + 12, 1U);
    mw_put_be32(packet + 16, MW_CLEAR);
    mw_put_be32(packet + 20, 24U);
    mw_put_be32(packet + 24, 1U);
    mw_put_be32(packet + 28, 0x3f800000U);
    mw_put_be32(packet + 40, 0x3f800000U);
}

/* Clear plus present: the present hands the clear's list to RasterIX. */
static void write_clear_present_packet(uint8_t packet[56])
{
    memset(packet, 0, 56U);
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 8, 56U);
    mw_put_be32(packet + 12, 2U);
    mw_put_be32(packet + 16, MW_CLEAR);
    mw_put_be32(packet + 20, 24U);
    mw_put_be32(packet + 24, 1U);
    mw_put_be32(packet + 28, 0x3f800000U);
    mw_put_be32(packet + 40, 0x3f800000U);
    mw_put_be32(packet + 48, MW_PRESENT);
    mw_put_be32(packet + 52, 0U);
}

int main(void)
{
    zzm_v3_capabilities_t caps;
    zzm_status_t status;
    const zzm_minigl_ops_v1_t *ops;
    void *context;
    zzm_minigl_wire_open_t request = { 640U, 480U, 0U };
    uint32_t generation1;
    uint32_t generation2;
    uint32_t handle;
    const void *gl = NULL;
    void *module_allocation;
    uint32_t quarantined_kept[MINIGL_RIX_COMMAND_BUFFERS + 4U];
    unsigned quarantined_kept_count = 0U;
    uint32_t quarantined_owner = 0U;
    setup_host_memory();
    prepare_module_file();
    memset(&caps, 0, sizeof(caps));
    caps.image_flags = ZZM_IMAGE_NEEDS_CORE1 | ZZM_IMAGE_NEEDS_HOST_VISIBLE |
                       ZZM_IMAGE_NEEDS_VFP_NEON;
    caps.wire_version = 9U;
    caps.transport_descriptor_major = 1U;
    caps.host_window_layout_major = ZZM_HOST_WINDOW_LAYOUT_MAJOR;
    zzm_minigl_boot_caps_init(ZZM_MINIGL_IMAGE_STANDARD);
    zzm_loader_set_v3_capabilities(&caps);
    zzm_loader_boot_load();
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_AVAILABLE);
    memset((void *)(uintptr_t)ZZM_MINIGL_SLOT_ADDRESS, 0xa5,
           TEST_MODULE_IMAGE_BYTES + TEST_MODULE_BSS_BYTES);
    assert(acquire_module_bounded() == ZZM_LOAD_READY);
    assert(module_load_passes > 8U);
    assert(module_open_calls == 1U);
    assert(module_close_calls == 1U);
    assert(module_read_calls == 4U);
    assert(module_flush_calls == 3U);
    assert(module_icache_calls == 1U);
    assert(memcmp((const void *)(uintptr_t)ZZM_MINIGL_SLOT_ADDRESS,
                  module_file + ZZM_V3_HEADER_SIZE, 32U) == 0);
    {
        const uint8_t *bss =
            (const uint8_t *)(uintptr_t)(ZZM_MINIGL_SLOT_ADDRESS +
                                         TEST_MODULE_IMAGE_BYTES);
        uint32_t i;

        for (i = 0U; i < 40U; i++)
            assert(bss[i] == 0U);
    }
    assert(zzm_minigl_worker_registered());
    ops = zzm_minigl_ops(&context);
    assert(ops != NULL && context != NULL);

    assert(zzm_minigl_transport_acquire(&generation1));
    assert(generation1 != 0U);
    assert(card_pool.block_count == 0U);
    assert(ops->open_68k(context, &request, &handle));
    ops->close_68k(context, handle);
    /* The bind reports its present and feed modes through the module log
     * service (rate-limited per second, hence a fresh window). */
    now_us += 2000000U;
    g_log_used = 0U;
    g_log[0] = '\0';
    assert(ops->app_bind(context, 1U, &gl) == 0 && gl != NULL);
    assert(strstr(g_log, "[ZZM] [app] present: copy (direct not requested)") != NULL);
    assert(strstr(g_log, "[ZZM] [app] gl feed: async memfetch") != NULL);
    /* Queued ARM presentation needs two retained frames plus one draw target. */
    assert(amiberry_transport_inproc_ctx()->state.target.buffer_count == 3U);
    {
        const aar_gl *draw = gl;
        const uint8_t *extensions = draw->GetString(0x1f03U); /* GL_EXTENSIONS */
        double depth;
        uint8_t mask[4];

        assert(draw->version == AAR_GL_VERSION_11);
        assert(extensions != NULL && extensions[0] == '\0');
        assert(draw->StencilFunc == NULL);
        draw->ClearDepth(0.25);
        draw->GetDoublev(0x0b73U, &depth); /* GL_DEPTH_CLEAR_VALUE */
        assert(depth == 0.25);
        draw->ClearDepth(1.0);
        draw->ColorMask(0U, 1U, 0U, 1U);
        draw->GetBooleanv(0x0c23U, mask); /* GL_COLOR_WRITEMASK */
        assert(mask[0] == 0U && mask[1] == 1U && mask[2] == 0U && mask[3] == 1U);
        draw->ColorMask(1U, 1U, 1U, 1U);
        /* C-GL.14: no stencil buffer exists, so GL_STENCIL_BITS reads 0 and
         * leaves no GL error. Only that name is special: an unknown name
         * still writes 0 and records GL_INVALID_ENUM. */
        {
            int32_t bits = -1;

            assert(draw->GetError() == 0U);
            draw->GetIntegerv(0x0d57U, &bits); /* GL_STENCIL_BITS */
            assert(bits == 0);
            assert(draw->GetError() == 0U);
            bits = -1;
            draw->GetIntegerv(0xfffeU, &bits);
            assert(bits == 0);
            assert(draw->GetError() == 0x0500U); /* GL_INVALID_ENUM */
        }
    }
    test_app_user_clipping((const aar_gl *)gl);
    {
        const aar_gl *draw = (const aar_gl *)gl;
        uint32_t textures[3];
        uint8_t *pixels = calloc(1U, 64U << 20);

        assert(pixels != NULL);
        draw->Color3f(1.0f, 0.0f, 0.0f);
        draw->Begin(4U); /* GL_TRIANGLES */
        draw->Vertex2f(-0.5f, -0.5f);
        draw->Vertex2f(0.5f, -0.5f);
        draw->Vertex2f(0.0f, 0.5f);
        draw->End();
        draw->GenTextures(3, textures);
        draw->BindTexture(0x0de1U, textures[0]);
        draw->TexImage2D(0x0de1U, 0, 0x1908, 2048, 2048, 0,
                         0x1908U, 0x1401U, pixels);
        draw->BindTexture(0x0de1U, textures[1]);
        draw->TexImage2D(0x0de1U, 0, 0x1908, 4096, 4096, 0,
                         0x1908U, 0x1401U, pixels);
        draw->BindTexture(0x0de1U, textures[2]);
        draw->TexImage2D(0x0de1U, 0, 0x1908, 4096, 4096, 0,
                         0x1908U, 0x1401U, pixels);
        assert(draw->GetError() == 0U);
        draw->ClearColor(0.0f, 0.0f, 1.0f, 1.0f);
        draw->Clear(0x4000U);
        draw->Finish();
        /* TexImage2D auto-enables GL_GENERATE_MIPMAP: the generated chain
         * must reach the rasterizer, so a minified NEAREST_MIPMAP_NEAREST
         * sample of a 2x2 texture (three black, one red texel) comes from
         * the 1x1 level, not a base-level corner. */
        {
            uint32_t mips;
            uint8_t texel[16];
            float quad[16] = {
                -1.0f, -1.0f, 0.0f, 0.0f,
                1.0f, -1.0f, 1.0f, 0.0f,
                -1.0f, 1.0f, 0.0f, 1.0f,
                1.0f, 1.0f, 1.0f, 1.0f,
            };
            const uint32_t *frame;
            mw_ctx *mw;

            memset(texel, 0, sizeof(texel));
            texel[8] = 255U;  /* (1,0) red */
            texel[11] = 255U; /* alpha */
            draw->GenTextures(1, &mips);
            draw->BindTexture(0x0de1U, mips);
            draw->TexParameteri(0x0de1U, 0x2801U /* MIN_FILTER */,
                                0x2702 /* NEAREST_MIPMAP_NEAREST */);
            /* GENERATE_MIPMAP before the image: the raw image handler
             * itself must push the generated chain to the rasterizer. */
            {
                struct AmiberryContext *guest = amiberry_context(mini_CurrentContext);
                uint8_t *param = amiberry_command(guest, MW_TEXTURE_PARAM, 12);

                assert(param != NULL);
                mw_put_be32(param, mips);
                mw_put_be32(param + 4, 0x8191U);
                mw_put_be32(param + 8, 1U);
                assert(amiberry_flush(guest));
            }
            /* Submit the image on the wire too: through the frontend the
             * auto-GENERATE_MIPMAP param after TexImage2D would upload the
             * chain from the param handler and mask the image handler. */
            {
                struct AmiberryContext *guest = amiberry_context(mini_CurrentContext);
                uint8_t *img = amiberry_command(guest, MW_TEXTURE_IMAGE_RAW,
                                                56U + 16U);

                assert(img != NULL);
                mw_put_be32(img, mips);          /* name */
                mw_put_be32(img + 4, 0);         /* level 0 */
                mw_put_be32(img + 8, 4);         /* internal RGBA */
                mw_put_be32(img + 12, 2);        /* width */
                mw_put_be32(img + 16, 2);        /* height */
                mw_put_be32(img + 20, 0x1908U);  /* GL_RGBA */
                mw_put_be32(img + 24, 0x1401U);  /* GL_UNSIGNED_BYTE */
                mw_put_be32(img + 28, 4);        /* alignment */
                mw_put_be32(img + 52, 16U);      /* source bytes */
                memcpy(img + 56, texel, 16U);
                assert(amiberry_flush(guest));
            }
            draw->Enable(0x0de1U /* GL_TEXTURE_2D */);
            draw->ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            draw->Clear(0x4000U);
            draw->VertexPointer(2, 0x1406U /* GL_FLOAT */, 16, quad);
            draw->TexCoordPointer(2, 0x1406U, 16, quad + 2);
            draw->EnableClientState(0x8074U /* VERTEX_ARRAY */);
            draw->EnableClientState(0x8078U /* TEXTURE_COORD_ARRAY */);
            draw->DrawArrays(0x0005U /* TRIANGLE_STRIP */, 0, 4);
            draw->DisableClientState(0x8078U);
            draw->DisableClientState(0x8074U);
            draw->Disable(0x0de1U);
            draw->Finish();
            assert(draw->GetError() == 0U);
            /* The generated 1x1 level must be present in the rasterizer,
             * and a fresh image drops stale deeper levels. */
            {
                uint16_t lw = 0U;
                uint16_t lh = 0U;

                assert(minigl_raster_test_level_size(mips, 1U, &lw, &lh) == 1);
                assert(lw == 1U && lh == 1U);
                lw = 0U;
                lh = 0U;
                assert(minigl_raster_test_level_size(mips, 2U, &lw, &lh) == 0);
            }
            mw = amiberry_transport_inproc_ctx();
            assert(mw != NULL && mw->color_buffers[mw->state.target.back]);
            frame = (const uint32_t *)mw->color_buffers[mw->state.target.back];
            (void)frame;
        }
        assert(ops->app_swap(context) == 0);
        free(pixels);
    }
    assert(ops->app_swap(context) == 0);
    assert(card_pool.class_used[CARD_POOL_CLASS_MINIGL_CONTEXT] >= (128U << 20));
    {
        /* P1: ARM work in flight, then a 68k packet arriving in worker context,
         * with no unbounded loop (count feed steps per poll) and eventual completion.
         * The app's swap feeds its own frame, so the GPU holds the fetched list
         * to keep the work outstanding. */
        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);
        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind_flags(context, 1U, 0U, &gl) == 0 && gl != NULL);
        const aar_gl *draw = (const aar_gl *)gl;
        volatile ZZ9KMiniGLControlBlockV9 *ctrl;
        uint32_t init_consumer;
        uint32_t slot_idx;
        uint8_t *slot_buf;
        unsigned start_calls_before;
        float quad[8] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };

        ZZ9KMiniGLTransportDescriptor desc;
        assert(mw_service_get_transport_descriptor(&desc) == 0);
        ctrl = (volatile ZZ9KMiniGLControlBlockV9 *)(void *)(desc.window_address + desc.ctrl_offset);
        assert(minigl_raster_drain());
        test_rix_user_clipping(draw);
        g_fetch_hold = 1;
        g_fetch_release_us = ~(uint64_t)0;

        /* Draw an ARM frame */
        draw->VertexPointer(2, 0x1406U, 8, quad);
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        draw->DisableClientState(0x8074U);

        assert(ops->app_swap(context) == 0);
        assert(minigl_raster_work_outstanding() != 0);

        init_consumer = mw_wire_ld32(&ctrl->consumer_seq);
        /* 68k packet arrives */
        slot_idx = init_consumer % mw_wire_ld32(&ctrl->slot_count);
        slot_buf = (uint8_t *)(void *)desc.window_address + (slot_idx == 0 ? desc.slot0_offset : desc.slot1_offset);
        mw_put_be32(slot_buf, 0x414D3344U);
        mw_put_be32(slot_buf + 4, 9U);
        mw_put_be32(slot_buf + 8, 16U);
        mw_put_be32(slot_buf + 12, 0U);
        mw_wire_st32((volatile uint32_t *)&ctrl->producer_seq, init_consumer + 1U);

        /* In worker context (system thread), poll must run at most one bounded feed step,
         * defer the 68k packet, and NOT advance consumer_seq */
        start_calls_before = g_test_rix_start_calls;
        zzm_minigl_service_tick(0);
        assert(mw_wire_ld32(&ctrl->consumer_seq) == init_consumer);
        assert(g_test_rix_start_calls <= start_calls_before + 1U);
        assert(minigl_raster_work_outstanding() != 0);

        /* Allow completion and poll again: eventual completion */
        g_fetch_hold = 0;
        test_rix_run_fetches();
        zzm_minigl_service_tick(0);
        while (minigl_raster_work_outstanding())
            zzm_minigl_service_tick(0);
        assert(!minigl_raster_work_outstanding());
        if (mw_wire_ld32(&ctrl->consumer_seq) == init_consumer)
            zzm_minigl_service_tick(0);
        assert(mw_wire_ld32(&ctrl->consumer_seq) == init_consumer + 1U);
    }
    {
        /* A mid-flight async ring drained from worker context completes
         * through bounded feed steps instead of failing into a GL error. */
        extern int zzm_module_can_park(void);
        extern void zzm_module_set_worker_context(int active);
        const aar_gl *draw = (const aar_gl *)gl;
        float quad[8] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };
        unsigned long fed_bytes;

        /* The prior close packet retired the platform; rebind it like a
         * fresh session so this drain drives a live backend. */
        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);
        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind_flags(context, 1U, AAR_ZZ9000_GL_BIND_INLINE_RIX_FEED,
                                   &gl) == 0 && gl != NULL);
        draw = (const aar_gl *)gl;
        assert(minigl_raster_hardware_available());
        assert(minigl_raster_drain());
        g_test_rix_full_reads = TEST_FIFO_FULL_READS;

        draw->VertexPointer(2, 0x1406U, 8, quad);
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        draw->DisableClientState(0x8074U);
        assert(ops->app_swap(context) == 0);
        assert(minigl_raster_feed_pending() != 0);

        g_test_rix_start_result = RIX_DRV_DMA_COMPLETE;
        g_test_rix_complete_result = RIX_DRV_DMA_COMPLETE;
        /* The FIFO stays full for the first reads: the drain must keep
         * stepping (bounded) until vacancy returns. */
        g_test_rix_full_reads = 3U;
        fed_bytes = g_test_rix_tlf_bytes;

        zzm_module_set_worker_context(1);
        assert(zzm_module_can_park() == 0);
        assert(minigl_raster_drain());
        zzm_module_set_worker_context(0);
        assert(!minigl_raster_feed_pending());
        assert(g_test_rix_tlf_bytes > fed_bytes);
    }
    {
        /* A worker-context drain must not call yield_wait while an app GL
         * call remains active. */
        extern int zzm_module_can_park(void);
        extern void zzm_module_api_enter(void);
        extern void zzm_module_api_leave(void);
        extern void zzm_module_set_worker_context(int active);

        zzm_module_api_enter();
        assert(zzm_module_can_park() == 1);

        zzm_module_set_worker_context(1);
        assert(zzm_module_can_park() == 0);

        wait_yields = 0U;
        assert(minigl_raster_drain() != 0);
        assert(wait_yields == 0U);

        zzm_module_set_worker_context(0);
        assert(zzm_module_can_park() == 1);
        zzm_module_api_leave();
    }
    {
        /* The oldest queued frame is presented once RasterIX has finished
         * its own colour target, not once the whole ring is idle: the first
         * two frames' fences have landed, the engine still reports busy, and
         * the swap that follows flips the first frame while the second is
         * still held back. */
        const aar_gl *draw;
        float quad[8] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };
        unsigned flips_before;

        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);
        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind_flags(context, 1U, 0U, &gl) == 0 && gl != NULL);
        draw = (const aar_gl *)gl;
        assert(minigl_raster_drain());
        g_fetch_hold = 1;
        g_fetch_release_us = ~(uint64_t)0;
        flips_before = g_flips;

        draw->VertexPointer(2, 0x1406U, 8, quad);
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        assert(ops->app_swap(context) == 0);
        draw->DrawArrays(0x0005U, 0, 4);
        assert(ops->app_swap(context) == 0);
        draw->DisableClientState(0x8074U);
        assert(minigl_raster_work_outstanding() != 0);
        assert(g_flips == flips_before);

        /* Both lists have been fetched; the engine is not idle. */
        g_fetch_hold = 0;
        test_rix_run_fetches();
        g_test_rix_status = RIX_DRV_STATUS_BUSY;
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        draw->DisableClientState(0x8074U);
        assert(ops->app_swap(context) == 0);
        assert(g_flips == flips_before + 1U);
        assert(minigl_raster_work_outstanding() != 0);

        g_test_rix_status = RIX_DRV_STATUS_IDLE;
        assert(minigl_raster_drain());
    }
    {
        /* The app's GL calls feed its own ring: a call made while a sealed
         * frame waits pushes it on, and a call with nothing queued touches
         * no RasterIX or DMA register. */
        const aar_gl *draw;
        float quad[8] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };

        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);
        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind_flags(context, 1U, AAR_ZZ9000_GL_BIND_INLINE_RIX_FEED,
                                   &gl) == 0 && gl != NULL);
        draw = (const aar_gl *)gl;
        assert(minigl_raster_drain());
        g_test_rix_full_reads = TEST_FIFO_FULL_READS;
        draw->VertexPointer(2, 0x1406U, 8, quad);
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        draw->DisableClientState(0x8074U);
        assert(ops->app_swap(context) == 0);
        assert(minigl_raster_feed_pending() != 0);

        g_test_rix_full_reads = 0U;
        g_gl_call_device_ops = 0U;
        (void)draw->GetError();
        assert(g_gl_call_device_ops != 0U);

        assert(minigl_raster_drain());
        assert(!minigl_raster_feed_pending());
        g_gl_call_device_ops = 0U;
        (void)draw->GetError();
        assert(g_gl_call_device_ops == 0U);
    }
    {
        /* A worker feed step that cannot move (the FIFO is full) is no
         * progress, so the worker may idle instead of retrying; with the
         * app parked in a GL call it keeps feeding. */
        extern void zzm_module_api_enter(void);
        extern void zzm_module_api_leave(void);
        const aar_gl *draw = (const aar_gl *)gl;
        float quad[8] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };

        g_test_rix_full_reads = TEST_FIFO_FULL_READS;
        draw->VertexPointer(2, 0x1406U, 8, quad);
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        draw->DisableClientState(0x8074U);
        assert(ops->app_swap(context) == 0);
        assert(minigl_raster_feed_pending() != 0);
        assert(zzm_minigl_service_tick(0) == 0);
        zzm_module_api_enter();
        assert(zzm_minigl_service_tick(0) == 1);
        zzm_module_api_leave();
        g_test_rix_full_reads = 0U;
        assert(minigl_raster_drain());
    }
    {
        /* A wire command that arrives while an ARM GL call is parked is
         * not dispatched until that call returns, and the call's backend
         * state is unchanged when it resumes. The worker still retires
         * the ring the call is waiting on. */
        extern void zzm_module_api_enter(void);
        extern void zzm_module_api_leave(void);
        const aar_gl *draw = (const aar_gl *)gl;
        float quad[8] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };
        ZZ9KMiniGLTransportDescriptor desc;
        volatile ZZ9KMiniGLControlBlockV9 *ctrl;
        uint32_t *color;
        const uint32_t *bound;
        uint32_t saved;
        uint32_t consumer;
        uint32_t slot_idx;
        uint8_t *slot_buf;
        unsigned step;

        assert(mw_service_get_transport_descriptor(&desc) == 0);
        ctrl = (volatile ZZ9KMiniGLControlBlockV9 *)(void *)(
            desc.window_address + desc.ctrl_offset);
        assert(minigl_raster_drain());
        while (mw_service_has_work())
            (void)zzm_minigl_service_tick(0);
        g_fetch_hold = 1;
        g_fetch_release_us = ~(uint64_t)0;
        draw->VertexPointer(2, 0x1406U, 8, quad);
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        draw->DisableClientState(0x8074U);
        assert(ops->app_swap(context) == 0);
        assert(minigl_raster_work_outstanding() != 0);
        bound = minigl_raster_color_buffer();
        assert(bound != NULL);
        color = (uint32_t *)(uintptr_t)bound;
        saved = color[0];
        color[0] = 0x11223344U;

        zzm_module_api_enter();
        consumer = mw_wire_ld32(&ctrl->consumer_seq);
        slot_idx = consumer % mw_wire_ld32(&ctrl->slot_count);
        slot_buf = (uint8_t *)(void *)desc.window_address +
            (slot_idx == 0U ? desc.slot0_offset : desc.slot1_offset);
        mw_put_be32(slot_buf, 0x414D3344U);
        mw_put_be32(slot_buf + 4, 9U);
        mw_put_be32(slot_buf + 8, 16U);
        mw_put_be32(slot_buf + 12, 0U);
        mw_wire_st32((volatile uint32_t *)&ctrl->producer_seq, consumer + 1U);
        assert(mw_service_has_work());
        g_fetch_hold = 0;
        test_rix_run_fetches();
        for (step = 0U; step < 8U && minigl_raster_work_outstanding(); ++step)
            (void)zzm_minigl_service_tick(0);
        (void)zzm_minigl_service_tick(0);
        assert(!minigl_raster_work_outstanding());
        assert(mw_wire_ld32(&ctrl->consumer_seq) == consumer);
        assert(color[0] == 0x11223344U);
        assert(minigl_raster_color_buffer() == bound);
        g_yield_runs_worker = 1;
        zzm_module_api_leave();
        assert(mw_wire_ld32(&ctrl->consumer_seq) == consumer + 1U);
        color[0] = saved;
        g_fetch_count = 0U;
    }
    {
        /* Forced reclamation drops a parked GL stack, so it must clear the
         * module API/deferred-dispatch state and let retained wire work run. */
        extern void zzm_module_api_enter(void);
        extern int zzm_module_api_active(void);
        ZZ9KMiniGLTransportDescriptor desc;
        volatile ZZ9KMiniGLControlBlockV9 *ctrl;
        uint32_t consumer;
        uint32_t slot_idx;
        uint8_t *slot_buf;

        assert(mw_service_get_transport_descriptor(&desc) == 0);
        ctrl = (volatile ZZ9KMiniGLControlBlockV9 *)(void *)(
            desc.window_address + desc.ctrl_offset);
        consumer = mw_wire_ld32(&ctrl->consumer_seq);
        slot_idx = consumer % mw_wire_ld32(&ctrl->slot_count);
        slot_buf = (uint8_t *)(void *)desc.window_address +
            (slot_idx == 0U ? desc.slot0_offset : desc.slot1_offset);
        zzm_module_api_enter();
        mw_put_be32(slot_buf, 0x414D3344U);
        mw_put_be32(slot_buf + 4, 9U);
        mw_put_be32(slot_buf + 8, 16U);
        mw_put_be32(slot_buf + 12, 0U);
        mw_wire_st32((volatile uint32_t *)&ctrl->producer_seq, consumer + 1U);
        assert(zzm_minigl_service_tick(0) == 0);
        assert(zzm_module_api_active());
        assert(mw_wire_ld32(&ctrl->consumer_seq) == consumer);
        ops->app_reclaim(context, 1U);
        assert(!zzm_module_api_active());
        assert(zzm_minigl_service_tick(0) != 0);
        assert(mw_wire_ld32(&ctrl->consumer_seq) == consumer + 1U);
    }
    {
        /* MW_PRESENT rotates from A's submitted target to a fresh target.
         * B must retire A's fence before replacing backend owner state, for
         * both the async app ring and a synchronous wire context. */
        const aar_gl *draw;
        float quad[8] = { -0.5f, -0.5f, 0.5f, -0.5f,
                          0.5f, 0.5f, -0.5f, 0.5f };
        uint32_t handle_a = 0U;
        uint32_t handle_b = 0U;
        uint8_t present[24] = { 0U };
        mw_ctx *wire;
        unsigned step;

        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind_flags(context, 1U, 0U, &gl) == 0 && gl != NULL);
        draw = (const aar_gl *)gl;
        draw->VertexPointer(2, 0x1406U, 8, quad);
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        draw->DisableClientState(0x8074U);
        g_fetch_hold = 1;
        g_fetch_release_us = ~(uint64_t)0;
        assert(ops->app_swap(context) == 0);
        for (step = 0U; step < 64U && minigl_raster_feed_pending(); ++step)
            (void)minigl_raster_try_feed(RIX_DRV_FEED_SOURCE_SCHEDULER);
        assert(g_fetch_count != 0U);
        g_fetch_release_us = now_us + 2000U;
        assert(ops->open_68k(context, &request, &handle_b));
        assert(g_fetch_count == 0U);
        ops->close_68k(context, handle_b);
        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);

        assert(ops->open_68k(context, &request, &handle_a));
        wire = mw_service_test_context(handle_a);
        assert(wire != NULL);
        minigl_raster_clear(0x4000U, 0xff0000ffU, 0U);
        mw_put_be32(present, MW_MAGIC);
        mw_put_be32(present + 4, MW_VERSION);
        mw_put_be32(present + 8, sizeof(present));
        mw_put_be32(present + 12, 1U);
        mw_put_be32(present + 16, MW_PRESENT);
        g_fetch_hold = 1;
        g_fetch_release_us = ~(uint64_t)0;
        assert(mw_host_submit(wire, present, sizeof(present)) == MW_STATUS_OK);
        for (step = 0U; step < 64U && minigl_raster_feed_pending(); ++step)
            (void)minigl_raster_try_feed(RIX_DRV_FEED_SOURCE_SCHEDULER);
        assert(g_fetch_count != 0U);
        g_fetch_release_us = now_us + 2000U;
        assert(ops->open_68k(context, &request, &handle_b));
        assert(g_fetch_count == 0U);
        ops->close_68k(context, handle_a);
        ops->close_68k(context, handle_b);
        g_fetch_hold = 0;
        g_fetch_count = 0U;
    }
    {
        /* A recorded clear still runs when another context replaces the
         * platform, instead of being dropped by the abort path. */
        const uint32_t *target;
        uint32_t handle68k = 0U;
        unsigned long fed;

        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);
        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind_flags(context, 1U, 0U, &gl) == 0 && gl != NULL);
        assert(minigl_raster_hardware_available());
        assert(minigl_raster_drain());
        target = minigl_raster_color_buffer();
        assert(target != NULL);
        fed = g_test_rix_tlf_bytes;
        g_fsc.watch_address = (uint32_t)(uintptr_t)target;
        g_fsc.watch_hit = 0;
        minigl_raster_clear(0x4000U, 0xff0000ffU, 0U);
        assert(g_test_rix_tlf_bytes == fed);
        assert(!g_fsc.watch_hit);
        assert(ops->open_68k(context, &request, &handle68k));
        assert(g_fsc.watch_hit);
        assert(target[0] == 0xff0000ffU);
        ops->close_68k(context, handle68k);
        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);
    {
        /* Once a closed context retires the current target, closing another
         * context must still drain the live backend rather than quarantining
         * the platform owner solely because no target is bound. */
        const zzm_minigl_wire_open_t small = { 4U, 4U, 0U };
        uint32_t handle_a = 0U;
        uint32_t handle_b = 0U;
        uint32_t handle_c = 0U;
        uint8_t vertices[4] = { 0U };
        uint8_t staging[4] = { 0U };
        MiniGLRasterBuffers buffers;
        mw_ctx *a;
        mw_ctx *c;
        uint32_t quarantines;

        assert(ops->open_68k(context, &small, &handle_a));
        assert(ops->open_68k(context, &small, &handle_b));
        assert(ops->open_68k(context, &small, &handle_c));
        a = mw_service_test_context(handle_a);
        c = mw_service_test_context(handle_c);
        assert(a != NULL && c != NULL);
        memset(&buffers, 0, sizeof(buffers));
        buffers.color = a->color_buffers[0];
        buffers.color_bytes = a->width * a->height * sizeof(*buffers.color);
        buffers.depth = a->depth_buffer;
        buffers.depth_bytes = a->width * a->height * 4U;
        buffers.vertices = vertices;
        buffers.vertices_bytes = sizeof(vertices);
        buffers.texture_staging = staging;
        buffers.texture_staging_bytes = sizeof(staging);
        buffers.texture_pool = a->texture_pool;
        buffers.texture_pool_bytes = a->texture_pool_bytes;
        buffers.width = (uint16_t)a->width;
        buffers.height = (uint16_t)a->height;
        assert(minigl_raster_configure_buffers(&buffers));
        assert(minigl_raster_color_buffer() == a->color_buffers[0]);
        quarantines = mw_rix_quarantine_count();
        ops->close_68k(context, handle_a);
        assert(minigl_raster_color_buffer() == NULL);
        ops->close_68k(context, handle_b);
        assert(mw_rix_quarantine_count() == quarantines);
        assert(!c->rix_memory_quarantined);
        assert(test_card_pool_holds(c->color_buffer_addresses[0]));
        ops->close_68k(context, handle_c);
    }
        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind_flags(context, 1U, 0U, &gl) == 0 && gl != NULL);
    }
    {
        /* A DDR list the GPU is still fetching must complete before the
         * forced abort resets the renderer, even though STATUS looks idle
         * between bursts and then turns busy again. */
        const aar_gl *draw;
        float quad[8] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };
        unsigned step;

        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);
        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind_flags(context, 1U, 0U, &gl) == 0 && gl != NULL);
        draw = (const aar_gl *)gl;
        assert(minigl_raster_hardware_available());
        g_fetch_hold = 1;
        g_fetch_release_us = ~(uint64_t)0;
        draw->VertexPointer(2, 0x1406U, 8, quad);
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        draw->DisableClientState(0x8074U);
        assert(ops->app_swap(context) == 0);
        for (step = 0U; step < 64U && minigl_raster_feed_pending(); ++step)
            (void)minigl_raster_try_feed(RIX_DRV_FEED_SOURCE_SCHEDULER);
        assert(g_fetch_count != 0U);

        g_test_rix_idle_first = 2U;
        g_test_rix_soft_resets = 0U;
        g_fetch_release_us = now_us + 2000U;
        assert(mw_rix_abort_engines() == 1);
        assert(g_fetch_count == 0U && !g_fetch_hold);
        assert(g_test_rix_soft_resets == 1U);
        g_test_rix_idle_first = 0U;

        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);
        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind_flags(context, 1U, AAR_ZZ9000_GL_BIND_INLINE_RIX_FEED,
                                   &gl) == 0 && gl != NULL);
    }
    {
        /* Finding 3: the real unload path (ops->fini) drains pending feed
         * work before the app context teardown force-aborts the engines,
         * and leaves the module reusable. */
        const aar_gl *draw = (const aar_gl *)gl;
        float quad[8] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };

        assert(minigl_raster_drain());
        g_test_rix_full_reads = TEST_FIFO_FULL_READS;

        draw->VertexPointer(2, 0x1406U, 8, quad);
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        draw->DisableClientState(0x8074U);

        assert(ops->app_swap(context) == 0);
        assert(minigl_raster_feed_pending() != 0);
        g_test_rix_full_reads = 0U;
        {
            unsigned long fed_bytes = g_test_rix_tlf_bytes;

            ops->fini(context);
            /* The unload fed the queued frame across the FIFO instead of
             * dropping it on the force-abort path. */
            assert(g_test_rix_tlf_bytes > fed_bytes);
        }
        assert(minigl_raster_feed_pending() == 0);

        /* The teardown released the platform with the app context, so
         * hardware reports unavailable until the next bind reinstates it. */
        assert(!minigl_raster_hardware_available());
        /* Reload through the loader like a real idle-unload/resident cycle:
         * init re-installs the doorbell that fini removed. */
        zzm_loader_release(ZZM_MODULE_MINIGL);
        assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
        assert(status.state == ZZM_STATE_AVAILABLE);
        assert(acquire_module_bounded() == ZZM_LOAD_READY);
        ops = zzm_minigl_ops(&context);
        assert(ops != NULL && context != NULL);
        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind(context, 1U, &gl) == 0 && gl != NULL);
        assert(minigl_raster_hardware_available());
    }
    {
        /* Wire contexts use the asynchronous ring too: the same command
         * buffer depth as an ARM app, not the old two-buffer sync ring.
         * Opening one installs that platform and leaves RasterIX available. */
        uint32_t handle68k;
        mw_ctx *wire;

        assert(minigl_raster_hardware_available());
        assert(minigl_raster_test_command_buffer_count() == MINIGL_RIX_COMMAND_BUFFERS);
        assert(ops->open_68k(context, &request, &handle68k));
        wire = mw_service_test_context(handle68k);
        assert(wire != NULL && wire->rix_async != 0U);
        assert(mw_host_rix_platform_context() == wire);
        assert(minigl_raster_test_command_buffer_count() ==
               MINIGL_RIX_COMMAND_BUFFERS);
        assert(minigl_raster_hardware_available());
        ops->close_68k(context, handle68k);
        /* The wire context's teardown retired the platform; the next block
         * rebinds it. */
    }
    {
        /* P2: an old-table platform gives an async feed over MMIO, with no backend disable. */
        const aar_gl *draw;
        float quad[8] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };
        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);

        assert(minigl_platform_set_graphics(&old_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind_flags(context, 1U, AAR_ZZ9000_GL_BIND_INLINE_RIX_FEED,
                                   &gl) == 0 && gl != NULL);
        draw = (const aar_gl *)gl;

        draw->VertexPointer(2, 0x1406U, 8, quad);
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        draw->DisableClientState(0x8074U);
        g_test_rix_full_reads = TEST_FIFO_FULL_READS;
        assert(ops->app_swap(context) == 0);
        assert(minigl_raster_feed_pending() != 0);
        g_test_rix_full_reads = 0U;
        assert(minigl_raster_hardware_available());
        assert(ops->app_swap(context) == 0);
        assert(minigl_raster_hardware_available());

        /* Restore standard test service */
        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);
        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind(context, 1U, &gl) == 0 && gl != NULL);
    }
    {
        /* A wire context's second packet is dispatched while its own first
         * packet's list is still rendering.  The service only defers packets
         * for another context's ring. */
        ZZ9KMiniGLTransportDescriptor desc;
        volatile ZZ9KMiniGLControlBlockV9 *ctrl;
        uint32_t handle = 0U;
        uint32_t generation = 0U;
        uint32_t consumer;
        uint32_t slot_idx;
        uint8_t *slot_buf;
        uint8_t first[56];
        uint8_t second[48];
        mw_ctx *wire;

        assert(zzm_minigl_transport_acquire(&generation));
        assert(mw_service_get_transport_descriptor(&desc) == 0);
        ctrl = (volatile ZZ9KMiniGLControlBlockV9 *)(void *)(
            desc.window_address + desc.ctrl_offset);
        assert(ops->open_68k(context, &request, &handle));
        wire = mw_service_test_context(handle);
        assert(wire != NULL && wire->rix_async != 0U);
        assert(mw_host_rix_platform_context() == wire);
        mw_wire_st32((volatile uint32_t *)&ctrl->op_handle, handle);

        g_fetch_hold = 1;
        g_fetch_wedged = 1;
        g_fetch_count = 0U;
        write_clear_present_packet(first);
        consumer = mw_wire_ld32(&ctrl->consumer_seq);
        slot_idx = consumer % mw_wire_ld32(&ctrl->slot_count);
        slot_buf = (uint8_t *)(void *)desc.window_address +
            (slot_idx == 0U ? desc.slot0_offset : desc.slot1_offset);
        memcpy(slot_buf, first, sizeof(first));
        mw_wire_st32((volatile uint32_t *)&ctrl->producer_seq, consumer + 1U);
        (void)zzm_minigl_service_tick(0);
        assert(mw_wire_ld32(&ctrl->consumer_seq) == consumer + 1U);
        assert(minigl_raster_work_outstanding() != 0);
        assert(g_fetch_count != 0U);

        write_clear_packet(second);
        consumer = mw_wire_ld32(&ctrl->consumer_seq);
        slot_idx = consumer % mw_wire_ld32(&ctrl->slot_count);
        slot_buf = (uint8_t *)(void *)desc.window_address +
            (slot_idx == 0U ? desc.slot0_offset : desc.slot1_offset);
        memcpy(slot_buf, second, sizeof(second));
        mw_wire_st32((volatile uint32_t *)&ctrl->producer_seq, consumer + 1U);
        (void)zzm_minigl_service_tick(0);
        assert(mw_wire_ld32(&ctrl->consumer_seq) == consumer + 1U);
        assert(minigl_raster_work_outstanding() != 0);
        assert(g_fetch_count != 0U);

        g_fetch_wedged = 0;
        g_fetch_hold = 0;
        test_rix_run_fetches();
        mw_wire_st32((volatile uint32_t *)&ctrl->op_handle, 0U);
        ops->close_68k(context, handle);
    }
    {
        /* Worker context cannot park.  A wire present, span and sync still
         * complete when the fences land during the bounded wait, and the
         * span copies the frame the list produced, not the bytes from
         * before it ran. */
        extern void zzm_module_set_worker_context(int active);
        const zzm_minigl_wire_open_t small = { 2U, 2U, 0U };
        static uint8_t vram[0x20010];
        uint32_t handle = 0U;
        mw_ctx *wire;
        uint8_t target[48];
        uint8_t clear[48];
        uint8_t present[24];
        uint8_t again[56];
        uint8_t span_packet[72];
        uint8_t sync_packet[28];
        uint8_t *span;
        uint32_t before;
        unsigned yields;
        uint8_t *payload;

        assert(ops->open_68k(context, &small, &handle));
        wire = mw_service_test_context(handle);
        assert(wire != NULL && wire->rix_async != 0U);
        memset(target, 0, sizeof(target));
        mw_put_be32(target, MW_MAGIC);
        mw_put_be32(target + 4, MW_VERSION);
        mw_put_be32(target + 8, sizeof(target));
        mw_put_be32(target + 12, 1U);
        mw_put_be32(target + 16, MW_TARGET);
        mw_put_be32(target + 20, 24U);
        mw_put_be32(target + 24, 2U);
        mw_put_be32(target + 28, 2U);
        mw_put_be32(target + 32, 1U);
        mw_put_be32(target + 36, 16U);
        mw_put_be32(target + 40, 2U);
        mw_put_be32(target + 44, 2U);
        assert(mw_host_submit(wire, target, sizeof(target)) == MW_STATUS_OK);
        assert(wire->state.target.buffer_count == 1U);

        g_fetch_hold = 1;
        g_fetch_wedged = 0;
        g_fetch_release_us = ~(uint64_t)0;
        g_fetch_count = 0U;
        write_clear_packet(clear);
        assert(mw_host_submit(wire, clear, sizeof(clear)) == MW_STATUS_OK);
        memset(present, 0, sizeof(present));
        mw_put_be32(present, MW_MAGIC);
        mw_put_be32(present + 4, MW_VERSION);
        mw_put_be32(present + 8, sizeof(present));
        mw_put_be32(present + 12, 1U);
        mw_put_be32(present + 16, MW_PRESENT);
        assert(mw_host_submit(wire, present, sizeof(present)) == MW_STATUS_OK);
        assert(g_fetch_count != 0U);
        assert(minigl_raster_work_outstanding() != 0);
        before = wire->color_buffers[wire->state.target.latest][0];

        memset(vram, 0xa5, sizeof(vram));
        mw_host_set_present_vram(vram, 0x01000000U);
        span = vram + 0x00020000U;
        memset(span_packet, 0, sizeof(span_packet));
        mw_put_be32(span_packet, MW_MAGIC);
        mw_put_be32(span_packet + 4, MW_VERSION);
        mw_put_be32(span_packet + 8, sizeof(span_packet));
        mw_put_be32(span_packet + 12, 1U);
        mw_put_be32(span_packet + 16, MW_PRESENT_SPAN);
        mw_put_be32(span_packet + 20, 48U);
        payload = span_packet + 24;
        mw_put_be32(payload, MW_BLIT);
        mw_put_be32(payload + 4, 4U);
        mw_put_be32(payload + 8, 4U);
        mw_put_be32(payload + 20, 1U);
        mw_put_be32(payload + 32, 2U);
        mw_put_be32(payload + 36, 2U);
        mw_put_be32(payload + 40, 0x00020000U);
        mw_put_be32(payload + 44, 8U);

        zzm_module_set_worker_context(1);
        yields = wait_yields;
        g_fetch_release_us = now_us;
        assert(mw_host_submit(wire, span_packet, sizeof(span_packet)) ==
               MW_STATUS_OK);
        assert(wait_yields == yields);
        assert(g_fetch_count == 0U);
        assert(wire->color_buffers[wire->state.target.latest][0] != before);
        assert(span[0] != 0xa5U);
        assert(memcmp(span, &before, 2U) != 0);
        if (wire->color_rgb565[wire->state.target.latest])
            assert(memcmp(span, wire->color_buffers[wire->state.target.latest],
                          2U) == 0);

        g_fetch_hold = 1;
        g_fetch_release_us = ~(uint64_t)0;
        g_fetch_count = 0U;
        write_clear_present_packet(again);
        assert(mw_host_submit(wire, again, sizeof(again)) == MW_STATUS_OK);
        assert(minigl_raster_work_outstanding() != 0);
        memset(sync_packet, 0, sizeof(sync_packet));
        mw_put_be32(sync_packet, MW_MAGIC);
        mw_put_be32(sync_packet + 4, MW_VERSION);
        mw_put_be32(sync_packet + 8, sizeof(sync_packet));
        mw_put_be32(sync_packet + 12, 1U);
        mw_put_be32(sync_packet + 16, MW_SYNC);
        mw_put_be32(sync_packet + 20, 4U);
        g_fetch_release_us = now_us;
        assert(mw_host_submit(wire, sync_packet, sizeof(sync_packet)) ==
               MW_STATUS_OK);
        assert(!minigl_raster_work_outstanding());
        assert(wait_yields == yields);
        zzm_module_set_worker_context(0);
        g_fetch_hold = 0;
        g_fetch_count = 0U;
        mw_host_set_present_vram(NULL, 0U);
        ops->close_68k(context, handle);
    }
    ops->app_unbind(context);
    display_width = 640U;
    display_height = 480U;
    display_pitch = 1280U;
    display_format = 1U;
    display_front = 0x08000000U;
    display_back = 0x08100000U;
    display_layout = 2U;
    display_latest = 1;
    assert(ops->app_bind_flags(context, 1U, AAR_ZZ9000_GL_BIND_DIRECT_PRESENT, &gl) == 0);
    assert(amiberry_transport_inproc_ctx()->state.target.buffer_count == 3U);
    assert(ops->app_swap(context) == 0);
    assert(ops->app_swap(context) == 0);
    ops->app_unbind(context);
    /* The bind flags select three distinct feed modes.  The synchronous bit
     * is part of the public ABI and must not be rejected by validation. */
    assert(ops->app_bind_flags(context, 1U, 0U, &gl) == 0);
    assert(amiberry_transport_inproc_ctx()->rix_async != 0U);
    assert(amiberry_transport_inproc_ctx()->rix_submit_mode ==
           RIX_DRV_SUBMIT_MEMFETCH);
    ops->app_unbind(context);
    assert(ops->app_bind_flags(context, 1U, AAR_ZZ9000_GL_BIND_INLINE_RIX_FEED,
                               &gl) == 0);
    assert(amiberry_transport_inproc_ctx()->rix_async != 0U);
    assert(amiberry_transport_inproc_ctx()->rix_submit_mode ==
           RIX_DRV_SUBMIT_INLINE);
    ops->app_unbind(context);
    assert(ops->app_bind_flags(context, 1U, AAR_ZZ9000_GL_BIND_SYNC_RIX_FEED,
                               &gl) == 0);
    assert(amiberry_transport_inproc_ctx()->rix_async == 0U);
    assert(amiberry_transport_inproc_ctx()->rix_submit_mode ==
           RIX_DRV_SUBMIT_MEMFETCH);
    ops->app_unbind(context);
    assert(ops->app_bind_flags(context, 1U, 16U, &gl) == AAR_E_INVAL);

    /*
     * copy_bound must honour the bound layout byte for byte (it used to
     * write RGB565 BE / BGRA whatever the display negotiated). The direct
     * seam feeds a known pure-red source word for both frame sources.
     */
    {
        static const struct {
            uint32_t format;
            uint32_t layout;
            uint32_t pitch;
            uint8_t bytes[4];
        } layouts[] = {
            { 1U, 1U, 4U * 2U, { 0xF8U, 0x00U } }, /* RGB565 BE */
            { 1U, 2U, 4U * 2U, { 0x00U, 0xF8U } }, /* RGB565 LE */
            { 1U, 3U, 4U * 2U, { 0x00U, 0x1FU } }, /* BGR565 BE */
            { 1U, 4U, 4U * 2U, { 0x1FU, 0x00U } }, /* BGR565 LE */
            { 2U, 5U, 4U * 4U, { 0x00U, 0x00U, 0xFFU, 0xFFU } }, /* BGRA */
            { 2U, 6U, 4U * 4U, { 0xFFU, 0x00U, 0x00U, 0xFFU } }, /* RGBA */
        };
        uint32_t i;
        uint8_t *frame = calloc(1U, 64U);
        const uint32_t rgba_red = 0xFF0000FFU; /* r<<24 | g<<16 | b<<8 | a */
        const uint16_t rgb565_red = 0xF800U;

        /* The p96 mock keeps the copy destination out of the shared arena
         * the earlier scenarios allocated from. */
        assert(frame != NULL);
        zzapp_gl_test_set_p96(frame, 64U);
        for (i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
            memset(frame, 0, 64U);
            assert(zzapp_gl_test_copy_bound(&rgba_red, 0, 4U, 2U,
                                            layouts[i].pitch,
                                            layouts[i].format,
                                            0x08100000U,
                                            layouts[i].layout) == 1);
            assert(memcmp(frame, layouts[i].bytes,
                          layouts[i].format == 1U ? 2U : 4U) == 0);
        }
        /* The RGB565 frame source (the RasterIX half) converts per layout
         * too, including the R/B-swapped BGR565 variants. */
        memset(frame, 0, 64U);
        assert(zzapp_gl_test_copy_bound((const uint32_t *)&rgb565_red, 1,
                                        4U, 2U, 8U, 1U, 0x08100000U, 1U) == 1);
        assert(frame[0] == 0xF8U && frame[1] == 0x00U);
        memset(frame, 0, 64U);
        assert(zzapp_gl_test_copy_bound((const uint32_t *)&rgb565_red, 1,
                                        4U, 2U, 8U, 1U, 0x08100000U, 3U) == 1);
        assert(frame[0] == 0x00U && frame[1] == 0x1FU);
        zzapp_gl_test_set_p96(NULL, 0U);
        free(frame);
    }

    ops->app_unbind(context);
    display_width = 640U;
    display_height = 480U;
    display_pitch = 1280U;
    display_format = 1U;
    display_layout = 2U;

    /*
     * Presentation must invalidate only targets the hardware wrote: a
     * CPU-written RGBA frame crosses with no invalidate at all, a target
     * flagged as the RasterIX RGB565 half is invalidated once.
     */
    display_width = 4U;
    display_height = 2U;
    display_front = 0x08000000U;
    display_back = 0x08100000U;
    display_latest = 0;
    display_format = 1U;
    display_layout = 1U;
    display_pitch = 8U;
    assert(ops->app_bind_flags(context, 1U, 0U, &gl) == 0);
    {
        const aar_gl *draw = (const aar_gl *)gl;
        mw_ctx *mw;

        draw->ClearColor(1.0f, 0.0f, 0.0f, 1.0f);
        draw->Clear(0x4000U);
        draw->Finish();
        ops->app_swap(context);
        mw = amiberry_transport_inproc_ctx();
        assert(mw != NULL);
        mw->color_rgb565[0] = 0U;
        mw->color_rgb565[1] = 0U;
        mw->color_rgb565[2] = 0U;
        zzapp_gl_test_reset_invalidates();
        ops->app_swap(context);
        assert(zzapp_gl_test_invalidates() == 0U);
        mw->color_rgb565[0] = 1U;
        mw->color_rgb565[1] = 1U;
        mw->color_rgb565[2] = 1U;
        zzapp_gl_test_reset_invalidates();
        ops->app_swap(context);
        assert(zzapp_gl_test_invalidates() == 1U);
    }
    ops->app_unbind(context);
    display_width = 640U;
    display_height = 480U;
    display_pitch = 1280U;
    display_format = 1U;
    display_layout = 2U;
    /* C-GL.5: the swap presents the frame (AAR spec 8.5). */
    test_gl_swap_presents(ops, context, 0, 0);
    test_gl_swap_presents(ops, context, 1, 0);
    test_gl_swap_presents(ops, context, 0, 1);
    test_gl_swap_presents(ops, context, 1, 1);
    test_direct_mailbox(ops, context);
    display_width = 640U;
    display_height = 480U;
    display_pitch = 1280U;
    display_format = 1U;
    display_layout = 2U;

    display_latest = 0;
    hold_flip = 1;
    flip_idle = 1;
    flip_waits = 0U;
    assert(ops->app_bind_flags(context, 1U, AAR_ZZ9000_GL_BIND_SYNC_PRESENT, &gl) == 0);
    assert(ops->app_swap(context) == 0);
    /* The held flip is waited for in the host's registered flip wait. */
    assert(flip_waits != 0U);
    hold_flip = 0;
    ops->app_unbind(context);
    ops->app_reclaim(context, 1U);
    assert(zzm_minigl_begin_reset() == ZZM_MINIGL_RESET_QUIESCENT);
    assert(zzm_minigl_reset_poll() == ZZM_MINIGL_RESET_QUIESCENT);
    assert(card_pool.block_count == 0U);
    assert(zzm_minigl_transport_acquire(&generation2));
    assert(generation2 > generation1);
    zzm_minigl_doorbell();
    assert(zzm_minigl_has_work());
    zzm_minigl_test_set_poll_hook(release_transport_from_worker_poll);
    zzm_minigl_service_tick(0);
    zzm_minigl_test_set_poll_hook(NULL);
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_LOADED);
    assert(zzm_minigl_worker_registered());
    assert(acquire_module_bounded() == ZZM_LOAD_READY);
    ops = zzm_minigl_ops(&context);
    assert(ops != NULL && context != NULL);
    assert(ops->app_bind(context, 1U, &gl) == 0 && gl != NULL);
    ops->app_unbind(context);
    ops->app_reclaim(context, 1U);
    assert(card_pool.block_count == 0U);
    {
        /* Warm Amiga reset when the DMAC channel cannot be claimed again
         * after the stop (the card's failure): the reset still completes
         * at once, Kickstart's second reset request completes too, and the
         * next bind claims the channel back. */
        const aar_gl *draw;
        float quad[8] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };
        int result = ZZM_MINIGL_RESET_BUSY;
        unsigned steps;
        unsigned claims;
        unsigned long fed_bytes;

        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind_flags(context, 1U, AAR_ZZ9000_GL_BIND_INLINE_RIX_FEED,
                                   &gl) == 0 && gl != NULL);
        draw = (const aar_gl *)gl;
        draw->VertexPointer(2, 0x1406U, 8, quad);
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        draw->DisableClientState(0x8074U);
        assert(ops->app_swap(context) == 0);

        g_test_rix_claim_fails = 1;
        for (steps = 0U; steps < 16U && result == ZZM_MINIGL_RESET_BUSY; ++steps)
            result = zzm_minigl_reset_step();
        assert(result == ZZM_MINIGL_RESET_QUIESCENT);
        assert(steps == 1U);
        result = ZZM_MINIGL_RESET_BUSY;
        for (steps = 0U; steps < 16U && result == ZZM_MINIGL_RESET_BUSY; ++steps)
            result = zzm_minigl_reset_step();
        assert(result == ZZM_MINIGL_RESET_QUIESCENT);
        assert(card_pool.block_count == 0U);

        g_test_rix_claim_fails = 0;
        claims = g_test_rix_claims;
        assert(ops->app_bind_flags(context, 1U, AAR_ZZ9000_GL_BIND_INLINE_RIX_FEED,
                                   &gl) == 0 && gl != NULL);
        assert(g_test_rix_claims > claims);
        draw = (const aar_gl *)gl;
        fed_bytes = g_test_rix_tlf_bytes;
        draw->VertexPointer(2, 0x1406U, 8, quad);
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        draw->DisableClientState(0x8074U);
        assert(ops->app_swap(context) == 0);
        assert(minigl_raster_drain());
        assert(g_test_rix_tlf_bytes > fed_bytes);
        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);
        assert(card_pool.block_count == 0U);
    }
    /*
     * A 68k session has closed its transport and context. Leave one module
     * allocation live so unload must reclaim the base module owner before an
     * ARM app can reserve the complete resident quota again.
     */
    module_allocation = malloc(1U);
    assert(module_allocation != NULL);
    zzm_loader_release(ZZM_MODULE_MINIGL);
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_AVAILABLE);
    assert(!zzm_minigl_worker_registered());
    assert(card_pool.class_used[CARD_POOL_CLASS_MINIGL_MODULE] == 0U);

    /* The next owner is an ARM application: it must reload and bind cleanly. */
    assert(acquire_module_bounded() == ZZM_LOAD_READY);
    ops = zzm_minigl_ops(&context);
    assert(ops != NULL && context != NULL);
    assert(ops->app_bind(context, 1U, &gl) == 0 && gl != NULL);
    ops->app_unbind(context);
    ops->app_reclaim(context, 1U);
    {
        /* A 68k target remains discoverable while a failed ARM-ring drain
         * quarantines the target owner during close. */
        uint32_t handle68k;
        uint32_t color_address;
        uint32_t depth_address;
        uint32_t quarantines;
        mw_ctx *wire;
        MiniGLRasterBuffers buffers;
        static uint8_t vertices[16];
        static uint8_t staging[16];
        unsigned step;

        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);
        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->open_68k(context, &request, &handle68k));
        assert(ops->app_bind_flags(context, 1U, 0U, &gl) == 0 && gl != NULL);
        wire = mw_service_test_context(handle68k);
        assert(wire != NULL && wire->color_buffers[0] != NULL &&
               wire->depth_buffer != NULL && wire->texture_pool != NULL);
        color_address = wire->color_buffer_addresses[0];
        depth_address = wire->depth_buffer_address;
        memset(&buffers, 0, sizeof(buffers));
        buffers.color = wire->color_buffers[0];
        buffers.color_bytes = wire->width * wire->height * sizeof(*buffers.color);
        buffers.depth = wire->depth_buffer;
        buffers.depth_bytes = wire->width * wire->height * 4U;
        buffers.vertices = vertices;
        buffers.vertices_bytes = sizeof(vertices);
        buffers.texture_staging = staging;
        buffers.texture_staging_bytes = sizeof(staging);
        buffers.texture_pool = wire->texture_pool;
        buffers.texture_pool_bytes = wire->texture_pool_bytes;
        buffers.width = (uint16_t)wire->width;
        buffers.height = (uint16_t)wire->height;
        assert(minigl_raster_finish());
        assert(buffers.width != 0U && buffers.height != 0U);
        assert((uint32_t)buffers.width * buffers.height <=
               buffers.color_bytes / sizeof(*buffers.color));
        assert((uint32_t)buffers.width * buffers.height <= buffers.depth_bytes / 3U);
        assert(buffers.vertices_bytes != 0U && buffers.texture_staging_bytes != 0U);
        assert(buffers.texture_pool_bytes != 0U);
        minigl_raster_release_buffers();
        assert(minigl_raster_configure_buffers(&buffers));

        g_fetch_hold = 1;
        g_fetch_wedged = 1;
        minigl_raster_clear(0x4000U, 0U, 0U);
        assert(minigl_raster_submit_present());
        for (step = 0U; step < 64U && minigl_raster_feed_pending(); ++step)
            (void)minigl_raster_try_feed(RIX_DRV_FEED_SOURCE_SCHEDULER);
        assert(g_fetch_count != 0U);

        quarantines = mw_rix_quarantine_count();
        ops->close_68k(context, handle68k);
        assert(mw_rix_quarantine_count() == quarantines + 2U);
        assert(test_card_pool_holds(color_address));
        assert(test_card_pool_holds(depth_address));
        g_fetch_hold = 0;
        g_fetch_wedged = 0;
        g_fetch_count = 0U;
        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);
    }
    {
        /* A wedged GPU never finishes its DDR list.  The forced teardown
         * resets it but must not hand back any memory RasterIX may still
         * read or write: the command buffers and render targets stay in
         * live card-pool blocks. */
        const aar_gl *draw;
        float quad[8] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };
        uint32_t *kept = quarantined_kept;
        unsigned *kept_count = &quarantined_kept_count;
        uint32_t quarantines;
        unsigned step;
        mw_ctx *mw;

        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind_flags(context, 1U, 0U, &gl) == 0 && gl != NULL);
        draw = (const aar_gl *)gl;
        assert(minigl_raster_hardware_available());
        g_fetch_hold = 1;
        g_fetch_wedged = 1;
        draw->VertexPointer(2, 0x1406U, 8, quad);
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        draw->DisableClientState(0x8074U);
        assert(ops->app_swap(context) == 0);
        for (step = 0U; step < 64U && minigl_raster_feed_pending(); ++step)
            (void)minigl_raster_try_feed(RIX_DRV_FEED_SOURCE_SCHEDULER);
        assert(g_fetch_count != 0U);
        mw = amiberry_transport_inproc_ctx();
        assert(mw != NULL);
        for (step = 0U; step < MINIGL_RIX_COMMAND_BUFFERS; ++step)
            if (mw->rix_command_buffer_addresses[step] != 0U)
                kept[(*kept_count)++] = mw->rix_command_buffer_addresses[step];
        for (step = 0U; step < 3U; ++step)
            kept[(*kept_count)++] = mw->color_buffer_addresses[step];
        kept[(*kept_count)++] = mw->depth_buffer_address;
        quarantines = mw_rix_quarantine_count();
        g_test_rix_soft_resets = 0U;

        quarantined_owner = mw->card_pool_owner;
        ops->app_unbind(context);
        assert(g_test_rix_soft_resets != 0U);
        assert(mw_rix_quarantine_count() == quarantines + 1U);
        for (step = 0U; step < *kept_count; ++step)
            assert(test_card_pool_holds(kept[step]));
        g_fetch_hold = 0;
        g_fetch_wedged = 0;
        g_fetch_count = 0U;
    }
    {
        /* An ARM app's outstanding ring still defers wire packets: dispatch
         * would switch the shared backend.  Past the wait deadline the
         * backend is disabled, that app's owners are quarantined, and the
         * packet is processed. */
        extern void (*g_rix_test_log)(const char *fmt, ...);
        const aar_gl *draw;
        float quad[8] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };
        ZZ9KMiniGLTransportDescriptor desc;
        volatile ZZ9KMiniGLControlBlockV9 *ctrl;
        uint32_t consumer;
        uint32_t slot_idx;
        uint8_t *slot_buf;
        uint32_t quarantines;
        uint64_t armed;
        unsigned step;

        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->app_bind_flags(context, 1U, 0U, &gl) == 0 && gl != NULL);
        assert(mw_host_rix_platform_context() == amiberry_transport_inproc_ctx());
        draw = (const aar_gl *)gl;
        assert(minigl_raster_drain());
        g_fetch_hold = 1;
        g_fetch_wedged = 1;
        g_fetch_release_us = ~(uint64_t)0;
        draw->VertexPointer(2, 0x1406U, 8, quad);
        draw->EnableClientState(0x8074U);
        draw->DrawArrays(0x0005U, 0, 4);
        draw->DisableClientState(0x8074U);
        assert(ops->app_swap(context) == 0);
        for (step = 0U; step < 64U && minigl_raster_feed_pending(); ++step)
            (void)minigl_raster_try_feed(RIX_DRV_FEED_SOURCE_SCHEDULER);
        assert(minigl_raster_work_outstanding() != 0);

        {
            uint32_t generation = 0U;

            assert(zzm_minigl_transport_acquire(&generation));
        }
        assert(mw_service_get_transport_descriptor(&desc) == 0);
        ctrl = (volatile ZZ9KMiniGLControlBlockV9 *)(void *)(
            desc.window_address + desc.ctrl_offset);
        consumer = mw_wire_ld32(&ctrl->consumer_seq);
        slot_idx = consumer % mw_wire_ld32(&ctrl->slot_count);
        slot_buf = (uint8_t *)(void *)desc.window_address +
            (slot_idx == 0U ? desc.slot0_offset : desc.slot1_offset);
        mw_put_be32(slot_buf, 0x414D3344U);
        mw_put_be32(slot_buf + 4, 9U);
        mw_put_be32(slot_buf + 8, 16U);
        mw_put_be32(slot_buf + 12, 0U);
        mw_wire_st32((volatile uint32_t *)&ctrl->producer_seq, consumer + 1U);

        g_log_used = 0U;
        g_log[0] = '\0';
        g_rix_test_log = test_capture_rix_log;
        armed = now_us;
        (void)zzm_minigl_service_tick(0);
        assert(mw_wire_ld32(&ctrl->consumer_seq) == consumer);
        now_us = armed + 1000000ULL;
        (void)zzm_minigl_service_tick(0);
        assert(mw_wire_ld32(&ctrl->consumer_seq) == consumer);
        quarantines = mw_rix_quarantine_count();
        now_us = armed + 4000000ULL;
        (void)zzm_minigl_service_tick(0);
        assert(mw_wire_ld32(&ctrl->consumer_seq) == consumer + 1U);
        assert(strstr(g_log, "wait watchdog disabling") != NULL);
        assert(mw_rix_quarantine_count() == quarantines + 1U);
        g_rix_test_log = NULL;
        g_fetch_hold = 0;
        g_fetch_wedged = 0;
        g_fetch_count = 0U;
        ops->app_unbind(context);
        ops->app_reclaim(context, 1U);
    }

    {
        /* Amiga reset with a wire ring still rendering takes the same
         * abort and quarantine as an ARM ring that has not finished:
         * the engine is reset and the colour target and command buffer
         * stay in the card pool. */
        uint32_t handle = 0U;
        mw_ctx *wire;
        uint8_t packet[56];
        uint32_t color;
        uint32_t command;
        uint32_t quarantines;
        unsigned resets;
        unsigned step;

        assert(minigl_platform_set_graphics(&test_rix_service, &graphics_caps,
                                            ZZM_MODULE_OWNER(ZZM_MODULE_MINIGL)));
        assert(ops->open_68k(context, &request, &handle));
        wire = mw_service_test_context(handle);
        assert(wire != NULL && wire->rix_async != 0U);
        color = wire->color_buffer_addresses[0];
        command = wire->rix_command_buffer_addresses[0];
        assert(color != 0U && command != 0U);
        g_fetch_hold = 1;
        g_fetch_wedged = 1;
        g_fetch_count = 0U;
        write_clear_present_packet(packet);
        assert(mw_host_submit(wire, packet, sizeof(packet)) == MW_STATUS_OK);
        for (step = 0U; step < 64U && minigl_raster_feed_pending(); ++step)
            (void)minigl_raster_try_feed(RIX_DRV_FEED_SOURCE_SCHEDULER);
        assert(g_fetch_count != 0U);
        assert(minigl_raster_work_outstanding() != 0);
        quarantines = mw_rix_quarantine_count();
        resets = g_test_rix_soft_resets;
        assert(zzm_minigl_begin_reset() == ZZM_MINIGL_RESET_QUIESCENT);
        assert(mw_rix_quarantine_count() == quarantines + 1U);
        assert(g_test_rix_soft_resets > resets);
        assert(test_card_pool_holds(color));
        assert(test_card_pool_holds(command));
        g_fetch_wedged = 0;
        g_fetch_hold = 0;
        g_fetch_count = 0U;
    }

    zzm_loader_release(ZZM_MODULE_MINIGL);
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_AVAILABLE);
    assert(card_pool.class_used[CARD_POOL_CLASS_MINIGL_MODULE] == 0U);
    /* Core-0 recovery invokes the same runtime reclamation after forgetting
     * the core-1 image; it must preserve these accepted DDR lists too. */
    (void)zzm_runtime_release_module(ZZM_MODULE_MINIGL);
    /* Corruption is detected only after every bounded read/hash pass, and
     * the acquire remains pending until the slot is coherently poisoned. */
    prepare_module_file();
    zzm_loader_boot_load();
    module_file[ZZM_V3_HEADER_SIZE + 31U] ^= 0x5aU;
    memset((void *)(uintptr_t)ZZM_MINIGL_SLOT_ADDRESS, 0xa5,
           TEST_MODULE_IMAGE_BYTES + TEST_MODULE_BSS_BYTES);
    assert(acquire_module_bounded() == ZZM_LOAD_FAILED);
    assert(module_open_calls == 1U && module_close_calls == 1U);
    assert(module_read_calls == 4U);
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_LOAD_FAILED);
    assert(status.reason == ZZM_REASON_IMAGE_INVALID);
    assert(!zzm_minigl_worker_registered());
    assert_module_poisoned();
    module_file[ZZM_V3_HEADER_SIZE + 31U] ^= 0x5aU;

    /* A later read failure closes the persistent FIL and reports the same
     * FILE_READ reason as the former one-shot f_read. */
    zzm_loader_boot_load();
    module_read_fail_call = 2U;
    memset((void *)(uintptr_t)ZZM_MINIGL_SLOT_ADDRESS, 0xa5,
           TEST_MODULE_IMAGE_BYTES + TEST_MODULE_BSS_BYTES);
    assert(acquire_module_bounded() == ZZM_LOAD_FAILED);
    module_read_fail_call = 0U;
    assert(module_open_calls == 1U && module_close_calls == 1U);
    assert(module_read_calls == 2U);
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_LOAD_FAILED);
    assert(status.reason == ZZM_REASON_FILE_READ);
    assert(!zzm_minigl_worker_registered());
    assert_module_poisoned();

    /* The released slot is poison. A following SD-open failure must report
     * LOAD_FAILED without entering its stale fini entry. */
    zzm_loader_boot_load();
    reject_module_open = 1;
    assert(acquire_module_bounded() == ZZM_LOAD_FAILED);
    assert(module_open_calls == 0U && module_close_calls == 0U);
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_LOAD_FAILED);
    assert(status.reason == ZZM_REASON_FILE_OPEN);
    assert(!zzm_minigl_worker_registered());
    assert_module_poisoned();
    /* Reclamation must retain every address that an unfinished DDR list may
     * still fetch, rather than merely retaining an owner record. */
    {
        unsigned index;

        assert(quarantined_owner != 0U);
        for (index = 0U; index < quarantined_kept_count; ++index)
            assert(test_card_pool_holds(quarantined_kept[index]));
    }
    test_reset_during_final_module_init();
    test_reset_during_transport_acquire();
    test_reset_releases_public_acquire_reference();
    test_reset_cancels_resumable_load();
    puts("MiniGL real module loader/runtime integration passed");
    return 0;
}
