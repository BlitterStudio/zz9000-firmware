/*
 * PIE ELF loader for the trusted ARM apps, one loader state per process
 * record.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Relocations come from PT_DYNAMIC, so an image without section headers
 * loads: DT_REL/DT_RELSZ/DT_RELENT (8-byte entries, the addend is the word at
 * the target) and DT_RELA/DT_RELASZ/DT_RELAENT (12-byte entries, the addend is
 * in the entry), R_ARM_RELATIVE only. The relocated word is addend +
 * load_bias. DT_NEEDED, a PLT relocation table and any other relocation type
 * are refused.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "aar/app.h"
#include "zzapp_proto.h"
#include "zzapp_state.h"
#include "zzapp_runtime.h"
#include "zzapp_rec.h"
#include "zzapp_xport.h"

#define EM_ARM                 40u
#define ET_DYN                 3u
#define PT_LOAD                1u
#define PT_INTERP              3u
#define PT_NOTE                4u
#define PT_DYNAMIC             2u
#define PF_X                   1u
#define PF_W                   2u
#define PF_R                   4u
#define R_ARM_RELATIVE         23u
#define EF_ARM_EABIMASK        0xff000000u
#define EF_ARM_EABI_VER5       0x05000000u
#define EF_ARM_ABI_FLOAT_SOFT  0x200u
#define EF_ARM_ABI_FLOAT_HARD  0x400u
#define DT_NULL                0
#define DT_NEEDED              1
#define DT_PLTRELSZ            2
#define DT_RELA                7
#define DT_RELASZ              8
#define DT_RELAENT             9
#define DT_JMPREL              23
#define DT_REL                 17
#define DT_RELSZ               18
#define DT_RELENT              19
#define DT_RELRSZ              35
#define DT_RELR                36
#define DT_RELRENT             37
#define LOADER_REL_BYTES       8u
#define LOADER_RELA_BYTES      12u
/* Kind of a saved relocation, stored with it. */
#define LOADER_RELOC_REL       1u
#define LOADER_RELOC_RELA      2u
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
    uint32_t min_heap_bytes;
    uint32_t image_end;
    uint32_t nseg;
    /* Relocation tables found in PT_DYNAMIC, as file offsets (0 bytes: none). */
    uint32_t rel_off;
    uint32_t rel_bytes;
    uint32_t rela_off;
    uint32_t rela_bytes;
    struct load_seg seg[ZZAPP_MAX_LOAD];
};

static uint32_t image_proc_limit(void)
{
    uint32_t limit = zzapp_services_memory_limit();

    return limit != 0u && limit < ZZAPP_MEM_PRIVATE_WINDOW_BYTES ?
           limit : ZZAPP_MEM_PRIVATE_WINDOW_BYTES;
}

static int image_commit_fits_limit(const struct load_image *img)
{
    uint32_t image = (img->image_end + ZZAPP_MEM_FRAME_BYTES - 1u) &
                     ~(ZZAPP_MEM_FRAME_BYTES - 1u);
    uint32_t stack = (img->stack_bytes + ZZAPP_MEM_FRAME_BYTES - 1u) &
                     ~(ZZAPP_MEM_FRAME_BYTES - 1u);

    return image + stack + img->min_heap_bytes <= image_proc_limit();
}

/* Defined in zzapp_host.c. The loader does not own the lifecycle words. */
void zzapp_loader_bound(uint32_t card, uint8_t *mem, uint32_t owner,
                        uint32_t generation);
void zzapp_loader_unbound(void);
void zzapp_loader_unbound_resources(void);
uint32_t zzapp_loader_generation(void);
uint32_t zzapp_loader_state(void);
void zzapp_loader_set_state(uint32_t state);
int zzapp_loader_set_image(uint32_t entry, uint32_t image_end,
                           uint32_t stack_bytes, uint32_t note_flags,
                           uint32_t min_heap_bytes);
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
    uint32_t map_flush_end;
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
};

/* The loader's state is the record's loader part: one per process record,
 * type stable. A bad index stops the caller (never "slot 0"). */
_Static_assert(sizeof(struct loader_slot) <= ZZAPP_REC_LOADER_BYTES,
               "loader state exceeds its block of the process record");
_Static_assert(_Alignof(struct loader_slot) <= 8u,
               "loader state alignment");

static void loader_hard_fail(void)
{
#ifdef ZZAPP_HOST_TEST
    abort();
#else
    __builtin_trap();
#endif
}

static uint32_t loader_pid(void)
{
    return zzapp_services_current_pid();
}

static zzapp_rec_t *loader_rec_or_null(void)
{
    return zzapp_rec_by_pid(loader_pid());
}

static struct loader_slot *loader_part(void)
{
    zzapp_rec_t *rec = loader_rec_or_null();

    if (!rec)
        loader_hard_fail();
    return (struct loader_slot *)(void *)rec->loader.bytes;
}

#define g_card       (loader_part()->card)
#define g_mem        (loader_part()->mem)
#define g_bytes      (loader_part()->bytes)
#define g_owner      (loader_part()->owner)
#define g_file_end   (loader_part()->file_end)
#define g_back       (loader_part()->back)
#define g_zero_frames (loader_part()->zero_frames)
#define g_zeroed_frames (loader_part()->zeroed_frames)
#define g_image      (loader_part()->image)
#define g_relocs     (loader_part()->relocs)
#define g_relocs_bytes (loader_part()->relocs_bytes)
#define g_reloc_entry (loader_part()->reloc_entry)
#define g_reloc_written (loader_part()->reloc_written)
#define g_reloc_apply (loader_part()->reloc_apply)
#define g_reloc_section (loader_part()->reloc_section)
#define g_reloc_prepare_started (loader_part()->reloc_prepare_started)
#define g_image_prepared (loader_part()->image_prepared)
#define g_back_owner (loader_part()->back_owner)
#define g_generation (loader_part()->generation)
#define g_ready      (loader_part()->ready)
#define g_zero_offset (loader_part()->zero_offset)
#define g_map_segment (loader_part()->map_segment)
#define g_map_offset (loader_part()->map_offset)
#define g_map_page (loader_part()->map_page)
#define g_map_flush_end (loader_part()->map_flush_end)
#define g_map_tail (loader_part()->map_tail)
#define g_map_phase (loader_part()->map_phase)
#ifdef ZZAPP_HOST_TEST
/* Host harness only: the flat window that stands in for the app's private
 * memory, and the counters tests read. They are kept per record index, apart
 * from the record, which holds the production fields only. */
struct loader_host {
    uint8_t *host_mem;
    uint8_t *host_back;
    uint8_t *host_file;
    uint32_t host_zero_last_bytes;
    uint32_t host_work_last_bytes;
    uint32_t host_reloc_last_entries;
    uint32_t host_work_last_kind;
};
static struct loader_host g_loader_host[ZZAPP_REC_MAX_APPS];

static struct loader_host *loader_host_part(void)
{
    uint32_t pid = loader_pid();

    if (pid >= ZZAPP_REC_MAX_APPS || !zzapp_rec_by_pid(pid))
        loader_hard_fail();
    return &g_loader_host[pid];
}
#define g_host_mem   (loader_host_part()->host_mem)
#define g_host_back  (loader_host_part()->host_back)
#define HOST_CARD 0xc0000000u
#define g_host_file  (loader_host_part()->host_file)
#define g_host_zero_last_bytes (loader_host_part()->host_zero_last_bytes)
#define g_host_work_last_bytes (loader_host_part()->host_work_last_bytes)
#define g_host_reloc_last_entries \
    (loader_host_part()->host_reloc_last_entries)
