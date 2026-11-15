/*
 * PIE ELF loader for one trusted ARM app.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Relocations are SHT_RELA / R_ARM_RELATIVE only. A raw Arm ld image still
 * has SHT_REL; that is rejected. The place is overwritten with
 * r_addend + load_bias. It is not added to the old word.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zzapp.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"

#define EM_ARM                 40u
#define ET_DYN                 3u
#define PT_LOAD                1u
#define PT_INTERP              3u
#define PT_NOTE                4u
#define PT_DYNAMIC             2u
#define PF_X                   1u
#define PF_W                   2u
#define PF_R                   4u
#define SHT_RELA               4u
#define SHT_NOTE               7u
#define SHT_REL                9u
#define SHF_ALLOC              2u
#define R_ARM_RELATIVE         23u
#define EF_ARM_ABI_FLOAT_HARD  0x400u
#define DT_NEEDED              1
#define DT_NULL                0
#define DT_REL                 17
#define ELF_NOTE_ALIGN         4u
#define ZZAPP_MAX_PHDR         32u
#define ZZAPP_MAX_SHDR         64u
#define ZZAPP_MAX_LOAD         4u

struct load_seg {
    uint32_t off;
    uint32_t vaddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
};

struct load_image {
    uint32_t entry;
    uint32_t stack_bytes;
    uint32_t note_flags;
    uint32_t note_minor;
    uint32_t min_heap_bytes;
    uint32_t image_end;
    uint32_t nseg;
    struct load_seg seg[ZZAPP_MAX_LOAD];
};

static uint32_t image_proc_limit(const struct load_image *img)
{
    uint32_t limit;

    if (img->note_minor < 3u)
        return ZZAPP_ARENA_BYTES;
    limit = zzapp_services_memory_limit();
    return limit != 0u && limit < ZZAPP_MEM_PRIVATE_WINDOW_BYTES ?
           limit : ZZAPP_MEM_PRIVATE_WINDOW_BYTES;
}

static int image_commit_fits_limit(const struct load_image *img)
{
    uint32_t image = (img->image_end + ZZAPP_MEM_FRAME_BYTES - 1u) &
                     ~(ZZAPP_MEM_FRAME_BYTES - 1u);
    uint32_t stack = (img->stack_bytes + ZZAPP_MEM_FRAME_BYTES - 1u) &
                     ~(ZZAPP_MEM_FRAME_BYTES - 1u);
    uint32_t commit = img->note_minor < 3u ? ZZAPP_ARENA_BYTES :
                      image + stack + img->min_heap_bytes;

    return commit <= image_proc_limit(img);
}

/* Defined in zzapp_host.c. The loader does not own the lifecycle words. */
void zzapp_loader_bound(uint32_t card, uint8_t *mem, uint32_t bytes,
                        uint32_t owner, uint32_t generation);
void zzapp_loader_unbound(void);
uint32_t zzapp_loader_generation(void);
uint32_t zzapp_loader_state(void);
void zzapp_loader_set_state(uint32_t state);
void zzapp_services_set_state(uint32_t state);
int zzapp_loader_set_image(uint32_t entry, uint32_t image_end,
                           uint32_t stack_bytes, uint32_t note_flags,
                           uint32_t note_minor, uint32_t min_heap_bytes);
int zzapp_loader_have_image(void);

struct loader_slot {
    uint32_t card;
    uint8_t *mem;
    uint32_t bytes;
    uint32_t owner;
    uint32_t generation;
    uint32_t file_end;
    uint32_t back;
    uint32_t back_owner;
    int ready;
    uint32_t zero_frames;
    uint32_t zeroed_frames;
    uint32_t zero_offset;
    uint32_t map_segment;
    uint32_t map_offset;
    uint32_t map_page;
    uint32_t map_tail;
    uint8_t map_phase;
    struct load_image image;
    uint8_t *relocs;
    uint32_t relocs_bytes;
    uint32_t reloc_entry;
    uint32_t reloc_written;
    uint32_t reloc_apply;
    uint16_t reloc_section;
    uint8_t reloc_prepare_started;
    int image_prepared;
#ifdef ZZAPP_HOST_TEST
    uint8_t *host_mem;
    uint8_t *host_back;
    uint8_t *host_file;
    uint32_t host_zero_last_bytes;
    uint32_t host_work_last_bytes;
#endif

};
uint32_t zzapp_services_current_slot(void) __attribute__((weak));

static struct loader_slot g_loader[ZZAPP_AS_SLOT_COUNT];

static uint32_t loader_slot(void)
{
    uint32_t slot = zzapp_services_current_slot ? zzapp_services_current_slot() : 0U;

    return slot < ZZAPP_AS_SLOT_COUNT ? slot : 0U;
}

#define g_card       (g_loader[loader_slot()].card)
#define g_mem        (g_loader[loader_slot()].mem)
#define g_bytes      (g_loader[loader_slot()].bytes)
#define g_owner      (g_loader[loader_slot()].owner)
#define g_file_end   (g_loader[loader_slot()].file_end)
#define g_back       (g_loader[loader_slot()].back)
#define g_zero_frames (g_loader[loader_slot()].zero_frames)
#define g_zeroed_frames (g_loader[loader_slot()].zeroed_frames)
#define g_image      (g_loader[loader_slot()].image)
#define g_relocs     (g_loader[loader_slot()].relocs)
#define g_relocs_bytes (g_loader[loader_slot()].relocs_bytes)
#define g_reloc_entry (g_loader[loader_slot()].reloc_entry)
#define g_reloc_written (g_loader[loader_slot()].reloc_written)
#define g_reloc_apply (g_loader[loader_slot()].reloc_apply)
#define g_reloc_section (g_loader[loader_slot()].reloc_section)
#define g_reloc_prepare_started (g_loader[loader_slot()].reloc_prepare_started)
#define g_image_prepared (g_loader[loader_slot()].image_prepared)
#define g_back_owner (g_loader[loader_slot()].back_owner)
#define g_generation (g_loader[loader_slot()].generation)
#define g_ready      (g_loader[loader_slot()].ready)
#define g_zero_offset (g_loader[loader_slot()].zero_offset)
#define g_map_segment (g_loader[loader_slot()].map_segment)
#define g_map_offset (g_loader[loader_slot()].map_offset)
#define g_map_page (g_loader[loader_slot()].map_page)
#define g_map_tail (g_loader[loader_slot()].map_tail)
#define g_map_phase (g_loader[loader_slot()].map_phase)
#ifdef ZZAPP_HOST_TEST
#define g_host_mem   (g_loader[loader_slot()].host_mem)
#define g_host_back  (g_loader[loader_slot()].host_back)
#define HOST_CARD 0xc0000000u
#define g_host_file  (g_loader[loader_slot()].host_file)
#define g_host_zero_last_bytes \
    (g_loader[loader_slot()].host_zero_last_bytes)
#define g_host_work_last_bytes \
    (g_loader[loader_slot()].host_work_last_bytes)
