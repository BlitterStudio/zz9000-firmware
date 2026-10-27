/*
 * ZZ9000 ARM Firmware Wire-v9 Transport Service Implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mw_service.h"
#include "mw_parse.h"
#include "memorymap.h"
#include "card_pool.h"
#include <string.h>
#include <stdio.h>
#include "minigl_raster.h"

#if defined(__arm__) && !defined(MINIGL_HOST_TEST)
#include "xil_cache.h"
#endif


#define MW_VRAM_ARM_BASE   0x00200000UL /* FRAMEBUFFER_ADDRESS */

/* BLIT/CAPTURE spans are valid below the transport window base; the
 * published descriptor carries the same limit so the library and firmware
 * agree on the bound. Zero until a window is bound. */
static uint32_t g_vram_limit_arm = 0U;

typedef char mw_v9_control_size_matches_window[
    sizeof(ZZ9KMiniGLControlBlockV9) == 128U ? 1 : -1];
typedef char mw_v9_status_size_matches_window[
    sizeof(ZZ9KMiniGLStatusV9) == 128U ? 1 : -1];
typedef char mw_v9_slots_match_window[
    MW_DEFAULT_SLOT_CAPACITY_Z3 == MW_WINDOW_SLOT_STRIDE_Z3 ? 1 : -1];
typedef char mw_v9_reply_matches_window[
    (MW_WINDOW_SIZE_Z3 - MW_WINDOW_REPLY_OFFSET_Z3) ==
        MW_DEFAULT_REPLY_CAPACITY ? 1 : -1];

#if defined(MINIGL_HOST_TEST)
#define MW_DIAG(...) do { } while (0)
#else
#define MW_DIAG(...) printf(__VA_ARGS__)
#endif

static const char *mw_backend_name(const mw_ctx *context)
{
    if (context && context->state.force_cpu)
        return "cpu";
    return minigl_raster_hardware_available() ? "rasterix" : "cpu";
}

static uint32_t mw_packet_first_opcode(const uint8_t *packet, uint32_t bytes)
{
    return packet && bytes >= 24U ? mw_be32(packet + 16U) : 0U;
}

static uint32_t g_reject_total;
static uint32_t g_reject_since_log;
static uint32_t g_reject_log_events;

static void mw_log_reject(uint32_t status, uint32_t opcode)
{
    (void)status;
    (void)opcode;
    g_reject_total++;
    g_reject_since_log++;
    /* printf on the worker shares the polled UART FIFO with core 0 with no
     * arbitration: an unthrottled reject print per packet can monopolize
     * the FIFO, starve core 0's console (and its boot), and drown every
     * other diagnostic. Log the first reject, then one line per
     * MW_REJECT_LOG_INTERVAL with the suppressed count. */
    if (g_reject_total == 1U || g_reject_since_log >= MW_REJECT_LOG_INTERVAL) {
        MW_DIAG("[MGL9] reject status=%08lx opcode=%08lx (suppressed %lu)\n",
                (unsigned long)status, (unsigned long)opcode,
                (unsigned long)(g_reject_since_log - 1U));
        g_reject_log_events++;
        g_reject_since_log = 0;
    }
}

uint32_t mw_service_reject_log_events(void)
{
    return g_reject_log_events;
}
#if defined(MINIGL_HOST_TEST)
static uint8_t *g_host_vram_base = NULL;

void mw_service_set_host_vram(void *base, uint32_t limit_arm)
{
    g_host_vram_base = (uint8_t *)base;
    if (limit_arm)
        g_vram_limit_arm = limit_arm;
    else
        g_vram_limit_arm = mw_service_transport_window_base();
}
#endif