#define g_host_work_last_kind (loader_host_part()->host_work_last_kind)
#endif

#ifdef ZZAPP_HOST_TEST
/* A new boot of the harness: forget every window left from earlier cases. */
static void loader_host_reset(void)
{
    uint32_t i;

    for (i = 0U; i < ZZAPP_REC_MAX_APPS; i++) {
        free(g_loader_host[i].host_mem);
        free(g_loader_host[i].host_back);
        free(g_loader_host[i].host_file);
    }
    memset(g_loader_host, 0, sizeof(g_loader_host));
}
#endif

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

/* Loader service spans use XTime global-timer ticks. COUNTS_PER_SECOND is the
 * CPU clock divided by two, so the 666 MHz CPU's timer runs at about 333 MHz.
 * Loading swquake on the card, the longest pass of each kind was: zero
 * 138667 ticks (0.42 ms), copy 59571 (0.18 ms), map 159258 (0.48 ms),
 * relocation extraction 295814 (0.89 ms), relocation application 110622
 * (0.33 ms). Zero is limited to
 * 32 KiB, map to 96 KiB, and copy to 48 KiB per pass. A pass never splits
 * inside a 4 KiB page: every byte bound is a multiple of the page size, so
 * frame-boundary splitting and image mapping still line up below the 1 MiB
 * frame size. */
#ifndef ZZAPP_LOADER_ZERO_CHUNK_BYTES
#define ZZAPP_LOADER_ZERO_CHUNK_BYTES (32u * 1024u)
#endif
#ifndef ZZAPP_LOADER_COPY_CHUNK_BYTES
#define ZZAPP_LOADER_COPY_CHUNK_BYTES (48u * 1024u)
#endif
#ifndef ZZAPP_LOADER_MAP_CHUNK_BYTES
#define ZZAPP_LOADER_MAP_CHUNK_BYTES (96u * 1024u)
#endif
#define ZZAPP_LOADER_RELOC_CHUNK_ENTRIES 512U
#define ZZAPP_LOADER_RELOC_APPLY_CHUNK_ENTRIES 256U
/* Cortex-A9 L1 line: relocations within this distance share a flush. */
#define ZZAPP_LOADER_CACHE_LINE_BYTES 32u

enum loader_map_phase {
    LOADER_MAP_ZERO = 0,
    LOADER_MAP_SETUP,
    LOADER_MAP_COPY,
    LOADER_MAP_IMAGE,
    LOADER_MAP_TAIL
};

/* Loader pass kinds, recorded for the core-1 launcher perf attribution. */
#define ZZAPP_LOADER_WORK_ZERO   1u
#define ZZAPP_LOADER_WORK_COPY   2u
#define ZZAPP_LOADER_WORK_MAP    3u
#define ZZAPP_LOADER_WORK_RELOC  4u
#define ZZAPP_LOADER_WORK_APPLY  5u
#define ZZAPP_LOADER_WORK_ALLOC  6u

#if ZZAPP_PERF_COUNTERS || defined(ZZAPP_HOST_TEST)
static uint32_t worker_work_bytes;
static uint32_t worker_work_kind;

void zzapp_loader_work_begin_pass(void)
{
    worker_work_bytes = 0U;
    worker_work_kind = 0U;
}

uint32_t zzapp_loader_work_last_bytes(void)
{
    return worker_work_bytes;
}

uint32_t zzapp_loader_work_last_kind(void)
{
    return worker_work_kind;
}

static void loader_note_work(uint32_t bytes, uint32_t kind)
{
    worker_work_bytes = bytes;
    worker_work_kind = kind;
#ifdef ZZAPP_HOST_TEST
    g_host_work_last_bytes = bytes;
    g_host_work_last_kind = kind;
#endif
}
#else
void zzapp_loader_work_begin_pass(void)
{
}

uint32_t zzapp_loader_work_last_bytes(void)
{
    return 0U;
}

uint32_t zzapp_loader_work_last_kind(void)
{
    return 0U;
}

#define loader_note_work(bytes, kind) \
    do { (void)(bytes); (void)(kind); } while (0)
#endif