#endif
#define CARD_POOL_OWNER_SLOT_SHIFT 22u
#define CARD_POOL_OWNER_GENERATION_MASK ((1u << CARD_POOL_OWNER_SLOT_SHIFT) - 1u)

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static int range_ok(uint32_t off, uint32_t len, uint32_t limit)
{
    return len <= limit && off <= limit - len;
}

/* PF_* flags for the image page that starts at `page`: the union of every
 * PT_LOAD whose memory touches it. The SDK linker script packs .data right
 * after .text, so one page often holds code and data. Mapping each segment
 * with only its own flags let the later segment win, which made shared
 * code pages non-executable or shared data pages read-only.
 *
 * The heap (and argv) starts right after `image_end`, so the rest of the
 * page holding image_end is also writable. swquake's last PT_LOAD is a
 * read-only .rela.dyn, and its first malloc wrote into that page. */
uint32_t zzapp_loader_page_flags(const uint32_t (*segs)[3], uint32_t nseg,
                                 uint32_t image_end, uint32_t page)
{
    uint32_t flags = 0;
    uint32_t i;

    for (i = 0; i < nseg; i++) {
        uint32_t start = segs[i][0];
        uint32_t end = start + segs[i][1];

        if (segs[i][1] && start < page + 0x1000u && end > page)
            flags |= segs[i][2];
    }
    if (image_end > page && image_end < page + 0x1000u)
        flags |= PF_R | PF_W;
    return flags;
}

#ifndef ZZAPP_LOADER_WORK_CHUNK_BYTES
#define ZZAPP_LOADER_WORK_CHUNK_BYTES (1024u * 1024u)
#endif
#define ZZAPP_LOADER_RELOC_CHUNK_ENTRIES 4096U

enum loader_map_phase {
    LOADER_MAP_ZERO = 0,
    LOADER_MAP_SETUP,
    LOADER_MAP_COPY,
    LOADER_MAP_IMAGE,
    LOADER_MAP_TAIL
};

static void loader_note_work(uint32_t bytes)
{
#ifdef ZZAPP_HOST_TEST
    g_host_work_last_bytes = bytes;
#else
    (void)bytes;
#endif
}


#if !defined(ZZAPP_HOST_TEST) || defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
#include "card_pool.h"
#if !defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
#include "memorymap.h"
#include "xil_cache.h"
#include "xil_cache_l.h"
#else
typedef uintptr_t INTPTR;
#define Xil_L1DCacheFlushRange(address, bytes) ((void)(address), (void)(bytes))
#define Xil_ICacheInvalidateRange(address, bytes) ((void)(address), (void)(bytes))
#endif

static zzapp_mem_t g_app_memory;
static int g_app_memory_ready;

void zzapp_loader_memory_init(void)
{
    if (!g_app_memory_ready &&
        zzapp_mem_init(&g_app_memory, &card_pool) == 0) {
        zzapp_host_register_low_memory_callback(&g_app_memory);
        g_app_memory_ready = 1;
    }
}

#ifdef ZZAPP_LOADER_INCREMENTAL_HOST_TEST
static uint32_t pool_alloc(uint32_t bytes, uint32_t owner)
{
    (void)owner;
    if (bytes == ZZAPP_ARENA_BYTES) {
        g_host_mem = (uint8_t *)malloc(bytes);
        return g_host_mem ? HOST_CARD : 0U;
    }
    g_host_back = (uint8_t *)malloc(bytes);
    return g_host_back ? HOST_CARD - 0x01000000U : 0U;
}
#else
static uint32_t pool_alloc(uint32_t bytes, uint32_t owner)
{
    uint32_t addr;
    uint32_t end;

    addr = card_pool_alloc(&card_pool, bytes, owner);
    if (!addr)
        return 0;
    end = addr + bytes;
    if (end < addr || addr < CARD_POOL_PERMANENT_ADDRESS ||
        end > CARD_POOL_PERMANENT_END) {
        card_pool_free(&card_pool, addr, owner);
        return 0;
    }
    return addr;
}
#endif
#ifdef ZZAPP_LOADER_INCREMENTAL_HOST_TEST
static uint32_t g_host_mapping_calls;
static int host_as_create(zzapp_as_t *as, uint32_t slot, uint32_t generation)
{
    (void)as;
    (void)slot;
    (void)generation;
    g_host_mapping_calls++;
    return 0;
}
static void host_as_destroy(zzapp_as_t *as) { (void)as; }
static int host_as_map_frames(zzapp_as_t *as, uint32_t va,
                              const uint32_t *frames, uint32_t count)
{
    (void)as; (void)va; (void)frames; (void)count;
    return 0;
}
static int host_as_map_image(zzapp_as_t *as, uint32_t va, uint32_t pa,
                             uint32_t bytes, uint32_t permissions)
{
    (void)as; (void)va; (void)pa; (void)bytes; (void)permissions;
    return 0;
}
static int host_as_unmap_frames(zzapp_as_t *as, uint32_t va, uint32_t count)
{
    (void)as; (void)va; (void)count;
    return 0;
}
#define zzapp_as_create host_as_create
#define zzapp_as_destroy host_as_destroy
#define zzapp_as_map_frames host_as_map_frames
#define zzapp_as_map_image host_as_map_image
#define zzapp_as_unmap_frames host_as_unmap_frames
#endif




static uint32_t loader_image_permissions(uint32_t flags)
{
    if (flags & PF_X)
        return (flags & PF_W) ? ZZAPP_AS_IMAGE_RWX : ZZAPP_AS_IMAGE_RX;
    return (flags & PF_W) ? ZZAPP_AS_IMAGE_RW : ZZAPP_AS_IMAGE_RO;
}