#if defined(__arm__) && !defined(MINIGL_HOST_TEST)
static void mw_service_cache_maintain_span(uint32_t arm_base, const uint8_t *req, uint32_t req_bytes, int is_flush)
{
    uint32_t format, bpp, pitch, count, i;
    if (req_bytes < 24)
        return;
    format = mw_be32(req + 4);
    if (format < 2 || format > 13)
        return;
    bpp = (format <= 3) ? 3U : (format >= 6 && format <= 9) ? 4U : 2U;
    pitch = mw_be32(req + 8);
    count = mw_be32(req + 20);
    if (!count || count > MW_BLIT_MAX_RECTS || req_bytes != 24 + 16 * count)
        return;
    for (i = 0; i < count; ++i) {
        uint32_t rx0 = mw_be32(req + 24 + i * 16 + 0);
        uint32_t ry0 = mw_be32(req + 24 + i * 16 + 4);
        uint32_t rx1 = mw_be32(req + 24 + i * 16 + 8);
        uint32_t ry1 = mw_be32(req + 24 + i * 16 + 12);
        if (rx0 >= rx1 || ry0 >= ry1)
            continue;
        uint32_t row_bytes = (rx1 - rx0) * bpp;
        for (uint32_t y = ry0; y < ry1; ++y) {
            UINTPTR address = (UINTPTR)(arm_base + (uint64_t)y * pitch + (uint64_t)rx0 * bpp);
            if (is_flush)
                Xil_DCacheFlushRange(address, row_bytes);
            else
                Xil_DCacheInvalidateRange(address, row_bytes);
        }
    }
    if (is_flush)
        __asm__ volatile ("dsb sy" ::: "memory");
}
#endif

#define MW_MAX_CONTEXTS 8U

static volatile ZZ9KMiniGLControlBlockV9 *g_ctrl = NULL;
static volatile ZZ9KMiniGLStatusV9       *g_status = NULL;
static uint8_t *g_slot_ptrs[2] = {NULL, NULL};
static uint8_t *g_reply_ptr = NULL;

static mw_ctx *g_contexts[MW_MAX_CONTEXTS];
static uint32_t g_active_handle = 0;
static uint32_t g_generation = 1U;
static int      g_doorbell_pending = 0;
static int      g_is_bound = 0;

static mw_ctx *mw_service_find_context(uint32_t handle)
{
    uint32_t slot;
    if (!handle)
        return NULL;
    slot = (handle & 0xffffU);
    if (slot >= 1U && slot <= MW_MAX_CONTEXTS) {
        mw_ctx *c = g_contexts[slot - 1U];
        if (c && c->handle == handle)
            return c;
    }
    return NULL;
}