static void loader_note_relocs(uint32_t entries)
{
#ifdef ZZAPP_HOST_TEST
    g_host_reloc_last_entries = entries;
#else
    (void)entries;
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
/* Host build: L1 flush calls are recorded so tests can prove every
 * relocated word is flushed in the pass that writes it. */
#define ZZAPP_TEST_FLUSH_MAX 4096U
static struct {
    uintptr_t addr;
    uint32_t bytes;
} g_host_flush[ZZAPP_TEST_FLUSH_MAX];
static uint32_t g_host_flush_count;
static uint32_t g_host_flush_dropped;

static void host_note_flush(uintptr_t address, uint32_t bytes)
{
    if (g_host_flush_count < ZZAPP_TEST_FLUSH_MAX) {
        g_host_flush[g_host_flush_count].addr = address;
        g_host_flush[g_host_flush_count].bytes = bytes;
        g_host_flush_count++;
    } else {
        g_host_flush_dropped++;
    }
}

#define Xil_L1DCacheFlushRange(address, bytes) \
    host_note_flush((uintptr_t)(address), (uint32_t)(bytes))
#define Xil_ICacheInvalidateRange(address, bytes) ((void)(address), (void)(bytes))
#endif

static zzapp_mem_t g_app_memory;
static int g_app_memory_ready;

void zzapp_loader_memory_init(void)
{
#ifdef ZZAPP_HOST_TEST
    loader_host_reset();
#endif
    if (!g_app_memory_ready &&
        zzapp_mem_init(&g_app_memory, &card_pool) == 0) {
        zzapp_host_register_low_memory_callback(&g_app_memory);
        g_app_memory_ready = 1;
    }
}

/* Card pool blocks of this record's owner. The host harness backs the pool
 * with real memory, so staging in the incremental build is a real 16 MiB
 * class APP block, as on the card. */
static uint32_t pool_alloc(uint32_t bytes, uint32_t owner)
{
    uint32_t addr;
#ifndef ZZAPP_HOST_TEST
    uint32_t end;
#endif

    addr = card_pool_alloc(&card_pool, bytes, owner);
    if (!addr)
        return 0;
#ifndef ZZAPP_HOST_TEST
    end = addr + bytes;
    if (end < addr || addr < CARD_POOL_PERMANENT_ADDRESS ||
        end > CARD_POOL_PERMANENT_END) {
        card_pool_free(&card_pool, addr, owner);
        return 0;
    }
#endif
    return addr;
}
#if defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST) && \
    !defined(ZZAPP_LOADER_REAL_AS_HOST_TEST)
static uint32_t g_host_mapping_calls;
static int host_as_create(zzapp_as_t *as, uint32_t pid)
{
    (void)as;
    (void)pid;
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

static int loader_section_has_flags(const uint32_t (*segs)[3], uint32_t nseg,
				    uint32_t image_end, uint32_t page,
				    uint32_t flags)
{
    uint32_t offset;

    for (offset = 0U; offset < ZZAPP_MEM_FRAME_BYTES; offset += 0x1000U)
        if (zzapp_loader_page_flags(segs, nseg, image_end, page + offset) !=
            flags)
            return 0;
    return 1;
}

static int map_loaded_image(const struct load_image *img)
{
    zzapp_proc_t *proc = zzapp_proc_slot(loader_pid());
    uint32_t image_frames;
    uint32_t stack_frames;
    uint32_t heap_frames;
    uint32_t total_frames;
    uint32_t proc_limit;
    uint32_t refusal = 0U;

#if ZZAPP_PERF_COUNTERS || defined(ZZAPP_HOST_TEST)
    zzapp_loader_work_begin_pass();
#endif
#ifdef ZZAPP_HOST_TEST
    g_host_work_last_bytes = 0U;
    g_host_work_last_kind = 0U;
#endif
    if (!g_app_memory_ready || !proc ||
        img->image_end > ZZAPP_AS_ALIAS_BASE - ZZAPP_AS_WINDOW_BASE)
        return AAR_E_NOMEM;
    image_frames = (img->image_end + ZZAPP_MEM_FRAME_BYTES - 1u) >> 20;
    stack_frames = (img->stack_bytes + ZZAPP_MEM_FRAME_BYTES - 1u) >> 20;
    total_frames = image_frames + stack_frames +
                   img->min_heap_bytes / ZZAPP_MEM_FRAME_BYTES;
    if (image_frames > ZZAPP_MEM_MAX_FRAMES ||
        total_frames > ZZAPP_MEM_MAX_FRAMES)
        return AAR_E_NOMEM;
    heap_frames = total_frames - image_frames - stack_frames;
    proc_limit = image_proc_limit();
    if (g_zero_frames == 0U) {
        zzapp_mem_proc_init(&proc->memory, proc_limit);
        if (zzapp_mem_alloc_unzeroed(&g_app_memory, &proc->memory,
                                     image_frames, proc->image_frames) ||
            (heap_frames &&
             zzapp_mem_alloc_unzeroed(&g_app_memory, &proc->memory,
                                      heap_frames, proc->heap_frames)) ||
            zzapp_mem_alloc_unzeroed(&g_app_memory, &proc->memory,
                                     stack_frames, proc->stack_frames))
        {
            refusal = AAR_REASON_MEMORY;
            goto failed;
        }
        proc->image_frame_count = image_frames;
        proc->heap_frame_count = heap_frames;
        proc->stack_frame_count = stack_frames;
        zzapp_xport_publish_private(loader_rec_or_null());
        g_zero_frames = total_frames;
        g_zeroed_frames = 0U;
        g_zero_offset = 0U;
        g_map_phase = LOADER_MAP_ZERO;
        /* Frame allocation can scale with the image commit. Keep it out of
         * the first bounded zero slice and give it its own service pass. */
        loader_note_work(total_frames * ZZAPP_MEM_FRAME_BYTES,
                         ZZAPP_LOADER_WORK_ALLOC);
        return ZZAPP_LOADER_IN_PROGRESS;
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
        if (count > ZZAPP_LOADER_ZERO_CHUNK_BYTES)
            count = ZZAPP_LOADER_ZERO_CHUNK_BYTES;
        dst = (uint8_t *)(uintptr_t)proc->memory.frames[g_zeroed_frames] +
              g_zero_offset;
        memset(dst, 0, count);
        loader_note_work(count, ZZAPP_LOADER_WORK_ZERO);
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
        if (zzapp_as_create(&proc->as, loader_pid()))
            goto failed;
        proc->heap_base = image_frames * ZZAPP_MEM_FRAME_BYTES;
        proc->stack_base = ZZAPP_MEM_PRIVATE_WINDOW_BYTES - stack_frames *
                           ZZAPP_MEM_FRAME_BYTES;
        if (proc->stack_base < ZZAPP_MEM_FRAME_BYTES)
            goto failed;
        proc->heap_limit = ZZAPP_AS_ALIAS_BASE - ZZAPP_AS_WINDOW_BASE;
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
            if (count > ZZAPP_LOADER_COPY_CHUNK_BYTES)
                count = ZZAPP_LOADER_COPY_CHUNK_BYTES;
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
            loader_note_work(count, ZZAPP_LOADER_WORK_COPY);
            return ZZAPP_LOADER_IN_PROGRESS;
        }
        g_map_page = 0U;
        g_map_flush_end = 0U;
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
        if (g_map_page < g_map_flush_end) {
            uint32_t flags = zzapp_loader_page_flags(
                segs, img->nseg, img->image_end, g_map_page);
            uint32_t count = g_map_flush_end - g_map_page;
            uint8_t *address;

            if (count > ZZAPP_LOADER_MAP_CHUNK_BYTES)
                count = ZZAPP_LOADER_MAP_CHUNK_BYTES;
            address = (uint8_t *)(uintptr_t)(
                proc->image_frames[g_map_page >> 20] +
                (g_map_page & (ZZAPP_MEM_FRAME_BYTES - 1u)));
            Xil_L1DCacheFlushRange((INTPTR)(uintptr_t)address, count);
            if (flags & PF_X)
                Xil_ICacheInvalidateRange((INTPTR)(uintptr_t)address, count);
            g_map_page += count;
            loader_note_work(count, ZZAPP_LOADER_WORK_MAP);
            return ZZAPP_LOADER_IN_PROGRESS;
        }
        while (g_map_page < image_pages_end &&
               scanned < ZZAPP_LOADER_MAP_CHUNK_BYTES) {
            uint32_t flags = zzapp_loader_page_flags(
                segs, img->nseg, img->image_end, g_map_page);
            uint32_t count = 0U;
            uint8_t *address;

            if (flags == 0U) {
                g_map_page += 0x1000U;
                scanned += 0x1000U;
                continue;
            }
            if ((g_map_page & (ZZAPP_MEM_FRAME_BYTES - 1u)) == 0U &&
                g_map_page + ZZAPP_MEM_FRAME_BYTES <= image_pages_end &&
                loader_section_has_flags(segs, img->nseg, img->image_end,
                                         g_map_page, flags))
                count = ZZAPP_MEM_FRAME_BYTES;
            else
                while (g_map_page + count < image_pages_end &&
                       count < ZZAPP_LOADER_MAP_CHUNK_BYTES - scanned &&
                       (g_map_page + count) / ZZAPP_MEM_FRAME_BYTES ==
                           g_map_page / ZZAPP_MEM_FRAME_BYTES &&
                       zzapp_loader_page_flags(
                           segs, img->nseg, img->image_end,
                           g_map_page + count) == flags)
                    count += 0x1000U;
            if (zzapp_as_map_image(
                    &proc->as, ZZAPP_AS_WINDOW_BASE + g_map_page,
                    proc->image_frames[g_map_page >> 20] +
                        (g_map_page & (ZZAPP_MEM_FRAME_BYTES - 1u)),
                    count, loader_image_permissions(flags)))
                goto failed;
            if (count == ZZAPP_MEM_FRAME_BYTES) {
                g_map_flush_end = g_map_page + count;
                return ZZAPP_LOADER_IN_PROGRESS;
            }
            address = (uint8_t *)(uintptr_t)(
                proc->image_frames[g_map_page >> 20] +
                (g_map_page & (ZZAPP_MEM_FRAME_BYTES - 1u)));
            Xil_L1DCacheFlushRange((INTPTR)(uintptr_t)address, count);
            if (flags & PF_X)
                Xil_ICacheInvalidateRange((INTPTR)(uintptr_t)address, count);
            g_map_page += count;
            scanned += count;
        }
        loader_note_work(scanned, ZZAPP_LOADER_WORK_MAP);
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
            if (count > ZZAPP_LOADER_MAP_CHUNK_BYTES)
                count = ZZAPP_LOADER_MAP_CHUNK_BYTES;
            if (zzapp_as_map_image(
                    &proc->as, ZZAPP_AS_WINDOW_BASE + g_map_tail,
                    proc->image_frames[g_map_tail >> 20] + in_frame,
                    count, ZZAPP_AS_IMAGE_RW))
                goto failed;
            g_map_tail += count;
            loader_note_work(count, ZZAPP_LOADER_WORK_MAP);
            return ZZAPP_LOADER_IN_PROGRESS;
        }
#ifndef ZZAPP_LOADER_INCREMENTAL_HOST_TEST
        __asm__ __volatile__("dsb\n\tmcr p15, 0, r0, c7, c5, 6\n\tdsb\n\tisb"
                             ::: "memory");
#endif
        return AAR_OK;
    }
    goto failed;

failed:
    /* A refused frame allocation is MEMORY; every other failure here is the
     * address space (L2 tables out of the record's reserve) or the image. */
    zzapp_services_note_refusal(refusal ? refusal : AAR_REASON_TABLES);
    zzapp_as_destroy(&proc->as);
    (void)zzapp_mem_release_proc(&g_app_memory, &proc->memory);
    return AAR_E_NOMEM;
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
    zzapp_proc_t *proc = zzapp_proc_slot(loader_pid());
    uint32_t frames = bytes / ZZAPP_MEM_FRAME_BYTES;
    uint32_t first;

    if (!proc || !frames || frames > ZZAPP_MEM_MAX_FRAMES - proc->heap_frame_count)
        return AAR_E_NOMEM;
    first = proc->heap_frame_count;
    if (zzapp_mem_alloc(&g_app_memory, &proc->memory, frames,
                        proc->heap_frames + first))
        return AAR_E_NOMEM;
    if (zzapp_as_map_frames(&proc->as, ZZAPP_AS_WINDOW_BASE + proc->heap_base +
                            first * ZZAPP_MEM_FRAME_BYTES,
                            proc->heap_frames + first, frames)) {
        while (frames--)
            (void)zzapp_mem_release(&g_app_memory, &proc->memory,
                                    proc->heap_frames[first + frames]);
        return AAR_E_NOMEM;
    }
    proc->heap_frame_count += bytes / ZZAPP_MEM_FRAME_BYTES;
    return AAR_OK;
}