static int map_loaded_image(const struct load_image *img)
{
    zzapp_proc_t *proc = zzapp_proc_slot(loader_slot());
    uint32_t image_frames;
    uint32_t stack_frames;
    uint32_t heap_frames;
    uint32_t total_frames;
    uint32_t proc_limit;

#ifdef ZZAPP_HOST_TEST
    g_host_work_last_bytes = 0U;
#endif
    if (!g_app_memory_ready || !proc ||
        img->image_end > ZZAPP_AS_ALIAS_BASE - ZZAPP_AS_WINDOW_BASE)
        return ZZ_ENOMEM;
    image_frames = (img->image_end + ZZAPP_MEM_FRAME_BYTES - 1u) >> 20;
    stack_frames = (img->stack_bytes + ZZAPP_MEM_FRAME_BYTES - 1u) >> 20;
    total_frames = img->note_minor < 3u ?
                   ZZAPP_ARENA_BYTES / ZZAPP_MEM_FRAME_BYTES :
                   image_frames + stack_frames +
                   img->min_heap_bytes / ZZAPP_MEM_FRAME_BYTES;
    if (image_frames > ZZAPP_MEM_MAX_FRAMES ||
        total_frames > ZZAPP_MEM_MAX_FRAMES)
        return ZZ_ENOMEM;
    heap_frames = total_frames - image_frames - stack_frames;
    proc_limit = image_proc_limit(img);
    if (g_zero_frames == 0U) {
        zzapp_mem_proc_init(&proc->memory, proc_limit);
        if (zzapp_mem_alloc_unzeroed(&g_app_memory, &proc->memory,
                                     image_frames, proc->image_frames) ||
            (heap_frames &&
             zzapp_mem_alloc_unzeroed(&g_app_memory, &proc->memory,
                                      heap_frames, proc->heap_frames)) ||
            zzapp_mem_alloc_unzeroed(&g_app_memory, &proc->memory,
                                     stack_frames, proc->stack_frames))
            goto failed;
        proc->image_frame_count = image_frames;
        proc->heap_frame_count = heap_frames;
        proc->stack_frame_count = stack_frames;
        g_zero_frames = total_frames;
        g_zeroed_frames = 0U;
        g_zero_offset = 0U;
        g_map_phase = LOADER_MAP_ZERO;
    }
    if (g_map_phase == LOADER_MAP_ZERO) {
        uint32_t count;
        uint8_t *dst;

        if (g_zeroed_frames == g_zero_frames) {
#ifdef ZZAPP_HOST_TEST
            g_host_zero_last_bytes = 0U;
#endif
            g_map_phase = LOADER_MAP_SETUP;
            return ZZAPP_LOADER_IN_PROGRESS;
        }
        count = ZZAPP_MEM_FRAME_BYTES - g_zero_offset;
        if (count > ZZAPP_LOADER_WORK_CHUNK_BYTES)
            count = ZZAPP_LOADER_WORK_CHUNK_BYTES;
        dst = (uint8_t *)(uintptr_t)proc->memory.frames[g_zeroed_frames] +
              g_zero_offset;
        memset(dst, 0, count);
        loader_note_work(count);
#ifdef ZZAPP_HOST_TEST
        g_host_zero_last_bytes = count;
#endif
        g_zero_offset += count;
        if (g_zero_offset == ZZAPP_MEM_FRAME_BYTES) {
            g_zero_offset = 0U;
            g_zeroed_frames++;
        }
        return ZZAPP_LOADER_IN_PROGRESS;
    }
    if (g_map_phase == LOADER_MAP_SETUP) {
        if (zzapp_as_create(&proc->as, loader_slot(), proc->generation))
            goto failed;
        proc->heap_base = image_frames * ZZAPP_MEM_FRAME_BYTES;
        if (img->note_minor < 3u) {
            proc->stack_base = ZZAPP_ARENA_BYTES - stack_frames *
                               ZZAPP_MEM_FRAME_BYTES;
            proc->heap_limit = proc->stack_base;
        } else {
            proc->stack_base = ZZAPP_MEM_PRIVATE_WINDOW_BYTES - stack_frames *
                               ZZAPP_MEM_FRAME_BYTES;
            if (proc->stack_base < ZZAPP_MEM_FRAME_BYTES)
                goto failed;
            proc->heap_limit = ZZAPP_AS_ALIAS_BASE - ZZAPP_AS_WINDOW_BASE;
        }
        if (proc->heap_base > proc->heap_limit ||
            heap_frames > (proc->heap_limit - proc->heap_base) /
                          ZZAPP_MEM_FRAME_BYTES)
            goto failed;
        proc->stack_top = proc->stack_base +
                          stack_frames * ZZAPP_MEM_FRAME_BYTES;
        proc->heap_start = proc->heap_base;
        proc->heap_break = proc->heap_base;
        if ((heap_frames &&
             zzapp_as_map_frames(&proc->as,
                                 ZZAPP_AS_WINDOW_BASE + proc->heap_base,
                                 proc->heap_frames, heap_frames)) ||
            zzapp_as_map_frames(&proc->as,
                                ZZAPP_AS_WINDOW_BASE + proc->stack_base,
                                proc->stack_frames, stack_frames))
            goto failed;
        g_map_segment = 0U;
        g_map_offset = 0U;
        g_map_phase = LOADER_MAP_COPY;
        return ZZAPP_LOADER_IN_PROGRESS;
    }
    if (g_map_phase == LOADER_MAP_COPY) {
        while (g_map_segment < img->nseg) {
            const struct load_seg *seg = &img->seg[g_map_segment];
            uint32_t va;
            uint32_t in_frame;
            uint32_t count;
            uint8_t *dst;

            if (g_map_offset == seg->memsz) {
                g_map_segment++;
                g_map_offset = 0U;
                continue;
            }
            va = seg->vaddr + g_map_offset;
            in_frame = va & (ZZAPP_MEM_FRAME_BYTES - 1u);
            count = seg->memsz - g_map_offset;
            if (count > ZZAPP_MEM_FRAME_BYTES - in_frame)
                count = ZZAPP_MEM_FRAME_BYTES - in_frame;
            if (count > ZZAPP_LOADER_WORK_CHUNK_BYTES)
                count = ZZAPP_LOADER_WORK_CHUNK_BYTES;
            dst = (uint8_t *)(uintptr_t)(proc->image_frames[va >> 20] +
                                         in_frame);
            if (g_map_offset < seg->filesz) {
                uint32_t copied = seg->filesz - g_map_offset;

                if (count > copied)
                    count = copied;
                memcpy(dst, g_mem + seg->off + g_map_offset, count);
            } else {
                memset(dst, 0, count);
            }
            g_map_offset += count;
            loader_note_work(count);
            return ZZAPP_LOADER_IN_PROGRESS;
        }
        g_map_page = 0U;
        g_map_phase = LOADER_MAP_IMAGE;
        return ZZAPP_LOADER_IN_PROGRESS;
    }
    if (g_map_phase == LOADER_MAP_IMAGE) {
        uint32_t segs[ZZAPP_MAX_LOAD][3];
        uint32_t image_pages_end = (img->image_end + 0xfffU) & ~0xfffU;
        uint32_t scanned = 0U;
        uint32_t i;

        for (i = 0U; i < img->nseg; i++) {
            segs[i][0] = img->seg[i].vaddr;
            segs[i][1] = img->seg[i].memsz;
            segs[i][2] = (img->seg[i].flags & (PF_X | PF_W)) | PF_R;
        }
        while (g_map_page < image_pages_end &&
               scanned < ZZAPP_LOADER_WORK_CHUNK_BYTES) {
            uint32_t flags = zzapp_loader_page_flags(
                segs, img->nseg, img->image_end, g_map_page);
            uint32_t count = 0U;

            if (flags == 0U) {
                g_map_page += 0x1000U;
                scanned += 0x1000U;
                continue;
            }
            while (g_map_page + count < image_pages_end &&
                   count < ZZAPP_LOADER_WORK_CHUNK_BYTES &&
                   (g_map_page + count) / ZZAPP_MEM_FRAME_BYTES ==
                       g_map_page / ZZAPP_MEM_FRAME_BYTES &&
                   zzapp_loader_page_flags(segs, img->nseg, img->image_end,
                                           g_map_page + count) == flags)
                count += 0x1000U;
            if (zzapp_as_map_image(
                    &proc->as, ZZAPP_AS_WINDOW_BASE + g_map_page,
                    proc->image_frames[g_map_page >> 20] +
                        (g_map_page & (ZZAPP_MEM_FRAME_BYTES - 1u)),
                    count, loader_image_permissions(flags)))
                goto failed;
            Xil_L1DCacheFlushRange(
                (INTPTR)(uintptr_t)(proc->image_frames[g_map_page >> 20] +
                                     (g_map_page &
                                      (ZZAPP_MEM_FRAME_BYTES - 1u))),
                count);
            if (flags & PF_X)
                Xil_ICacheInvalidateRange(
                    (INTPTR)(uintptr_t)(
                        proc->image_frames[g_map_page >> 20] +
                        (g_map_page & (ZZAPP_MEM_FRAME_BYTES - 1u))),
                    count);
            g_map_page += count;
            loader_note_work(count);
        }
        if (g_map_page < image_pages_end)
            return ZZAPP_LOADER_IN_PROGRESS;
        g_map_tail = image_pages_end;
        g_map_phase = LOADER_MAP_TAIL;
        return ZZAPP_LOADER_IN_PROGRESS;
    }
    if (g_map_phase == LOADER_MAP_TAIL) {
        uint32_t map_end = image_frames * ZZAPP_MEM_FRAME_BYTES;
        uint32_t in_frame;
        uint32_t count;

        if (g_map_tail < map_end) {
            in_frame = g_map_tail & (ZZAPP_MEM_FRAME_BYTES - 1u);
            count = map_end - g_map_tail;
            if (count > ZZAPP_MEM_FRAME_BYTES - in_frame)
                count = ZZAPP_MEM_FRAME_BYTES - in_frame;
            if (count > ZZAPP_LOADER_WORK_CHUNK_BYTES)
                count = ZZAPP_LOADER_WORK_CHUNK_BYTES;
            if (zzapp_as_map_image(
                    &proc->as, ZZAPP_AS_WINDOW_BASE + g_map_tail,
                    proc->image_frames[g_map_tail >> 20] + in_frame,
                    count, ZZAPP_AS_IMAGE_RW))
                goto failed;
            g_map_tail += count;
            loader_note_work(count);
            return ZZAPP_LOADER_IN_PROGRESS;
        }
#ifndef ZZAPP_LOADER_INCREMENTAL_HOST_TEST
        __asm__ __volatile__("dsb\n\tmcr p15, 0, r0, c7, c5, 6\n\tdsb\n\tisb"
                             ::: "memory");
#endif
        return ZZ_OK;
    }
    goto failed;

failed:
    zzapp_as_destroy(&proc->as);
    (void)zzapp_mem_release_proc(&g_app_memory, &proc->memory);
    return ZZ_ENOMEM;
}
static uint8_t *image_frame_ptr(zzapp_proc_t *proc, uint32_t offset)
{
    if (offset >= proc->image_frame_count * ZZAPP_MEM_FRAME_BYTES)
        return NULL;
    return (uint8_t *)(uintptr_t)(proc->image_frames[offset >> 20] +
                                  (offset & (ZZAPP_MEM_FRAME_BYTES - 1u)));
}