int mw_service_bind_region(volatile void *region, uint32_t size)
{
    if (!region || size < (sizeof(ZZ9KMiniGLControlBlockV9) + sizeof(ZZ9KMiniGLStatusV9))) {
        g_ctrl = NULL;
        g_status = NULL;
        g_slot_ptrs[0] = NULL;
        g_slot_ptrs[1] = NULL;
        g_reply_ptr = NULL;
        g_is_bound = 0;
        return 0;
    }

    g_ctrl = (volatile ZZ9KMiniGLControlBlockV9 *)region;

    /* The window is one raw allocation from the host-visible heap
     * (surface_allocator_alloc_range): it holds whatever DDR last held --
     * at power-on typically all ones. A fresh binding must present a
     * quiescent transport, so zero BOTH ownership halves of the control
     * and status blocks: the worker then sees producer_seq/op_seq 0 ("no
     * host has opened the transport yet") instead of a garbage backlog it
     * would drain one rejected packet at a time. */
    memset((void *)(uintptr_t)region, 0,
           sizeof(ZZ9KMiniGLControlBlockV9) + sizeof(ZZ9KMiniGLStatusV9));
    g_status = (volatile ZZ9KMiniGLStatusV9 *)((uint8_t *)region + sizeof(ZZ9KMiniGLControlBlockV9));

    if (size >= MW_WINDOW_SIZE_Z3) { /* Full Z3 layout with 256K slots and 64K reply */
        g_slot_ptrs[0] = (uint8_t *)region + MW_WINDOW_SLOT0_OFFSET;
        g_slot_ptrs[1] = (uint8_t *)region + MW_WINDOW_SLOT0_OFFSET + MW_WINDOW_SLOT_STRIDE_Z3;
        g_reply_ptr = (uint8_t *)region + MW_WINDOW_REPLY_OFFSET_Z3;
        g_is_bound = 1;
        mw_service_init();
        g_ctrl->slot_capacity = MW_DEFAULT_SLOT_CAPACITY_Z3;
        g_ctrl->slot_count = MW_DEFAULT_SLOT_COUNT_Z3;
    } else if (size >= MW_WINDOW_SIZE_Z2_2SLOT) { /* Zorro II layout with 64K slots (2 slots + 16K reply) */
        g_slot_ptrs[0] = (uint8_t *)region + MW_WINDOW_SLOT0_OFFSET;
        g_slot_ptrs[1] = (uint8_t *)region + MW_WINDOW_SLOT0_OFFSET + MW_WINDOW_SLOT_STRIDE_Z2;
        g_reply_ptr = (uint8_t *)region + MW_WINDOW_REPLY_OFFSET_Z2_2SLOT;
        g_is_bound = 1;
        mw_service_init();
        g_ctrl->slot_capacity = MW_DEFAULT_SLOT_CAPACITY_Z2;
        g_ctrl->slot_count = MW_DEFAULT_SLOT_COUNT_Z2;
    } else if (size >= MW_WINDOW_SIZE_Z2_1SLOT) { /* Zorro II single slot layout (1 slot + 16K reply) */
        g_slot_ptrs[0] = (uint8_t *)region + MW_WINDOW_SLOT0_OFFSET;
        g_slot_ptrs[1] = NULL;
        g_reply_ptr = (uint8_t *)region + MW_WINDOW_REPLY_OFFSET_Z2_1SLOT;
        g_is_bound = 1;
        mw_service_init();
        g_ctrl->slot_capacity = MW_DEFAULT_SLOT_CAPACITY_Z2;
        g_ctrl->slot_count = 1U;
    } else {
        g_slot_ptrs[0] = NULL;
        g_slot_ptrs[1] = NULL;
        g_reply_ptr = NULL;
        g_is_bound = 1;
        mw_service_init();
    }
    return 1;
}

int mw_service_is_bound(void)
{
    return g_is_bound || (g_ctrl != NULL && g_status != NULL);
}

void mw_service_init(void)
{
    /* No transport window is bound: the service stays unavailable and every
     * entry point no-ops. The window only exists after the firmware-side
     * transport glue (minigl_transport.c) allocates it from the host-visible
     * heap and binds it, or a host test binds an explicit region. */
    if (!g_ctrl || !g_slot_ptrs[0] || !g_reply_ptr)
        return;

    /* Initialize firmware ownership block (bytes 0..63) */
    g_ctrl->magic = MW_SERVICE_MAGIC;
    g_ctrl->transport_version = MW_SERVICE_TRANSPORT_VER;
    g_ctrl->wire_version = MW_SERVICE_WIRE_VER;
    /* Absorb host progress instead of replaying it: re-init runs on core-1
     * (re)start and on transport resets, where the host-owned counters can
     * be ahead of the firmware-owned ones. Resetting consumer_seq to 0
     * would re-drain every slot retired before the re-init as a phantom
     * backlog (stale-packet replay); adopting producer_seq retires them. */
    g_ctrl->consumer_seq = g_ctrl->producer_seq;
    g_ctrl->completion_fence = g_ctrl->producer_seq;
    g_ctrl->slot_capacity = MW_DEFAULT_SLOT_CAPACITY_Z3;
    g_ctrl->slot_count = MW_DEFAULT_SLOT_COUNT_Z3;
    g_ctrl->firmware_heartbeat = 0;
    g_ctrl->op_reply_bytes = 0;
    g_ctrl->op_reply_handle = 0;
    /* Same for a synchronous op left pending by a previous worker life:
     * complete it with CONTEXT_LOST rather than re-executing stale op
     * fields. A host waiting on the fence observes the error and recovers
     * via the generation change; a freshly bound window has op_seq 0, so
     * nothing is published there. */
    if (g_ctrl->op_seq != g_ctrl->op_done_seq) {
        g_ctrl->op_status = MW_STATUS_CONTEXT_LOST;
        g_ctrl->op_done_seq = g_ctrl->op_seq;
    } else {
        g_ctrl->op_status = 0;
    }
    /* Initialize status block */
    if (g_status) {
        g_status->generation = g_generation;
        g_status->error = 0;
        g_status->error_sequence_hi = 0;
        g_status->error_sequence_lo = 0;
        g_status->firmware_epoch = 0;
        g_status->heartbeat = 0;
        g_status->capabilities = (1U << 18); /* Wire v9 capability bit */
    }
    g_doorbell_pending = 0;
    MW_DIAG("[MGL9] publish magic=%08lx wire=%u transport=%u slot_bytes=%lu slots=%lu caps=%08lx\n",
            (unsigned long)g_ctrl->magic, (unsigned)g_ctrl->wire_version,
            (unsigned)g_ctrl->transport_version, (unsigned long)g_ctrl->slot_capacity,
            (unsigned long)g_ctrl->slot_count,
            (unsigned long)(g_status ? g_status->capabilities : 0U));
}