int zzapp_loader_heap_shrink(uint32_t bytes)
{
    zzapp_proc_t *proc = zzapp_proc_slot(loader_pid());
    uint32_t frames = bytes / ZZAPP_MEM_FRAME_BYTES;
    uint32_t first;
    uint32_t i;

    if (!proc || !frames || frames > proc->heap_frame_count)
        return AAR_E_INVAL;
    first = proc->heap_frame_count - frames;
    if (zzapp_as_unmap_frames(&proc->as, ZZAPP_AS_WINDOW_BASE + proc->heap_base +
                              first * ZZAPP_MEM_FRAME_BYTES, frames))
        return AAR_E_INVAL;
    for (i = 0; i < frames; i++)
        (void)zzapp_mem_release(&g_app_memory, &proc->memory,
                                proc->heap_frames[first + i]);
    proc->heap_frame_count = first;
    return AAR_OK;
}

uint32_t zzapp_loader_pool_free(void)
{
    return g_app_memory_ready ? zzapp_mem_free_bytes(&g_app_memory) : 0u;
}
#else
static uint32_t pool_alloc(uint32_t bytes, uint32_t owner)
{
    if (owner == g_owner && g_host_mem)
        return 0;
    if (bytes == ZZAPP_HOST_WINDOW_BYTES) {
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
void zzapp_loader_memory_init(void)
{
    loader_host_reset();
}
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


/*
 * Retire stage 3, loader half: the host-visible blocks and the API layer's
 * resources (zzapp_loader_unbound_resources), then the card pool blocks the
 * record owns (the staging block while it is still held, and the back
 * buffer), the relocation table, and the host harness windows. Idempotent:
 * a replay after a core-1 restart finds the blocks gone and frees nothing.
 */
void zzapp_loader_retire_resources(void)
{
    zzapp_loader_unbound_resources();
#if !defined(ZZAPP_HOST_TEST) || defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
    if (g_card)
        card_pool_release_owner(&card_pool, g_owner);
    if (g_back)
        card_pool_free(&card_pool, g_back, g_back_owner);
#endif
#ifdef ZZAPP_HOST_TEST
    free(g_host_mem);
    free(g_host_back);
    free(g_host_file);
    g_host_mem = NULL;
    g_host_back = NULL;
    g_host_file = NULL;
    g_host_zero_last_bytes = 0U;
    g_host_work_last_bytes = 0U;
    g_host_reloc_last_entries = 0U;
    g_host_work_last_kind = 0U;
#endif
    g_back = 0;
    g_back_owner = 0;
    g_mem = NULL;
    g_bytes = 0;
    free_relocs();
}

/*
 * Retire stage 5, loader half: the frames go back to the ledger (the address
 * space that mapped them is gone by now), and the loader, host and process
 * state of the record are cleared.
 */
void zzapp_loader_retire_commit(void)
{
#if !defined(ZZAPP_HOST_TEST) || defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
    zzapp_proc_t *proc = zzapp_proc_slot(loader_pid());

    if (proc)
        (void)zzapp_mem_release_proc(&g_app_memory, &proc->memory);
#endif
    g_card = 0;
    g_owner = 0;
    g_generation = 0;
    g_file_end = 0;
    g_image_prepared = 0;
    g_zero_frames = 0U;
    g_zeroed_frames = 0U;
    g_zero_offset = 0U;
    g_map_segment = 0U;
    g_map_offset = 0U;
    g_map_page = 0U;
    g_map_tail = 0U;
    g_map_phase = LOADER_MAP_ZERO;
    g_ready = 0;
    zzapp_loader_unbound();
}

/* Everything the loader holds for the selected record goes back, and the
 * claim goes on: the record, its L2 reserve and its handle stay. */
static void drop_arena(void)
{
    zzapp_loader_retire_resources();
#if !defined(ZZAPP_HOST_TEST) || defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
    {
        zzapp_proc_t *proc = zzapp_proc_slot(loader_pid());

        if (proc)
            zzapp_as_destroy(&proc->as);
    }
#endif
    zzapp_loader_retire_commit();
}

int zzapp_loader_set_generation(uint32_t generation)
{
    if (generation == 0u || g_card != 0u)
        return AAR_E_INVAL;
    g_generation = generation;
    return AAR_OK;
}

/* Another record's staging block is still held (its LOAD_END has not copied
 * the image out). Staging is 16 MiB of class APP each, so a few loads that
 * start together can use the whole class limit between them although the
 * card has memory to spare. */
static int staging_held_by_others(void)
{
    uint32_t self = loader_pid();
    uint32_t pid;

    for (pid = 0U; pid < zzapp_rec_capacity(); pid++) {
        zzapp_rec_t *rec;
        const struct loader_slot *other;

        if (pid == self || zzapp_rec_state(pid) == ZZAPP_REC_FREE)
            continue;
        rec = zzapp_rec_by_pid(pid);
        if (!rec)
            continue;
        other = (const struct loader_slot *)(const void *)rec->loader.bytes;
        if (other->mem && other->bytes != 0U)
            return 1;
    }
    return 0;
}

int zzapp_loader_begin(void)
{
    uint32_t gen;
    uint32_t owner;
    uint32_t addr;

    if (zzapp_loader_state() != ZZAPP_ST_IDLE)
        return AAR_E_BUSY;
    gen = g_generation ? g_generation : zzapp_loader_generation() + 1u;
    if (gen == 0u)
        gen = 1u;
    owner = CARD_POOL_OWNER(CARD_POOL_CLASS_APP,
                            zzapp_rec_by_pid(loader_pid()) ?
                            zzapp_rec_by_pid(loader_pid())->hdr.owner_serial :
                            0U);
#if defined(ZZAPP_HOST_TEST) && !defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
    addr = pool_alloc(ZZAPP_HOST_WINDOW_BYTES, owner);
#else
    if (!g_app_memory_ready)
        return AAR_E_NOMEM;
    /* The staging block of the uploaded file: 16 MiB of class APP until
     * LOAD_END has copied the image into its frames. */
    addr = pool_alloc(ZZAPP_FILE_MAX, owner);
#endif
    if (!addr) {
        /* Short only because other claims hold staging: not a refusal. The
         * claim stays as it is and the client asks again (BUSY). */
        if (staging_held_by_others())
            return AAR_E_BUSY;
        zzapp_services_note_refusal(AAR_REASON_MEMORY);
        zzapp_loader_set_state(ZZAPP_ST_IDLE);
        return AAR_E_NOMEM;
    }
    g_owner = owner;
    g_file_end = 0;
    g_ready = 0;
#ifdef ZZAPP_LOADER_INCREMENTAL_HOST_TEST
#if !defined(ZZAPP_LOADER_REAL_AS_HOST_TEST)
    g_host_mapping_calls = 0U;
#endif
    g_host_flush_count = 0U;
    g_host_flush_dropped = 0U;
    g_host_reloc_last_entries = 0U;
#endif
#if defined(ZZAPP_HOST_TEST) && !defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
    g_card = addr;
    g_bytes = ZZAPP_HOST_WINDOW_BYTES;
    g_mem = g_host_mem;
    zzapp_loader_bound(g_card, g_mem, owner, gen);
#elif defined(ZZAPP_HOST_TEST)
    /* The flat window the host-run app executes in is not the staging block;
     * it stays until the arena is dropped. */
    g_host_mem = (uint8_t *)malloc(ZZAPP_HOST_WINDOW_BYTES);
    if (!g_host_mem) {
        card_pool_free(&card_pool, addr, owner);
        zzapp_services_note_refusal(AAR_REASON_MEMORY);
        zzapp_loader_set_state(ZZAPP_ST_IDLE);
        return AAR_E_NOMEM;
    }
    g_card = HOST_CARD;
    g_bytes = ZZAPP_FILE_MAX;
    g_mem = (uint8_t *)(uintptr_t)addr;
    zzapp_loader_bound(g_card, g_host_mem, owner, gen);
#else
    g_card = ZZAPP_AS_WINDOW_BASE;
    g_bytes = ZZAPP_FILE_MAX;
    g_mem = (uint8_t *)(uintptr_t)addr;
    /* On the card the host block's `mem` is only a flag that an arena is
     * bound: the app's memory is its frames, behind the window. */
    zzapp_loader_bound(g_card, (uint8_t *)(uintptr_t)ZZAPP_AS_WINDOW_BASE,
                       owner, gen);
#endif
    zzapp_services_on_launch();
    zzapp_loader_set_state(ZZAPP_ST_LOADING);
    return AAR_OK;
}

int zzapp_loader_chunk(uint32_t file_off, const void *src, uint32_t len)
{
    if (zzapp_loader_state() != ZZAPP_ST_LOADING || !g_mem)
        return AAR_E_INVAL;
    if (!src || len < ZZAPP_CHUNK_MIN || len > ZZAPP_CHUNK_MAX)
        return AAR_E_INVAL;
    if (!range_ok(file_off, len, ZZAPP_FILE_MAX) ||
        !range_ok(file_off, len, g_bytes))
        return AAR_E_INVAL;
    memcpy(g_mem + file_off, src, len);
    if (file_off + len > g_file_end)
        g_file_end = file_off + len;
    return AAR_OK;
}

/* AAR_CBF_* bits (AAR_CB_FLAGS) for the groups an app names. */
static uint32_t note_wire_flags(uint32_t groups)
{
    uint32_t flags = 0u;

    if (groups & AAR_GROUP_DISPLAY)
        flags |= AAR_CBF_DISPLAY;
    if (groups & AAR_GROUP_AUDIO)
        flags |= AAR_CBF_AUDIO;
    if (groups & AAR_GROUP_GL)
        flags |= AAR_CBF_GL;
    return flags;
}

/* Applies spec 2.3 to one 40-byte AAR note descriptor. */
static int note_descriptor_ok(const uint8_t *desc, struct load_image *img)
{
    uint32_t magic = rd32(desc);
    uint32_t version = rd32(desc + 4u);
    uint32_t profile = rd32(desc + 8u);
    uint32_t api_major = rd32(desc + 12u);
    uint32_t api_minor = rd32(desc + 16u);
    uint32_t stack_bytes = rd32(desc + 20u);
    uint32_t min_heap_mib = rd32(desc + 24u);
    uint32_t groups_required = rd32(desc + 28u);
    uint32_t groups_optional = rd32(desc + 32u);
    uint32_t reserved = rd32(desc + 36u);

    if (magic != AAR_NOTE_MAGIC || version != AAR_NOTE_VERSION)
        return AAR_E_INVAL;
    /* The image is ELF32 ARM: profile A32. */
    if (profile != AAR_PROFILE_A32)
        return AAR_E_PROFILE;
    if (api_major != AAR_API_MAJOR || api_minor > AAR_API_MINOR)
        return AAR_E_INVAL;
    if (stack_bytes == 0u)
        stack_bytes = ZZAPP_STACK_DEFAULT;
    if (stack_bytes < ZZAPP_STACK_MIN || stack_bytes > ZZAPP_STACK_MAX ||
        (stack_bytes & 15u) != 0u)
        return AAR_E_INVAL;
    if (reserved != 0u)
        return AAR_E_INVAL;
    if (groups_required & ~zzapp_runtime_groups())
        return AAR_E_NOGROUP;
    if (min_heap_mib >
        ZZAPP_MEM_PRIVATE_WINDOW_BYTES / ZZAPP_MEM_FRAME_BYTES)
        return AAR_E_NOMEM;
    img->stack_bytes = stack_bytes;
    img->note_flags = note_wire_flags(groups_required | groups_optional);
    img->min_heap_bytes = min_heap_mib << 20;
    return AAR_OK;
}

/* Walks the notes of one segment or section. Counts every AAR note (owner
 * "AAR", type AAR_NOTE_TYPE) in *found and applies the descriptor rules to
 * it; other notes are skipped. */
static int note_scan(const uint8_t *note, uint32_t nlen, struct load_image *img,
                     uint32_t *found)
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
            return AAR_E_INVAL;
        desc = off + 12u + name_pad;
        if (!range_ok(desc, descsz, nlen))
            return AAR_E_INVAL;
        if (type == AAR_NOTE_TYPE && namesz == 4u &&
            memcmp(note + off + 12u, "AAR", 4) == 0) {
            int rc;

            if (descsz != AAR_NOTE_DESC_BYTES)
                return AAR_E_INVAL;
            if (++*found > 1u)
                return AAR_E_INVAL;
            rc = note_descriptor_ok(note + desc, img);
            if (rc != AAR_OK)
                return rc;
        }
        if (desc + desc_pad < desc)
            return AAR_E_INVAL;
        off = desc + desc_pad;
    }
    return AAR_OK;
}