int zzapp_loader_heap_grow(uint32_t bytes)
{
    zzapp_proc_t *proc = zzapp_proc_slot(loader_slot());
    uint32_t frames = bytes / ZZAPP_MEM_FRAME_BYTES;
    uint32_t first;

    if (!proc || !frames || frames > ZZAPP_MEM_MAX_FRAMES - proc->heap_frame_count)
        return ZZ_ENOMEM;
    first = proc->heap_frame_count;
    if (zzapp_mem_alloc(&g_app_memory, &proc->memory, frames,
                        proc->heap_frames + first))
        return ZZ_ENOMEM;
    if (zzapp_as_map_frames(&proc->as, ZZAPP_AS_WINDOW_BASE + proc->heap_base +
                            first * ZZAPP_MEM_FRAME_BYTES,
                            proc->heap_frames + first, frames)) {
        while (frames--)
            (void)zzapp_mem_release(&g_app_memory, &proc->memory,
                                    proc->heap_frames[first + frames]);
        return ZZ_ENOMEM;
    }
    proc->heap_frame_count += bytes / ZZAPP_MEM_FRAME_BYTES;
    return ZZ_OK;
}

int zzapp_loader_heap_shrink(uint32_t bytes)
{
    zzapp_proc_t *proc = zzapp_proc_slot(loader_slot());
    uint32_t frames = bytes / ZZAPP_MEM_FRAME_BYTES;
    uint32_t first;
    uint32_t i;

    if (!proc || !frames || frames > proc->heap_frame_count)
        return ZZ_EINVAL;
    first = proc->heap_frame_count - frames;
    if (zzapp_as_unmap_frames(&proc->as, ZZAPP_AS_WINDOW_BASE + proc->heap_base +
                              first * ZZAPP_MEM_FRAME_BYTES, frames))
        return ZZ_EINVAL;
    for (i = 0; i < frames; i++)
        (void)zzapp_mem_release(&g_app_memory, &proc->memory,
                                proc->heap_frames[first + i]);
    proc->heap_frame_count = first;
    return ZZ_OK;
}

uint32_t zzapp_loader_pool_free(void)
{
    return g_app_memory_ready ? zzapp_mem_free_bytes(&g_app_memory) : 0u;
}
#else
static uint32_t pool_alloc(uint32_t bytes, uint32_t owner)
{
    (void)owner;
    if (owner == g_owner && g_host_mem)
        return 0;
    if (bytes == ZZAPP_ARENA_BYTES) {
        g_host_mem = (uint8_t *)malloc(bytes);
        return g_host_mem ? HOST_CARD : 0;
    }
    g_host_back = (uint8_t *)malloc(bytes);
    return g_host_back ? (HOST_CARD - 0x01000000u) : 0;
}

static void cache_image(const struct load_image *img)
{
    (void)img;
}
#define CARD_POOL_CLASS_APP 8
#define CARD_POOL_CLASS_APP_DISPLAY 14
void zzapp_loader_memory_init(void) {}
#endif
static void free_relocs(void)
{
    uint8_t *relocs = g_relocs;

    g_relocs = NULL;
    g_relocs_bytes = 0U;
    g_reloc_entry = 0U;
    g_reloc_written = 0U;
    g_reloc_apply = 0U;
    g_reloc_section = 0U;
    g_reloc_prepare_started = 0U;
    free(relocs);
}