uint32_t mw_service_transport_window_base(void)
{
    return g_slot_ptrs[0] ? (uint32_t)(uintptr_t)(g_slot_ptrs[0] - MW_WINDOW_SLOT0_OFFSET) : 0U;
}

uint32_t mw_service_transport_window_size(void)
{
    if (!g_slot_ptrs[0])
        return 0U;
    if (g_slot_ptrs[1] == g_slot_ptrs[0] + MW_WINDOW_SLOT_STRIDE_Z3)
        return MW_WINDOW_SIZE_Z3;
    if (g_slot_ptrs[1] == g_slot_ptrs[0] + MW_WINDOW_SLOT_STRIDE_Z2)
        return MW_WINDOW_SIZE_Z2_2SLOT;
    if (g_slot_ptrs[1] == NULL)
        return MW_WINDOW_SIZE_Z2_1SLOT;
    return 0U;
}

void mw_service_set_vram_limit_arm(uint32_t limit_arm)
{
    g_vram_limit_arm = limit_arm;
}

int mw_service_get_transport_descriptor(ZZ9KMiniGLTransportDescriptor *out)
{
    uint32_t base;
    uint32_t size;

    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (!g_ctrl || !g_slot_ptrs[0] || !g_reply_ptr || !g_is_bound)
        return -1;
    base = mw_service_transport_window_base();
    size = mw_service_transport_window_size();
    if (!base || !size)
        return -1;

    out->magic = MW_TRANSPORT_DESCRIPTOR_MAGIC;
    out->descriptor_version = MW_TRANSPORT_DESCRIPTOR_VERSION;
    out->transport_version = MW_SERVICE_TRANSPORT_VER;
    out->generation = g_generation;
    out->window_address = base;
    out->window_bytes = size;
    out->address_adjust = 0x001F0000U; /* ADDR_ADJ: board offset = ARM - adjust */
    out->ctrl_offset = MW_WINDOW_CTRL_OFFSET;
    out->status_offset = MW_WINDOW_STATUS_OFFSET;
    out->slot0_offset = MW_WINDOW_SLOT0_OFFSET;
    out->slot1_offset = (g_slot_ptrs[1] != NULL) ?
        (uint32_t)(MW_WINDOW_SLOT0_OFFSET + (g_slot_ptrs[1] - g_slot_ptrs[0])) : 0U;
    out->slot_capacity = g_ctrl->slot_capacity;
    out->slot_count = g_ctrl->slot_count;
    out->reply_offset = (uint32_t)(g_reply_ptr - (uint8_t *)(uintptr_t)base);
    out->reply_capacity = (out->reply_offset == MW_WINDOW_REPLY_OFFSET_Z3) ?
        MW_DEFAULT_REPLY_CAPACITY : MW_DEFAULT_REPLY_CAPACITY_Z2;
    out->required_alignment = 4U;
    out->cache_rules = MW_TRANSPORT_CACHE_ARM_NONCACHEABLE;
    out->vram_limit_arm = g_vram_limit_arm;
    return 0;
}