static int find_note(struct load_image *img, uint16_t phnum,
                     uint16_t phentsize, uint32_t phoff)
{
    uint32_t found = 0u;
    uint16_t i;
    int rc;

    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = g_mem + phoff + (uint32_t)i * phentsize;
        uint32_t type = rd32(ph);
        uint32_t off = rd32(ph + 4u);
        uint32_t filesz = rd32(ph + 16u);

        if (type != PT_NOTE)
            continue;
        if (!range_ok(off, filesz, g_file_end))
            return AAR_E_INVAL;
        rc = note_scan(g_mem + off, filesz, img, &found);
        if (rc != AAR_OK)
            return rc;
    }
    /* No SHT_NOTE fallback: spec 2.2.7 puts the note in a PT_NOTE segment. */
    return found == 1u ? AAR_OK : AAR_E_INVAL;
}

static int vaddr_to_file(const struct load_image *img, uint32_t vaddr,
                         uint32_t bytes, uint32_t *off)
{
    uint32_t i;

    for (i = 0; i < img->nseg; i++) {
        const struct load_seg *seg = &img->seg[i];

        if (vaddr >= seg->vaddr &&
            range_ok(vaddr - seg->vaddr, bytes, seg->filesz)) {
            *off = seg->off + (vaddr - seg->vaddr);
            return 1;
        }
    }
    return 0;
}