static void drop_arena(void)
{
    zzapp_host_revoke_visible();
#ifndef ZZAPP_HOST_TEST
    zzapp_proc_t *proc = zzapp_proc_slot(loader_slot());

    if (proc) {
        zzapp_as_destroy(&proc->as);
        (void)zzapp_mem_release_proc(&g_app_memory, &proc->memory);
    }
    if (g_card)
        card_pool_release_owner(&card_pool, g_owner);
    if (g_back)
        card_pool_free(&card_pool, g_back, g_back_owner);
#else
#ifdef ZZAPP_LOADER_INCREMENTAL_HOST_TEST
    {
        zzapp_proc_t *proc = zzapp_proc_slot(loader_slot());

        if (proc) {
            zzapp_as_destroy(&proc->as);
            (void)zzapp_mem_release_proc(&g_app_memory, &proc->memory);
        }
    }
#endif
    free(g_host_mem);
    free(g_host_back);
    free(g_host_file);
    g_host_mem = NULL;
    g_host_back = NULL;
    g_host_file = NULL;
    g_host_zero_last_bytes = 0U;
    g_host_work_last_bytes = 0U;
#endif
    g_card = 0;
    g_mem = NULL;
    g_bytes = 0;
    g_owner = 0;
    g_generation = 0;
    g_file_end = 0;
    free_relocs();
    g_image_prepared = 0;
    g_zero_frames = 0U;
    g_zeroed_frames = 0U;
    g_zero_offset = 0U;
    g_map_segment = 0U;
    g_map_offset = 0U;
    g_map_page = 0U;
    g_map_tail = 0U;
    g_map_phase = LOADER_MAP_ZERO;
    g_back = 0;
    g_back_owner = 0;
    g_ready = 0;
    zzapp_loader_unbound();
}

int zzapp_loader_set_generation(uint32_t generation)
{
    if (generation == 0u || g_card != 0u)
        return ZZ_EINVAL;
    g_generation = generation;
    return ZZ_OK;
}

int zzapp_loader_begin(void)
{
    uint32_t gen;
    uint32_t owner;
    uint32_t addr;

    if (zzapp_loader_state() != ZZAPP_ST_IDLE)
        return ZZ_EBUSY;
    gen = g_generation ? g_generation : zzapp_loader_generation() + 1u;
    if (gen == 0u)
        gen = 1u;
    owner = ((uint32_t)CARD_POOL_CLASS_APP << 24) |
            (loader_slot() << CARD_POOL_OWNER_SLOT_SHIFT) |
            (gen & CARD_POOL_OWNER_GENERATION_MASK);
#ifdef ZZAPP_HOST_TEST
    addr = pool_alloc(ZZAPP_ARENA_BYTES, owner);
#else
    if (!g_app_memory_ready)
        return ZZ_ENOMEM;
    addr = pool_alloc(ZZAPP_FILE_MAX, owner);
#endif
    if (!addr) {
        zzapp_loader_set_state(ZZAPP_ST_IDLE);
        return ZZ_ENOMEM;
    }
    g_owner = owner;
    g_file_end = 0;
    g_ready = 0;
#ifdef ZZAPP_LOADER_INCREMENTAL_HOST_TEST
    g_host_mapping_calls = 0U;
#endif
#ifdef ZZAPP_HOST_TEST
    g_card = addr;
    g_bytes = ZZAPP_ARENA_BYTES;
    g_mem = g_host_mem;
    zzapp_loader_bound(g_card, g_mem, g_bytes, owner, gen);
#else
    g_card = ZZAPP_AS_WINDOW_BASE;
    g_bytes = ZZAPP_FILE_MAX;
    g_mem = (uint8_t *)(uintptr_t)addr;
    zzapp_loader_bound(g_card, g_mem, ZZAPP_MEM_PRIVATE_WINDOW_BYTES,
                       owner, gen);
#endif
    zzapp_services_on_launch();
    {
        uint8_t *life = (uint8_t *)zzapp_services_window();
        if (life) {
            memset(life + ZZAPP_INPUT_OFF, 0, ZZAPP_INPUT_HDR);
            memset(life + ZZAPP_LOG_OFF, 0, ZZAPP_LOG_HDR);
            memset(life + ZZAPP_FILE_OFF, 0, ZZAPP_FILE_BYTES);
            memset(life + ZZAPP_PALETTE_OFF, 0, ZZAPP_PALETTE_BYTES);
            zzapp_put_be32(life + ZZAPP_LIFE_STOP_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_FAULT_OFF, ZZAPP_FAULT_NONE);
            zzapp_put_be32(life + ZZAPP_LIFE_EXIT_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_FLAGS_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_INPUT_DROPS_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_LOG_DROPS_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_FLIP_GEN_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_FLIP_ACK_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_FLIP_SHOWING_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_FLIP_ADDRESS_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_FLIP_SUBMIT_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_MAILBOX_REPLACED_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_WIDTH_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_HEIGHT_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_PITCH_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_FORMAT_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_CARD_FRONT_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_CARD_BACK_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_LAYOUT_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_PALETTE_GEN_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_PALETTE_FIRST_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_PALETTE_COUNT_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_CARD_SPARE_OFF, 0);
        }
    }
    zzapp_loader_set_state(ZZAPP_ST_LOADING);
    return ZZ_OK;
}

int zzapp_loader_chunk(uint32_t file_off, const void *src, uint32_t len)
{
    if (zzapp_loader_state() != ZZAPP_ST_LOADING || !g_mem)
        return ZZ_EINVAL;
    if (!src || len < ZZAPP_CHUNK_MIN || len > ZZAPP_CHUNK_MAX)
        return ZZ_EINVAL;
    if (!range_ok(file_off, len, ZZAPP_FILE_MAX) ||
        !range_ok(file_off, len, g_bytes))
        return ZZ_EINVAL;
    memcpy(g_mem + file_off, src, len);
    if (file_off + len > g_file_end)
        g_file_end = file_off + len;
    return ZZ_OK;
}

static int note_ok(const uint8_t *note, uint32_t nlen, struct load_image *img)
{
    uint32_t off = 0;

    while (off + 12u <= nlen) {
        uint32_t namesz = rd32(note + off);
        uint32_t descsz = rd32(note + off + 4u);
        uint32_t type = rd32(note + off + 8u);
        uint32_t name_pad = (namesz + 3u) & ~3u;
        uint32_t desc_pad = (descsz + 3u) & ~3u;
        uint32_t desc;

        if (namesz > nlen || off + 12u + name_pad < off + 12u)
            return ZZ_EINVAL;
        desc = off + 12u + name_pad;
        if (!range_ok(desc, descsz, nlen))
            return ZZ_EINVAL;
        if (type == ZZAPP_NOTE_TYPE && namesz >= 6u &&
            memcmp(note + off + 12u, "ZZAPP", 5) == 0 && descsz >= 24u) {
            struct zzapp_note body;

            memcpy(&body, note + desc, sizeof(body));
            if (body.magic != ZZAPP_NOTE_MAGIC || body.abi_major != 0u)
                return ZZ_EINVAL;
            if (body.abi_minor > ZZAPP_API_MINOR)
                return ZZ_EINVAL;
            if (body.stack_bytes == 0u)
                body.stack_bytes = ZZAPP_STACK_DEFAULT;
            if (body.stack_bytes < ZZAPP_STACK_MIN ||
                body.stack_bytes > ZZAPP_STACK_DEFAULT ||
                (body.stack_bytes & 7u) != 0u)
                return ZZ_EINVAL;
            img->stack_bytes = body.stack_bytes;
            img->note_flags = body.flags;
            img->note_minor = body.abi_minor;
            /* ZZAPP_NOTE in the 0.3 SDK marks a dynamic heap with a flag
             * and keeps minor 0, so the app still loads (with the fixed
             * heap) on 0.2 firmware. Treat it as the 0.3 contract here. */
            if (body.flags & ZZAPP_NOTE_FLAG_DYNAMIC_HEAP)
                img->note_minor = 3u;
            if (body.abi_minor >= 3u) {
                if (body.reserved >
                    ZZAPP_MEM_PRIVATE_WINDOW_BYTES / ZZAPP_MEM_FRAME_BYTES)
                    return ZZ_ENOMEM;
                img->min_heap_bytes = body.reserved << 20;
            }
            return ZZ_OK;
        }
        if (desc + desc_pad < desc)
            return ZZ_EINVAL;
        off = desc + desc_pad;
    }
    return ZZ_EINVAL;
}