void mw_service_serialize_transport_descriptor(
    const ZZ9KMiniGLTransportDescriptor *desc,
    ZZ9KMiniGLTransportDescriptorWire *out)
{
    if (!desc || !out)
        return;
    memset(out, 0, sizeof(*out));
    mw_put_be32(out->magic, desc->magic);
    mw_put_be16(out->descriptor_version, desc->descriptor_version);
    mw_put_be16(out->transport_version, desc->transport_version);
    mw_put_be32(out->generation, desc->generation);
    mw_put_be32(out->window_address, desc->window_address);
    mw_put_be32(out->window_bytes, desc->window_bytes);
    mw_put_be32(out->address_adjust, desc->address_adjust);
    mw_put_be32(out->ctrl_offset, desc->ctrl_offset);
    mw_put_be32(out->status_offset, desc->status_offset);
    mw_put_be32(out->slot0_offset, desc->slot0_offset);
    mw_put_be32(out->slot1_offset, desc->slot1_offset);
    mw_put_be32(out->slot_capacity, desc->slot_capacity);
    mw_put_be32(out->slot_count, desc->slot_count);
    mw_put_be32(out->reply_offset, desc->reply_offset);
    mw_put_be32(out->reply_capacity, desc->reply_capacity);
    mw_put_be32(out->required_alignment, desc->required_alignment);
    mw_put_be32(out->cache_rules, desc->cache_rules);
    mw_put_be32(out->vram_limit_arm, desc->vram_limit_arm);
}

void mw_service_deserialize_transport_descriptor(
    const ZZ9KMiniGLTransportDescriptorWire *wire,
    ZZ9KMiniGLTransportDescriptor *out)
{
    if (!wire || !out)
        return;
    memset(out, 0, sizeof(*out));
    out->magic = mw_be32(wire->magic);
    out->descriptor_version = ((uint16_t)wire->descriptor_version[0] << 8) |
                              wire->descriptor_version[1];
    out->transport_version = ((uint16_t)wire->transport_version[0] << 8) |
                             wire->transport_version[1];
    out->generation = mw_be32(wire->generation);
    out->window_address = mw_be32(wire->window_address);
    out->window_bytes = mw_be32(wire->window_bytes);
    out->address_adjust = mw_be32(wire->address_adjust);
    out->ctrl_offset = mw_be32(wire->ctrl_offset);
    out->status_offset = mw_be32(wire->status_offset);
    out->slot0_offset = mw_be32(wire->slot0_offset);
    out->slot1_offset = mw_be32(wire->slot1_offset);
    out->slot_capacity = mw_be32(wire->slot_capacity);
    out->slot_count = mw_be32(wire->slot_count);
    out->reply_offset = mw_be32(wire->reply_offset);
    out->reply_capacity = mw_be32(wire->reply_capacity);
    out->required_alignment = mw_be32(wire->required_alignment);
    out->cache_rules = mw_be32(wire->cache_rules);
    out->vram_limit_arm = mw_be32(wire->vram_limit_arm);
}
void mw_service_doorbell(void)
{
    g_doorbell_pending = 1;
}

int mw_service_has_work(void)
{
    if (!g_ctrl)
        return 0;

    return g_doorbell_pending ||
           (g_ctrl->producer_seq > g_ctrl->consumer_seq) ||
           (g_ctrl->op_seq != g_ctrl->op_done_seq);
}