/* PT_DYNAMIC: refuses DT_NEEDED and a PLT relocation table, and finds the
 * REL and RELA tables, whose DT values are virtual addresses inside a PT_LOAD
 * (needs the segments, so after they were read). A table that is named
 * without a size, a size that is not a whole number of entries, an entry
 * size other than the one the format fixes, or a table outside the file
 * bytes of every segment is refused. */
static int parse_dynamic(struct load_image *img, uint32_t off, uint32_t filesz)
{
    uint32_t rel = 0U, relsz = 0U, relent = 0U;
    uint32_t rela = 0U, relasz = 0U, relaent = 0U;
    uint32_t have_rel = 0U, have_rela = 0U;
    uint32_t i;

    if (!range_ok(off, filesz, g_file_end) || (filesz & 7u) != 0u)
        return AAR_E_INVAL;
    for (i = 0; i + 8u <= filesz; i += 8u) {
        int32_t tag = (int32_t)rd32(g_mem + off + i);
        uint32_t value = rd32(g_mem + off + i + 4u);

        if (tag == DT_NULL)
            break;
        switch (tag) {
        case DT_NEEDED:
            return AAR_E_INVAL;
        case DT_PLTRELSZ:
            if (value != 0U)
                return AAR_E_INVAL;
            break;
        case DT_REL:
            rel = value;
            have_rel = 1U;
            break;
        case DT_RELSZ:
            relsz = value;
            break;
        case DT_RELENT:
            relent = value;
            break;
        case DT_RELA:
            rela = value;
            have_rela = 1U;
            break;
        case DT_RELASZ:
            relasz = value;
            break;
        case DT_RELAENT:
            relaent = value;
            break;
        case DT_RELR:
        case DT_RELRSZ:
        case DT_RELRENT:
            /* Spec 2.2.5: packed relative relocations are refused in v1. */
            return AAR_E_INVAL;
        default:
            break;
        }
    }
    if (relent != 0U && relent != LOADER_REL_BYTES)
        return AAR_E_INVAL;
    if (relaent != 0U && relaent != LOADER_RELA_BYTES)
        return AAR_E_INVAL;
    if (relsz != 0U) {
        if (!have_rel || relent != LOADER_REL_BYTES ||
            (relsz % LOADER_REL_BYTES) != 0U ||
            !vaddr_to_file(img, rel, relsz, &img->rel_off))
            return AAR_E_INVAL;
        img->rel_bytes = relsz;
    }
    if (relasz != 0U) {
        if (!have_rela || relaent != LOADER_RELA_BYTES ||
            (relasz % LOADER_RELA_BYTES) != 0U ||
            !vaddr_to_file(img, rela, relasz, &img->rela_off))
            return AAR_E_INVAL;
        img->rela_bytes = relasz;
    }
    return AAR_OK;
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
    uint32_t dyn_off = 0U;
    uint32_t dyn_size = 0U;
    int have_dyn = 0;
    int rc;

    memset(img, 0, sizeof(*img));
    if (g_file_end < 52u)
        return AAR_E_INVAL;
    eh = g_mem;
    if (eh[0] != 0x7fu || eh[1] != 'E' || eh[2] != 'L' || eh[3] != 'F')
        return AAR_E_INVAL;
    if (eh[4] != 1u || eh[5] != 1u || eh[6] != 1u)
        return AAR_E_INVAL;
    if (rd16(eh + 16u) != ET_DYN || rd16(eh + 18u) != EM_ARM)
        return AAR_E_INVAL;
    /* Spec 2.2.2: EABI version 5, hard float. */
    if ((rd32(eh + 36u) & EF_ARM_EABIMASK) != EF_ARM_EABI_VER5 ||
        (rd32(eh + 36u) & EF_ARM_ABI_FLOAT_HARD) == 0u ||
        (rd32(eh + 36u) & EF_ARM_ABI_FLOAT_SOFT) != 0u)
        return AAR_E_INVAL;
    phoff = rd32(eh + 28u);
    shoff = rd32(eh + 32u);
    phentsize = rd16(eh + 42u);
    phnum = rd16(eh + 44u);
    shentsize = rd16(eh + 46u);
    shnum = rd16(eh + 48u);
    if (phentsize < 32u || phnum == 0u || phnum > ZZAPP_MAX_PHDR)
        return AAR_E_INVAL;
    /* An image without section headers has e_shnum 0 and any e_shentsize. */
    if (shnum && (shentsize < 40u || shnum > ZZAPP_MAX_SHDR))
        return AAR_E_INVAL;
    if (!range_ok(phoff, (uint32_t)phnum * phentsize, g_file_end))
        return AAR_E_INVAL;
    if (shnum && !range_ok(shoff, (uint32_t)shnum * shentsize, g_file_end))
        return AAR_E_INVAL;

    rc = find_note(img, phnum, phentsize, phoff);
    if (rc != AAR_OK)
        return rc;

    image_limit = ZZAPP_MEM_PRIVATE_WINDOW_BYTES;
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
            return AAR_E_INVAL;
        if (type == PT_DYNAMIC) {
            if (have_dyn)
                return AAR_E_INVAL;
            have_dyn = 1;
            dyn_off = off;
            dyn_size = filesz;
        }
        if (type != PT_LOAD)
            continue;
        if (img->nseg >= ZZAPP_MAX_LOAD)
            return AAR_E_INVAL;
        if ((flags & PF_W) && (flags & PF_X))
            return AAR_E_INVAL;
        if (memsz < filesz)
            return AAR_E_INVAL;
        if (!range_ok(off, filesz, g_file_end))
            return AAR_E_INVAL;
        if (!range_ok(vaddr, memsz, image_limit))
            return AAR_E_INVAL;
        if (filesz_sum + filesz < filesz_sum)
            return AAR_E_INVAL;
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
        return AAR_E_INVAL;
    if (memsz_sum + img->stack_bytes + ZZAPP_IMAGE_SLACK >
        ZZAPP_MEM_PRIVATE_WINDOW_BYTES)
        return AAR_E_INVAL;
    /* The heap starts here, so VFP/NEdouble data in app allocations
     * (GLQuake's hunk typed loads, newlib's double-heavy code) keeps its
     * natural alignment whatever an image's .bss tail happens to be.  An
     * 8-byte round let a rebuilt image with a larger .data shift the heap
     * base and abort the app on a misaligned float load. */
    img->image_end = (img->image_end + 31u) & ~31u;
    if (img->image_end + img->stack_bytes > ZZAPP_MEM_PRIVATE_WINDOW_BYTES)
        return AAR_E_INVAL;
    for (i = 0; i < img->nseg; i++) {
        uint32_t a = img->seg[i].vaddr;
        uint32_t ae = a + img->seg[i].memsz;
        uint16_t j;

        if (ae > ZZAPP_MEM_PRIVATE_WINDOW_BYTES - img->stack_bytes)
            return AAR_E_INVAL;
        for (j = (uint16_t)(i + 1u); j < img->nseg; j++) {
            uint32_t b = img->seg[j].vaddr;
            uint32_t be = b + img->seg[j].memsz;

            if (a < be && b < ae)
                return AAR_E_INVAL;
        }
    }
    if (have_dyn) {
        rc = parse_dynamic(img, dyn_off, dyn_size);
        if (rc != AAR_OK)
            return rc;
    }
    img->entry = rd32(eh + 24u);
    if (img->entry >= ZZAPP_MEM_PRIVATE_WINDOW_BYTES - img->stack_bytes)
        return AAR_E_INVAL;
    /* Spec 2.2.6: inside an executable segment, and A32 entries are even. */
    if ((img->entry & 1u) != 0u)
        return AAR_E_INVAL;
    for (i = 0; i < img->nseg; i++) {
        const struct load_seg *seg = &img->seg[i];

        if ((seg->flags & PF_X) && img->entry >= seg->vaddr &&
            img->entry - seg->vaddr < seg->memsz)
            break;
    }
    if (i == img->nseg)
        return AAR_E_INVAL;
    return AAR_OK;
}