static int find_note(struct load_image *img, const uint8_t *eh, uint16_t phnum,
                     uint16_t phentsize, uint32_t phoff, uint16_t shnum,
                     uint16_t shentsize, uint32_t shoff)
{
    uint16_t i;

    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = g_mem + phoff + (uint32_t)i * phentsize;
        uint32_t type = rd32(ph);
        uint32_t off = rd32(ph + 4u);
        uint32_t filesz = rd32(ph + 16u);

        if (type != PT_NOTE)
            continue;
        if (!range_ok(off, filesz, g_file_end))
            return ZZ_EINVAL;
        {
            int rc = note_ok(g_mem + off, filesz, img);

            if (rc == ZZ_OK)
                return ZZ_OK;
            if (rc != ZZ_EINVAL)
                return rc;
        }
    }
    if (!shoff || !shnum)
        return ZZ_EINVAL;
    for (i = 0; i < shnum; i++) {
        const uint8_t *sh = g_mem + shoff + (uint32_t)i * shentsize;
        uint32_t type = rd32(sh + 4u);
        uint32_t off = rd32(sh + 16u);
        uint32_t size = rd32(sh + 20u);

        (void)eh;
        if (type != SHT_NOTE)
            continue;
        if (!range_ok(off, size, g_file_end))
            return ZZ_EINVAL;
        {
            int rc = note_ok(g_mem + off, size, img);

            if (rc == ZZ_OK)
                return ZZ_OK;
            if (rc != ZZ_EINVAL)
                return rc;
        }
    }
    return ZZ_EINVAL;
}

static int dynamic_clean(uint32_t off, uint32_t filesz)
{
    uint32_t i;

    if (!range_ok(off, filesz, g_file_end) || (filesz & 7u) != 0u)
        return ZZ_EINVAL;
    for (i = 0; i + 8u <= filesz; i += 8u) {
        int32_t tag = (int32_t)rd32(g_mem + off + i);

        if (tag == DT_NULL)
            return ZZ_OK;
        if (tag == DT_NEEDED)
            return ZZ_EINVAL;
    }
    return ZZ_OK;
}



static int parse_elf(struct load_image *img)
{
    const uint8_t *eh;
    uint16_t phnum;
    uint16_t phentsize;
    uint16_t shnum;
    uint16_t shentsize;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t filesz_sum = 0;
    uint32_t memsz_sum = 0;
    uint32_t image_limit;
    uint16_t i;
    int rc;

    memset(img, 0, sizeof(*img));
    if (g_file_end < 52u)
        return ZZ_EINVAL;
    eh = g_mem;
    if (eh[0] != 0x7fu || eh[1] != 'E' || eh[2] != 'L' || eh[3] != 'F')
        return ZZ_EINVAL;
    if (eh[4] != 1u || eh[5] != 1u || eh[6] != 1u)
        return ZZ_EINVAL;
    if (rd16(eh + 16u) != ET_DYN || rd16(eh + 18u) != EM_ARM)
        return ZZ_EINVAL;
    if ((rd32(eh + 36u) & EF_ARM_ABI_FLOAT_HARD) == 0u)
        return ZZ_EINVAL;
    phoff = rd32(eh + 28u);
    shoff = rd32(eh + 32u);
    phentsize = rd16(eh + 42u);
    phnum = rd16(eh + 44u);
    shentsize = rd16(eh + 46u);
    shnum = rd16(eh + 48u);
    if (phentsize < 32u || phnum == 0u || phnum > ZZAPP_MAX_PHDR)
        return ZZ_EINVAL;
    if (shentsize < 40u || shnum > ZZAPP_MAX_SHDR)
        return ZZ_EINVAL;
    if (!range_ok(phoff, (uint32_t)phnum * phentsize, g_file_end))
        return ZZ_EINVAL;
    if (shnum && !range_ok(shoff, (uint32_t)shnum * shentsize, g_file_end))
        return ZZ_EINVAL;

    rc = find_note(img, eh, phnum, phentsize, phoff, shnum, shentsize, shoff);
    if (rc != ZZ_OK)
        return rc;

    image_limit = img->note_minor < 3u ? ZZAPP_ARENA_BYTES :
                  ZZAPP_MEM_PRIVATE_WINDOW_BYTES;
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = g_mem + phoff + (uint32_t)i * phentsize;
        uint32_t type = rd32(ph);
        uint32_t off = rd32(ph + 4u);
        uint32_t vaddr = rd32(ph + 8u);
        uint32_t filesz = rd32(ph + 16u);
        uint32_t memsz = rd32(ph + 20u);
        uint32_t flags = rd32(ph + 24u);
        struct load_seg *seg;

        if (type == PT_INTERP)
            return ZZ_EINVAL;
        if (type == PT_DYNAMIC) {
            rc = dynamic_clean(off, filesz);
            if (rc != ZZ_OK)
                return rc;
        }
        if (type != PT_LOAD)
            continue;
        if (img->nseg >= ZZAPP_MAX_LOAD)
            return ZZ_EINVAL;
        if ((flags & PF_W) && (flags & PF_X))
            return ZZ_EINVAL;
        if (memsz < filesz)
            return ZZ_EINVAL;
        if (!range_ok(off, filesz, g_file_end))
            return ZZ_EINVAL;
        if (!range_ok(vaddr, memsz, image_limit))
            return ZZ_EINVAL;
        if (filesz_sum + filesz < filesz_sum)
            return ZZ_EINVAL;
        filesz_sum += filesz;
        memsz_sum += memsz;
        seg = &img->seg[img->nseg++];
        seg->off = off;
        seg->vaddr = vaddr;
        seg->filesz = filesz;
        seg->memsz = memsz;
        seg->flags = flags;
        if (vaddr + memsz > img->image_end)
            img->image_end = vaddr + memsz;
    }
    if (img->nseg == 0u || filesz_sum > ZZAPP_FILE_MAX)
        return ZZ_EINVAL;
    if (memsz_sum + img->stack_bytes + ZZAPP_IMAGE_SLACK >
        ZZAPP_MEM_PRIVATE_WINDOW_BYTES)
        return ZZ_EINVAL;
    /* The heap starts here, so VFP/NEdouble data in app allocations
     * (GLQuake's hunk typed loads, newlib's double-heavy code) keeps its
     * natural alignment whatever an image's .bss tail happens to be.  An
     * 8-byte round let a rebuilt image with a larger .data shift the heap
     * base and abort the app on a misaligned float load. */
    img->image_end = (img->image_end + 31u) & ~31u;
    if (img->image_end + img->stack_bytes > ZZAPP_MEM_PRIVATE_WINDOW_BYTES)
        return ZZ_EINVAL;
    for (i = 0; i < img->nseg; i++) {
        uint32_t a = img->seg[i].vaddr;
        uint32_t ae = a + img->seg[i].memsz;
        uint16_t j;

        if (ae > ZZAPP_MEM_PRIVATE_WINDOW_BYTES - img->stack_bytes)
            return ZZ_EINVAL;
        for (j = (uint16_t)(i + 1u); j < img->nseg; j++) {
            uint32_t b = img->seg[j].vaddr;
            uint32_t be = b + img->seg[j].memsz;

            if (a < be && b < ae)
                return ZZ_EINVAL;
        }
    }
    img->entry = rd32(eh + 24u);
    if (img->entry >= ZZAPP_MEM_PRIVATE_WINDOW_BYTES - img->stack_bytes)
        return ZZ_EINVAL;
    return ZZ_OK;
}