void mw_service_reset_local(void)
{
    uint32_t i;
    for (i = 0; i < MW_MAX_CONTEXTS; ++i) {
        if (g_contexts[i]) {
            mw_host_destroy(g_contexts[i]);
            g_contexts[i] = NULL;
        }
    }
    g_active_handle = 0;
    mw_service_init();
}

void mw_service_reset(void)
{
    g_generation++;
    mw_service_reset_local();
}

void mw_service_on_core1_fault(void)
{
    g_generation++;
    if (g_status) {
        g_status->generation = g_generation;
        g_status->error = 3U; /* Reset error */
    }
    mw_service_reset_local();
}

int mw_service_poll(void)
{
    int work_done = 0;

    if (!g_ctrl)
        return 0;

    g_doorbell_pending = 0;
    g_ctrl->firmware_heartbeat++;
    if (g_status) {
        g_status->heartbeat = g_ctrl->firmware_heartbeat;
        g_status->trace_draws_hardware = minigl_raster_draws_hardware();
        g_status->trace_draws_software = minigl_raster_draws_software();
        g_status->trace_fallback_reason = minigl_raster_first_fallback_reason();
    }

    /* 1. Process asynchronous command packets from slot ring.
     * Guard rails: a gap no compliant host can produce means uninitialized
     * or destroyed control memory -- resynchronize once instead of
     * draining a phantom backlog (the boot-loop failure), and never drain
     * more than MW_POLL_MAX_RING_BATCH packets per call so this poll
     * always returns to its scheduler. */
    uint32_t producer = g_ctrl->producer_seq;
    uint32_t outstanding = producer - g_ctrl->consumer_seq;
    uint32_t batch;

    if (outstanding > MW_RING_MAX_OUTSTANDING) {
        g_ctrl->consumer_seq = producer;
        g_ctrl->completion_fence = producer;
        mw_log_reject(MW_STATUS_INVALID, 0U);
        work_done++;
    } else {
        batch = (outstanding < MW_POLL_MAX_RING_BATCH) ? outstanding
                                                       : MW_POLL_MAX_RING_BATCH;
        while (batch--) {
            uint32_t slot_idx = g_ctrl->consumer_seq % g_ctrl->slot_count;
            const uint8_t *slot_buf = g_slot_ptrs[slot_idx];

            if (slot_buf) {
                uint32_t packet_bytes = mw_be32(slot_buf + 8);
                uint32_t target_handle = g_ctrl->op_handle ? g_ctrl->op_handle : g_active_handle;
                mw_ctx *c = mw_service_find_context(target_handle);

                if (c && packet_bytes >= 16 && packet_bytes <= g_ctrl->slot_capacity) {
                    uint32_t opcode = 0U;
                    uint32_t status = mw_validate_packet_report(slot_buf, packet_bytes, &opcode);
                    if (status == MW_STATUS_OK)
                        status = mw_host_submit(c, slot_buf, packet_bytes);
                    g_ctrl->op_status = status;
                    if (status != MW_STATUS_OK)
                        mw_log_reject(status, opcode);
                } else if (!c) {
                    g_ctrl->op_status = MW_STATUS_CONTEXT_LOST;
                    mw_log_reject(g_ctrl->op_status,
                                  mw_packet_first_opcode(slot_buf, packet_bytes));
                } else {
                    g_ctrl->op_status = MW_STATUS_INVALID;
                    mw_log_reject(g_ctrl->op_status,
                                  mw_packet_first_opcode(slot_buf, packet_bytes));
                }
            }

            g_ctrl->consumer_seq++;
            g_ctrl->completion_fence = g_ctrl->consumer_seq;
            work_done++;
        }
    }

    /* 2. Process synchronous bridge operations (op_code != 0) */
    if (g_ctrl->op_seq != g_ctrl->op_done_seq) {
        uint32_t op = g_ctrl->op_code;
        uint32_t handle = g_ctrl->op_handle;

        switch (op) {
        case 1: { /* CREATE: width = op_arg1, height = op_arg2 */
            uint32_t width = g_ctrl->op_arg1;
            uint32_t height = g_ctrl->op_arg2;
            uint32_t slot;
            mw_ctx *c;

            /* Find empty slot */
            for (slot = 0; slot < MW_MAX_CONTEXTS; ++slot) {
                if (!g_contexts[slot])
                    break;
            }

            if (slot < MW_MAX_CONTEXTS) {
                uint32_t new_handle = (g_generation << 16) | (slot + 1U);
                c = mw_host_create_owned(width, height,
                    CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL, new_handle));
                if (c) {
                    uint32_t flags = g_ctrl->op_arg3;
                    if (flags & 1U)
                        c->state.force_cpu = 1;
                    c->handle = new_handle;
                    g_contexts[slot] = c;
                    g_active_handle = new_handle;
                    g_ctrl->op_reply_handle = new_handle;
                    g_ctrl->op_status = MW_STATUS_OK;
                    const char *backend = mw_backend_name(c);
                    (void)backend;
                    MW_DIAG("[MGL9] create handle=%08lx size=%lux%lu backend=%s\n",
                            (unsigned long)new_handle, (unsigned long)width,
                            (unsigned long)height, backend);
                } else {
                    g_ctrl->op_reply_handle = 0;
                    g_ctrl->op_status = MW_STATUS_FAILED;
                    MW_DIAG("[MGL9] create handle=%08lx size=%lux%lu backend=none\n",
                            0UL, (unsigned long)width, (unsigned long)height);
                }
            } else {
                g_ctrl->op_reply_handle = 0;
                g_ctrl->op_status = MW_STATUS_CAPACITY;
                MW_DIAG("[MGL9] create handle=%08lx size=%lux%lu backend=none\n",
                        0UL, (unsigned long)width, (unsigned long)height);
            }
            break;
        }

        case 3: { /* READBACK: op_handle, op_arg1 = bytes */
            mw_ctx *c = mw_service_find_context(handle);
            if (!c || !g_reply_ptr) {
                g_ctrl->op_status = MW_STATUS_UNAVAILABLE;
            } else {
                uint32_t cap = g_ctrl->op_arg1 ? g_ctrl->op_arg1 : (g_ctrl->op_arg3 ? g_ctrl->op_arg3 : MW_DEFAULT_REPLY_CAPACITY);
                uint32_t status = mw_host_readback_rgba8(c, g_reply_ptr, cap);
                g_ctrl->op_status = status;
                g_ctrl->op_reply_bytes = (status == MW_STATUS_OK) ? (c->width * c->height * 4) : 0;
            }
            break;
        }

        case 4: { /* DESTROY: op_handle */
            uint32_t slot = (handle & 0xffffU);
            if (slot >= 1U && slot <= MW_MAX_CONTEXTS) {
                mw_ctx *c = g_contexts[slot - 1U];
                if (c && c->handle == handle) {
                    uint32_t width = c->width;
                    uint32_t height = c->height;
                    const char *backend = mw_backend_name(c);
                    (void)width;
                    (void)height;
                    (void)backend;
                    mw_host_destroy(c);
                    g_contexts[slot - 1U] = NULL;
                    if (g_active_handle == handle)
                        g_active_handle = 0;
                    g_ctrl->op_status = MW_STATUS_OK;
                    MW_DIAG("[MGL9] destroy handle=%08lx size=%lux%lu backend=%s\n",
                            (unsigned long)handle, (unsigned long)width,
                            (unsigned long)height, backend);
                } else {
                    g_ctrl->op_status = MW_STATUS_UNAVAILABLE;
                    MW_DIAG("[MGL9] destroy handle=%08lx size=0x0 backend=none\n",
                            (unsigned long)handle);
                }
            } else {
                g_ctrl->op_status = MW_STATUS_UNAVAILABLE;
                MW_DIAG("[MGL9] destroy handle=%08lx size=0x0 backend=none\n",
                        (unsigned long)handle);
            }
            break;
        }

        case 5: { /* REQUEST: op_handle, op_arg1 = req bytes, op_arg3 = reply capacity */
            mw_ctx *c = mw_service_find_context(handle);
            if (!c || !g_reply_ptr) {
                g_ctrl->op_status = MW_STATUS_UNAVAILABLE;
            } else {
                uint32_t req_bytes = g_ctrl->op_arg1;
                uint32_t reply_cap = g_ctrl->op_arg3 ? g_ctrl->op_arg3 : MW_DEFAULT_REPLY_CAPACITY;
                uint32_t reply_written = 0;
                uint32_t status = mw_host_request(c, g_reply_ptr, req_bytes,
                                                  g_reply_ptr, reply_cap, &reply_written);
                g_ctrl->op_status = status;
                g_ctrl->op_reply_bytes = reply_written;
            }
            break;
        }

        case 6: { /* BLIT / CAPTURE: Direct VRAM presentation / capture */
            mw_ctx *c = mw_service_find_context(handle);
            if (!c || !g_reply_ptr) {
                g_ctrl->op_status = MW_STATUS_UNAVAILABLE;
            } else {
                uint32_t req_bytes = g_ctrl->op_arg1;
                uint32_t card_offset = g_ctrl->op_arg2;
                uint32_t span_total = g_ctrl->op_arg3;
                uint32_t arm = card_offset + 0x001F0000UL;

                /* Check bounds against the card's RTG VRAM range */
                if (card_offset < MNT_FB_BASE || arm < MW_VRAM_ARM_BASE ||
                    span_total == 0 || (uint64_t)arm + span_total > g_vram_limit_arm) {
                    g_ctrl->op_status = MW_STATUS_UNAVAILABLE;
                } else if (req_bytes < 24) {
                    g_ctrl->op_status = MW_STATUS_INVALID;
                } else {
#if defined(__arm__) && !defined(MINIGL_HOST_TEST)
                    Xil_DCacheInvalidateRange((INTPTR)g_reply_ptr, req_bytes);
#endif
                    uint32_t req_id = mw_be32(g_reply_ptr);
                    if (req_id != MW_BLIT && req_id != MW_CAPTURE) {
                        g_ctrl->op_status = MW_STATUS_INVALID;
                    } else {
#if defined(MINIGL_HOST_TEST)
                        uint8_t *span = g_host_vram_base ? (g_host_vram_base + card_offset)
                                                         : (uint8_t *)(uintptr_t)arm;
#else
                        uint8_t *span = (uint8_t *)(uintptr_t)arm;
#endif

#if defined(__arm__) && !defined(MINIGL_HOST_TEST)
                        if (req_id == MW_CAPTURE) {
                            mw_service_cache_maintain_span(arm, g_reply_ptr, req_bytes, 0 /* invalidate */);
                        }
#endif
                        uint32_t reply_written = 0;
                        uint32_t status = mw_host_request(c, g_reply_ptr, req_bytes,
                                                          span, span_total, &reply_written);
                        g_ctrl->op_status = status;
                        g_ctrl->op_reply_bytes = reply_written;

#if defined(__arm__) && !defined(MINIGL_HOST_TEST)
                        if (req_id == MW_BLIT && status == MW_STATUS_OK) {
                            mw_service_cache_maintain_span(arm, g_reply_ptr, req_bytes, 1 /* clean + dsb */);
                        }
#endif
                    }
                }
            }
            break;
        }

        default:
            g_ctrl->op_status = MW_STATUS_INVALID;
            mw_log_reject(g_ctrl->op_status, op);
            break;
        }

        g_ctrl->op_done_seq = g_ctrl->op_seq; /* Publish completion in firmware block */
        work_done++;
    }

    return work_done;
}