#if defined(ZZAPP_HOST_TEST) && !defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
static int overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return an && bn && a < b + bn && b < a + an;
}

/* Move each PT_LOAD from its file offset to p_vaddr. A segment is copied
 * only once that write cannot clobber another segment's still-unread file
 * bytes. Equal offset and vaddr is a no-op. A cycle returns AAR_E_NOMEM
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
            return AAR_E_NOMEM;
    }
    if (left)
        return AAR_E_NOMEM;
    /* Zero only after every source byte has moved: a BSS tail can overlap a
     * not-yet-copied file image even when PT_LOAD destinations do not overlap. */
    for (i = 0; i < img->nseg; i++) {
        if (img->seg[i].memsz > img->seg[i].filesz)
            memset(g_mem + img->seg[i].vaddr + img->seg[i].filesz, 0,
                   img->seg[i].memsz - img->seg[i].filesz);
    }
    return AAR_OK;
}
#endif

static int prepare_loaded_image(void)
{
    uint32_t copied = 0U;

    if (!g_reloc_prepare_started) {
        uint32_t entries;
        uint32_t saved_n;
        int rc = parse_elf(&g_image);

        if (rc != AAR_OK)
            return rc;
        if (!image_commit_fits_limit(&g_image)) {
            zzapp_services_note_refusal(AAR_REASON_MEMORY);
            return AAR_E_NOMEM;
        }
        entries = g_image.rel_bytes / LOADER_REL_BYTES +
                  g_image.rela_bytes / LOADER_RELA_BYTES;
        if (entries > UINT32_MAX / 16u)
            return AAR_E_NOMEM;
        saved_n = entries * 16u;
        g_relocs = saved_n ? (uint8_t *)malloc(saved_n) : NULL;
        if (saved_n && !g_relocs)
            return AAR_E_NOMEM;
        g_relocs_bytes = saved_n;
        g_reloc_prepare_started = 1U;
    }

    /* Table 0 is DT_REL, table 1 is DT_RELA; g_reloc_section counts them. */
    while (g_reloc_section < 2u &&
           copied < ZZAPP_LOADER_RELOC_CHUNK_ENTRIES) {
        uint32_t kind = g_reloc_section == 0u ? LOADER_RELOC_REL :
                                                LOADER_RELOC_RELA;
        uint32_t stride = kind == LOADER_RELOC_REL ? LOADER_REL_BYTES :
                                                     LOADER_RELA_BYTES;
        uint32_t table = kind == LOADER_RELOC_REL ? g_image.rel_off :
                                                    g_image.rela_off;
        uint32_t entries = (kind == LOADER_RELOC_REL ? g_image.rel_bytes :
                                                       g_image.rela_bytes) /
                           stride;

        while (g_reloc_entry < entries &&
               copied < ZZAPP_LOADER_RELOC_CHUNK_ENTRIES) {
            const uint8_t *rel = g_mem + table + g_reloc_entry * stride;
            uint32_t roff = rd32(rel);
            uint32_t info = rd32(rel + 4u);

            if ((info & 0xffu) != R_ARM_RELATIVE || (info >> 8) != 0u ||
                (roff & 3u) != 0u ||
                !range_ok(roff, 4u, ZZAPP_MEM_PRIVATE_WINDOW_BYTES))
                return AAR_E_INVAL;
            memcpy(g_relocs + g_reloc_written, rel, 8u);
            memcpy(g_relocs + g_reloc_written + 8u, &kind, 4u);
            if (kind == LOADER_RELOC_RELA)
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
    loader_note_work(copied * 16U, ZZAPP_LOADER_WORK_RELOC);
    if (g_reloc_section != 2u)
        return ZZAPP_LOADER_IN_PROGRESS;
    g_image_prepared = 1;
    /* Do not combine the final extraction slice with allocation or zeroing. */
    return ZZAPP_LOADER_IN_PROGRESS;
}

static int apply_loaded_relocations(void)
{
    uint32_t applied = 0U;
#if !defined(ZZAPP_HOST_TEST) || defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
    uintptr_t span_lo = 0U;
    uintptr_t span_hi = 0U;
#endif

    while (g_reloc_apply < g_relocs_bytes &&
           applied < ZZAPP_LOADER_RELOC_APPLY_CHUNK_ENTRIES) {
        uint32_t at = g_reloc_apply;
        uint32_t roff = rd32(g_relocs + at);
        uint32_t type = rd32(g_relocs + at + 8u);
#if defined(ZZAPP_HOST_TEST) && !defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
        uint32_t addend = type == LOADER_RELOC_REL ? rd32(g_mem + roff) :
                                             rd32(g_relocs + at + 12u);
        uint32_t place = addend + g_card;

        memcpy(g_mem + roff, &place, sizeof(place));
#else
        zzapp_proc_t *proc = zzapp_proc_slot(loader_pid());
        uint8_t *target = proc ? image_frame_ptr(proc, roff) : NULL;
        uint32_t addend;
        uint32_t place;

        if (!target)
            return AAR_E_INVAL;
        addend = type == LOADER_RELOC_REL ? rd32(target) :
                                            rd32(g_relocs + at + 12u);
        place = addend + g_card;
        memcpy(target, &place, sizeof(place));
        /* One flush per contiguous span replaces one flush per word.
         * Each word is flushed after its write and before this pass
         * returns, so the point of coherency is reached exactly as
         * before; words farther apart than a cache line never merge
         * into an over-wide range flush. */
        if (span_lo &&
            ((uintptr_t)target > span_hi + ZZAPP_LOADER_CACHE_LINE_BYTES ||
             (uintptr_t)target + sizeof(place) +
                     ZZAPP_LOADER_CACHE_LINE_BYTES < span_lo)) {
            Xil_L1DCacheFlushRange((INTPTR)span_lo,
                                   (uint32_t)(span_hi - span_lo));
            span_lo = (uintptr_t)target;
            span_hi = span_lo + sizeof(place);
        } else {
            if (span_lo == 0U || (uintptr_t)target < span_lo)
                span_lo = (uintptr_t)target;
            if ((uintptr_t)target + sizeof(place) > span_hi)
                span_hi = (uintptr_t)target + sizeof(place);
        }
#endif
        g_reloc_apply += 16u;
        applied++;
    }
#if !defined(ZZAPP_HOST_TEST) || defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
    if (span_lo)
        Xil_L1DCacheFlushRange((INTPTR)span_lo, (uint32_t)(span_hi - span_lo));
#endif
    loader_note_relocs(applied);
    loader_note_work(applied * 16U, ZZAPP_LOADER_WORK_APPLY);
    return g_reloc_apply == g_relocs_bytes ? AAR_OK :
        ZZAPP_LOADER_IN_PROGRESS;
}

/*
 * LOAD_END is done: the image sits in the record's frames and nothing reads
 * the uploaded file again, so the 16 MiB staging block goes back to the pool
 * (design 3.2). Without this a live app would hold 16 MiB of class APP for
 * its whole life and the class limit would cap the number of apps. The host
 * harness's flat window is the app's memory there, not staging, and stays.
 */
static void release_staging(void)
{
#if !defined(ZZAPP_HOST_TEST) || defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
    if (g_mem) {
        card_pool_free(&card_pool, (uint32_t)(uintptr_t)g_mem, g_owner);
        g_mem = NULL;
        g_bytes = 0;
        g_file_end = 0;
    }
#endif
}

int zzapp_loader_end(void)
{
    int rc;

    if (zzapp_loader_state() != ZZAPP_ST_LOADING || !g_mem)
        return AAR_E_INVAL;
    if (!g_image_prepared) {
        rc = prepare_loaded_image();
        if (rc == ZZAPP_LOADER_IN_PROGRESS)
            return rc;
        if (rc != AAR_OK) {
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
    if (rc != AAR_OK)
        goto failed;
    rc = apply_loaded_relocations();
    if (rc == ZZAPP_LOADER_IN_PROGRESS)
        return rc;
    if (rc != AAR_OK)
        goto failed;
    free_relocs();
#if defined(ZZAPP_HOST_TEST) && !defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
    cache_image(&g_image);
#endif
    rc = zzapp_loader_set_image(g_image.entry, g_image.image_end,
                                g_image.stack_bytes, g_image.note_flags,
                                g_image.min_heap_bytes);
    if (rc != AAR_OK) {
        zzapp_services_note_refusal(AAR_REASON_MEMORY);
        goto failed;
    }
    g_ready = 1;
    release_staging();
    return AAR_OK;

failed:
    drop_arena();
    zzapp_loader_set_state(ZZAPP_ST_IDLE);
    return rc;
}

/* The claim goes on: the resources go back and the host state is idle (the
 * control block's IDLE is published by the retire that ends the claim). */
int zzapp_loader_abort(void)
{
    uint32_t state = zzapp_loader_state();

    if (state != ZZAPP_ST_LOADING && state != ZZAPP_ST_STARTING)
        return AAR_E_BUSY;
    drop_arena();
    zzapp_loader_set_state(ZZAPP_ST_IDLE);
    return AAR_OK;
}

int zzapp_loader_alloc_back(uint32_t bytes, uint32_t *addr_out)
{
    uint32_t addr;

    if (!addr_out || !bytes)
        return AAR_E_INVAL;
    if (g_back)
        return AAR_E_BUSY;
    if (!g_owner)
        return AAR_E_INVAL;
    /* A firmware-owned display allocation cannot consume the app arena's
     * class budget, which is already fully reserved by the live arena. */
    g_back_owner = CARD_POOL_OWNER(CARD_POOL_CLASS_APP_DISPLAY, g_owner);
    addr = pool_alloc(bytes, g_back_owner);
    if (!addr) {
        g_back_owner = 0;
        return AAR_E_NOMEM;
    }
    g_back = addr;
    *addr_out = addr;
    return AAR_OK;
}

void zzapp_loader_free_back(uint32_t addr)
{
    if (!addr || addr != g_back)
        return;
#if !defined(ZZAPP_HOST_TEST) || defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
    card_pool_free(&card_pool, g_back, g_back_owner);
#else
    free(g_host_back);
    g_host_back = NULL;
#endif
    g_back = 0;
}

uint32_t zzapp_loader_card(void)
{
    return loader_rec_or_null() ? g_card : 0U;
}

uint8_t *zzapp_loader_mem(void)
{
    return loader_rec_or_null() ? g_mem : NULL;
}

int zzapp_loader_ready(void)
{
    return loader_rec_or_null() ? g_ready : 0;
}

void zzapp_loader_release(void)
{
    drop_arena();
}

#ifdef ZZAPP_HOST_TEST
uint32_t zzapp_test_arena_card(void)
{
    return loader_rec_or_null() ? g_card : 0U;
}

uint8_t *zzapp_test_arena_mem(void)
{
    return loader_rec_or_null() ? g_mem : NULL;
}
uint32_t zzapp_test_loader_zero_last_bytes(void)
{
    return loader_rec_or_null() ? g_host_zero_last_bytes : 0U;
}
uint32_t zzapp_test_loader_work_last_bytes(void)
{
    return loader_rec_or_null() ? g_host_work_last_bytes : 0U;
}

#if defined(ZZAPP_HOST_TEST) && defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST)
uint32_t zzapp_test_loader_flush_count(void)
{
    return g_host_flush_count;
}

int zzapp_test_loader_flush_range(uint32_t index, uintptr_t *addr,
                                  uint32_t *bytes)
{
    if (index >= g_host_flush_count)
        return 0;
    *addr = g_host_flush[index].addr;
    *bytes = g_host_flush[index].bytes;
    return 1;
}

uint32_t zzapp_test_loader_flush_dropped(void)
{
    return g_host_flush_dropped;
}

uint32_t zzapp_test_loader_reloc_entries_last(void)
{
    return loader_rec_or_null() ? g_host_reloc_last_entries : 0U;
}
#endif
#if defined(ZZAPP_LOADER_INCREMENTAL_HOST_TEST) && \
    !defined(ZZAPP_LOADER_REAL_AS_HOST_TEST)
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

uint32_t zzapp_test_loader_work_last_kind(void)
{
    return loader_rec_or_null() ? g_host_work_last_kind : 0U;
}
#endif