#if defined(ZZAPP_HOST_TEST) && !defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
static int overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return an && bn && a < b + bn && b < a + an;
}

/* Move each PT_LOAD from its file offset to p_vaddr. A segment is copied
 * only once that write cannot clobber another segment's still-unread file
 * bytes. Equal offset and vaddr is a no-op. A cycle returns ZZ_ENOMEM
 * rather than a second card-pool block: the 96 MiB class cap is already
 * held by the arena. */
static int place_segments(const struct load_image *img)
{
    const uint8_t *file = g_host_file ? g_host_file : g_mem;
    uint8_t done[ZZAPP_MAX_LOAD];
    uint32_t left = img->nseg;
    uint32_t guard = 0;
    uint32_t i;

    memset(done, 0, sizeof(done));
    while (left && guard++ < ZZAPP_MAX_LOAD) {
        int moved = 0;

        for (i = 0; i < img->nseg; i++) {
            uint32_t j;
            int blocked = 0;

            if (done[i])
                continue;
            for (j = 0; j < img->nseg; j++) {
                if (j == i || done[j])
                    continue;
                if (!g_host_file &&
                    overlap(img->seg[i].vaddr, img->seg[i].filesz,
                            img->seg[j].off, img->seg[j].filesz))
                    blocked = 1;
            }
            if (blocked)
                continue;
            if (img->seg[i].filesz)
                memmove(g_mem + img->seg[i].vaddr,
                        file + img->seg[i].off, img->seg[i].filesz);
            done[i] = 1;
            left--;
            moved = 1;
        }
        if (!moved)
            return ZZ_ENOMEM;
    }
    if (left)
        return ZZ_ENOMEM;
    /* Zero only after every source byte has moved: a BSS tail can overlap a
     * not-yet-copied file image even when PT_LOAD destinations do not overlap. */
    for (i = 0; i < img->nseg; i++) {
        if (img->seg[i].memsz > img->seg[i].filesz)
            memset(g_mem + img->seg[i].vaddr + img->seg[i].filesz, 0,
                   img->seg[i].memsz - img->seg[i].filesz);
    }
    return ZZ_OK;
}
#endif

static int prepare_loaded_image(void)
{
    uint16_t shnum;
    uint16_t shentsize;
    uint32_t shoff;
    uint32_t copied = 0U;

    if (!g_reloc_prepare_started) {
        uint32_t saved_n = 0U;
        uint16_t i;
        int rc = parse_elf(&g_image);

        if (rc != ZZ_OK)
            return rc;
        if (!image_commit_fits_limit(&g_image))
            return ZZ_ENOMEM;
        shoff = rd32(g_mem + 32u);
        shentsize = rd16(g_mem + 46u);
        shnum = rd16(g_mem + 48u);
        for (i = 0U; i < shnum; i++) {
            const uint8_t *sh = g_mem + shoff + (uint32_t)i * shentsize;
            uint32_t type = rd32(sh + 4u);
            uint32_t flags = rd32(sh + 8u);
            uint32_t addr = rd32(sh + 12u);
            uint32_t off = rd32(sh + 16u);
            uint32_t size = rd32(sh + 20u);
            uint32_t entsize = rd32(sh + 36u);
            uint32_t entries;

            if (type != SHT_RELA && type != SHT_REL)
                continue;
            if (((flags & SHF_ALLOC) && size &&
                 !range_ok(addr, size, g_image.note_minor < 3u ?
                           ZZAPP_ARENA_BYTES :
                           ZZAPP_MEM_PRIVATE_WINDOW_BYTES)) ||
                (type == SHT_RELA &&
                 (entsize != 12u || (size % 12u) != 0u)) ||
                (type == SHT_REL &&
                 (entsize != 8u || (size % 8u) != 0u)) ||
                !range_ok(off, size, g_file_end))
                return ZZ_EINVAL;
            entries = size / (type == SHT_RELA ? 12u : 8u);
            if (entries > (UINT32_MAX - saved_n) / 16u)
                return ZZ_ENOMEM;
            saved_n += entries * 16u;
        }
        g_relocs = saved_n ? (uint8_t *)malloc(saved_n) : NULL;
        if (saved_n && !g_relocs)
            return ZZ_ENOMEM;
        g_relocs_bytes = saved_n;
        g_reloc_prepare_started = 1U;
    }

    shoff = rd32(g_mem + 32u);
    shentsize = rd16(g_mem + 46u);
    shnum = rd16(g_mem + 48u);
    while (g_reloc_section < shnum &&
           copied < ZZAPP_LOADER_RELOC_CHUNK_ENTRIES) {
        const uint8_t *sh = g_mem + shoff +
            (uint32_t)g_reloc_section * shentsize;
        uint32_t type = rd32(sh + 4u);
        uint32_t size = rd32(sh + 20u);
        uint32_t stride;
        uint32_t entries;

        if (type != SHT_RELA && type != SHT_REL) {
            g_reloc_section++;
            g_reloc_entry = 0U;
            continue;
        }
        stride = type == SHT_RELA ? 12u : 8u;
        entries = size / stride;
        while (g_reloc_entry < entries &&
               copied < ZZAPP_LOADER_RELOC_CHUNK_ENTRIES) {
            const uint8_t *rel = g_mem + rd32(sh + 16u) +
                g_reloc_entry * stride;
            uint32_t roff = rd32(rel);
            uint32_t info = rd32(rel + 4u);

            if ((info & 0xffu) != R_ARM_RELATIVE || (info >> 8) != 0u ||
                (roff & 3u) != 0u ||
                !range_ok(roff, 4u, g_image.note_minor < 3u ?
                          ZZAPP_ARENA_BYTES :
                          ZZAPP_MEM_PRIVATE_WINDOW_BYTES))
                return ZZ_EINVAL;
            memcpy(g_relocs + g_reloc_written, rel, 8u);
            memcpy(g_relocs + g_reloc_written + 8u, &type, 4u);
            if (type == SHT_RELA)
                memcpy(g_relocs + g_reloc_written + 12u, rel + 8u, 4u);
            else
                memset(g_relocs + g_reloc_written + 12u, 0, 4u);
            g_reloc_written += 16u;
            g_reloc_entry++;
            copied++;
        }
        if (g_reloc_entry == entries) {
            g_reloc_section++;
            g_reloc_entry = 0U;
        }
    }
    loader_note_work(copied * 16U);
    if (g_reloc_section != shnum)
        return ZZAPP_LOADER_IN_PROGRESS;
    g_image_prepared = 1;
    return ZZ_OK;
}

static int apply_loaded_relocations(void)
{
    uint32_t applied = 0U;

    while (g_reloc_apply < g_relocs_bytes &&
           applied < ZZAPP_LOADER_RELOC_CHUNK_ENTRIES) {
        uint32_t at = g_reloc_apply;
        uint32_t roff = rd32(g_relocs + at);
        uint32_t type = rd32(g_relocs + at + 8u);
#if defined(ZZAPP_HOST_TEST) && !defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
        uint32_t addend = type == SHT_REL ? rd32(g_mem + roff) :
                                             rd32(g_relocs + at + 12u);
        uint32_t place = addend + g_card;

        memcpy(g_mem + roff, &place, sizeof(place));
#else
        zzapp_proc_t *proc = zzapp_proc_slot(loader_slot());
        uint8_t *target = proc ? image_frame_ptr(proc, roff) : NULL;
        uint32_t addend;
        uint32_t place;

        if (!target)
            return ZZ_EINVAL;
        addend = type == SHT_REL ? rd32(target) : rd32(g_relocs + at + 12u);
        place = addend + g_card;
        memcpy(target, &place, sizeof(place));
        Xil_L1DCacheFlushRange((INTPTR)(uintptr_t)target, sizeof(place));
#endif
        g_reloc_apply += 16u;
        applied++;
    }
    loader_note_work(applied * 16U);
    return g_reloc_apply == g_relocs_bytes ? ZZ_OK :
        ZZAPP_LOADER_IN_PROGRESS;
}

int zzapp_loader_end(void)
{
    int rc;

    if (zzapp_loader_state() != ZZAPP_ST_LOADING || !g_mem)
        return ZZ_EINVAL;
    if (!g_image_prepared) {
        rc = prepare_loaded_image();
        if (rc == ZZAPP_LOADER_IN_PROGRESS)
            return rc;
        if (rc != ZZ_OK) {
            drop_arena();
            zzapp_loader_set_state(ZZAPP_ST_IDLE);
            return rc;
        }
    }
#ifdef ZZAPP_HOST_TEST
#ifdef ZZAPP_LOADER_INCREMENTAL_HOST_TEST
    rc = map_loaded_image(&g_image);
#else
    rc = place_segments(&g_image);
#endif
#else
    rc = map_loaded_image(&g_image);
#endif
    if (rc == ZZAPP_LOADER_IN_PROGRESS)
        return rc;
    if (rc != ZZ_OK)
        goto failed;
    rc = apply_loaded_relocations();
    if (rc == ZZAPP_LOADER_IN_PROGRESS)
        return rc;
    if (rc != ZZ_OK)
        goto failed;
    free_relocs();
#if defined(ZZAPP_HOST_TEST) && !defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
    cache_image(&g_image);
#endif
    rc = zzapp_loader_set_image(g_image.entry, g_image.image_end,
                                g_image.stack_bytes, g_image.note_flags,
                                g_image.note_minor, g_image.min_heap_bytes);
    if (rc != ZZ_OK)
        goto failed;
    g_ready = 1;
    return ZZ_OK;

failed:
    drop_arena();
    zzapp_loader_set_state(ZZAPP_ST_IDLE);
    return rc;
}

int zzapp_loader_abort(void)
{
    uint32_t state = zzapp_loader_state();

    if (state != ZZAPP_ST_LOADING && state != ZZAPP_ST_STARTING)
        return ZZ_EBUSY;
    drop_arena();
    zzapp_services_set_state(ZZAPP_ST_IDLE);
    return ZZ_OK;
}

int zzapp_loader_alloc_back(uint32_t bytes, uint32_t *addr_out)
{
    uint32_t addr;

    if (!addr_out || !bytes)
        return ZZ_EINVAL;
    if (g_back)
        return ZZ_EBUSY;
    if (!g_owner)
        return ZZ_EINVAL;
    /* A firmware-owned display allocation cannot consume the app arena's
     * class budget, which is already fully reserved by the live arena. */
    g_back_owner = ((uint32_t)CARD_POOL_CLASS_APP_DISPLAY << 24) |
                   (g_owner & 0x00FFFFFFu);
    addr = pool_alloc(bytes, g_back_owner);
    if (!addr) {
        g_back_owner = 0;
        return ZZ_ENOMEM;
    }
    g_back = addr;
    *addr_out = addr;
    return ZZ_OK;
}

void zzapp_loader_free_back(uint32_t addr)
{
    if (!addr || addr != g_back)
        return;
#ifndef ZZAPP_HOST_TEST
    card_pool_free(&card_pool, g_back, g_back_owner);
#else
    free(g_host_back);
    g_host_back = NULL;
#endif
    g_back = 0;
}

uint32_t zzapp_loader_card(void)
{
    return g_card;
}

uint8_t *zzapp_loader_mem(void)
{
    return g_mem;
}

int zzapp_loader_ready(void)
{
    return g_ready;
}

void zzapp_loader_release(void)
{
    drop_arena();
}

#ifdef ZZAPP_HOST_TEST
uint32_t zzapp_test_arena_card(void)
{
    return g_card;
}

uint8_t *zzapp_test_arena_mem(void)
{
    return g_mem;
}
uint32_t zzapp_test_loader_zero_last_bytes(void)
{
    return g_host_zero_last_bytes;
}
uint32_t zzapp_test_loader_work_last_bytes(void)
{
    return g_host_work_last_bytes;
}
#ifdef ZZAPP_LOADER_INCREMENTAL_HOST_TEST
uint32_t zzapp_test_loader_mapping_calls(void)
{
    return g_host_mapping_calls;
}
#else
uint32_t zzapp_test_loader_mapping_calls(void)
{
    return 0U;
}
#endif
#endif
