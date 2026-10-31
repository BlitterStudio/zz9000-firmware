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
    uint32_t image_end;
    uint32_t nseg;
    struct load_seg seg[ZZAPP_MAX_LOAD];
};

/* Defined in zzapp_host.c. The loader does not own the lifecycle words. */
void zzapp_loader_bound(uint32_t card, uint8_t *mem, uint32_t bytes,
                        uint32_t owner, uint32_t generation);
void zzapp_loader_unbound(void);
uint32_t zzapp_loader_generation(void);
uint32_t zzapp_loader_state(void);
void zzapp_loader_set_state(uint32_t state);
void zzapp_loader_set_image(uint32_t entry, uint32_t image_end,
                            uint32_t stack_bytes, uint32_t note_flags);
int zzapp_loader_have_image(void);

static uint32_t g_card;
static uint8_t *g_mem;
static uint32_t g_bytes;
static uint32_t g_owner;
static uint32_t g_file_end;
static uint32_t g_back;
static uint32_t g_back_owner;
static int g_ready;

#ifdef ZZAPP_HOST_TEST
static uint8_t *g_host_mem;
static uint8_t *g_host_back;
#define HOST_CARD 0x20000000u
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

#ifndef ZZAPP_HOST_TEST
#include "card_pool.h"
#include "memorymap.h"
#include "xil_cache.h"

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

static void cache_image(const struct load_image *img)
{
    uint32_t i;

    Xil_DCacheFlushRange((INTPTR)g_card, g_bytes);
    for (i = 0; i < img->nseg; i++) {
        if (img->seg[i].flags & PF_X) {
            Xil_ICacheInvalidateRange((INTPTR)(g_card + img->seg[i].vaddr),
                                      img->seg[i].memsz);
        }
    }
    __asm__ __volatile__("dsb" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
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
#endif

static void drop_arena(void)
{
#ifndef ZZAPP_HOST_TEST
    if (g_card)
        card_pool_release_owner(&card_pool, g_owner);
    if (g_back)
        card_pool_free(&card_pool, g_back, g_back_owner);
#else
    free(g_host_mem);
    free(g_host_back);
    g_host_mem = NULL;
    g_host_back = NULL;
#endif
    g_card = 0;
    g_mem = NULL;
    g_bytes = 0;
    g_owner = 0;
    g_file_end = 0;
    g_back = 0;
    g_back_owner = 0;
    g_ready = 0;
    zzapp_loader_unbound();
}

int zzapp_loader_begin(void)
{
    uint32_t gen;
    uint32_t owner;
    uint32_t addr;

    if (zzapp_loader_state() != ZZAPP_ST_IDLE)
        return ZZ_EBUSY;
    gen = zzapp_loader_generation() + 1u;
    if (gen == 0u || (gen & 0x00FFFFFFu) == 0u)
        gen = 1u;
    owner = ((uint32_t)CARD_POOL_CLASS_APP << 24) | (gen & 0x00FFFFFFu);
    addr = pool_alloc(ZZAPP_ARENA_BYTES, owner);
    if (!addr) {
        zzapp_loader_set_state(ZZAPP_ST_IDLE);
        return ZZ_ENOMEM;
    }
    g_owner = owner;
    g_card = addr;
    g_bytes = ZZAPP_ARENA_BYTES;
    g_file_end = 0;
    g_ready = 0;
#ifdef ZZAPP_HOST_TEST
    g_mem = g_host_mem;
#else
    g_mem = (uint8_t *)(uintptr_t)addr;
#endif
    zzapp_loader_bound(g_card, g_mem, g_bytes, owner, gen);
    {
        uint8_t *life = (uint8_t *)zzapp_services_window();
        if (life) {
            zzapp_put_be32(life + ZZAPP_LIFE_STOP_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_FAULT_OFF, ZZAPP_FAULT_NONE);
            zzapp_put_be32(life + ZZAPP_LIFE_EXIT_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_FLAGS_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_INPUT_DROPS_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_LOG_DROPS_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_FLIP_GEN_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_FLIP_ACK_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_FLIP_SHOWING_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_WIDTH_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_HEIGHT_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_PITCH_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_FORMAT_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_CARD_FRONT_OFF, 0);
            zzapp_put_be32(life + ZZAPP_LIFE_CARD_BACK_OFF, 0);
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
        if (note_ok(g_mem + off, filesz, img) == ZZ_OK)
            return ZZ_OK;
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
        if (note_ok(g_mem + off, size, img) == ZZ_OK)
            return ZZ_OK;
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
        if (tag == DT_NEEDED || tag == DT_REL)
            return ZZ_EINVAL;
    }
    return ZZ_OK;
}

static int relocs_ok(uint16_t shnum, uint16_t shentsize, uint32_t shoff)
{
    uint16_t i;
    int saw = 0;

    if (!shoff || !shnum)
        return ZZ_EINVAL;
    for (i = 0; i < shnum; i++) {
        const uint8_t *sh = g_mem + shoff + (uint32_t)i * shentsize;
        uint32_t type = rd32(sh + 4u);
        uint32_t flags = rd32(sh + 8u);
        uint32_t addr = rd32(sh + 12u);
        uint32_t off = rd32(sh + 16u);
        uint32_t size = rd32(sh + 20u);
        uint32_t entsize = rd32(sh + 36u);
        uint32_t n;
        uint32_t r;

        if (type == SHT_REL)
            return ZZ_EINVAL;
        if ((flags & SHF_ALLOC) && size) {
            if (!range_ok(addr, size, g_bytes))
                return ZZ_EINVAL;
        }
        if (type != SHT_RELA)
            continue;
        if (entsize != 12u || (size % 12u) != 0u)
            return ZZ_EINVAL;
        if (!range_ok(off, size, g_file_end))
            return ZZ_EINVAL;
        n = size / 12u;
        for (r = 0; r < n; r++) {
            const uint8_t *rel = g_mem + off + r * 12u;
            uint32_t roff = rd32(rel);
            uint32_t info = rd32(rel + 4u);

            if ((info & 0xffu) != R_ARM_RELATIVE || (info >> 8) != 0u)
                return ZZ_EINVAL;
            if ((roff & 3u) != 0u || !range_ok(roff, 4u, g_bytes))
                return ZZ_EINVAL;
        }
        saw = 1;
        (void)saw;
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
        if (!range_ok(vaddr, memsz, g_bytes))
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
    if (memsz_sum + img->stack_bytes + ZZAPP_IMAGE_SLACK > ZZAPP_ARENA_BYTES)
        return ZZ_EINVAL;
    img->image_end = (img->image_end + 7u) & ~7u;
    if (img->image_end + img->stack_bytes > g_bytes)
        return ZZ_EINVAL;
    for (i = 0; i < img->nseg; i++) {
        uint32_t a = img->seg[i].vaddr;
        uint32_t ae = a + img->seg[i].memsz;
        uint16_t j;

        if (ae > g_bytes - img->stack_bytes)
            return ZZ_EINVAL;
        for (j = (uint16_t)(i + 1u); j < img->nseg; j++) {
            uint32_t b = img->seg[j].vaddr;
            uint32_t be = b + img->seg[j].memsz;

            if (a < be && b < ae)
                return ZZ_EINVAL;
        }
    }
    img->entry = rd32(eh + 24u);
    if (img->entry >= g_bytes - img->stack_bytes)
        return ZZ_EINVAL;
    return relocs_ok(shnum, shentsize, shoff);
}

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
                if (overlap(img->seg[i].vaddr, img->seg[i].filesz,
                            img->seg[j].off, img->seg[j].filesz))
                    blocked = 1;
            }
            if (blocked)
                continue;
            if (img->seg[i].filesz && img->seg[i].vaddr != img->seg[i].off)
                memmove(g_mem + img->seg[i].vaddr, g_mem + img->seg[i].off,
                        img->seg[i].filesz);
            if (img->seg[i].memsz > img->seg[i].filesz)
                memset(g_mem + img->seg[i].vaddr + img->seg[i].filesz, 0,
                       img->seg[i].memsz - img->seg[i].filesz);
            done[i] = 1;
            left--;
            moved = 1;
        }
        if (!moved)
            return ZZ_ENOMEM;
    }
    return left ? ZZ_ENOMEM : ZZ_OK;
}

int zzapp_loader_end(void)
{
    struct load_image img;
    uint16_t shnum;
    uint16_t shentsize;
    uint32_t shoff;
    int rc;

    if (zzapp_loader_state() != ZZAPP_ST_LOADING || !g_mem)
        return ZZ_EINVAL;
    rc = parse_elf(&img);
    if (rc != ZZ_OK) {
        drop_arena();
        zzapp_loader_set_state(ZZAPP_ST_IDLE);
        return rc;
    }
    shoff = rd32(g_mem + 32u);
    shentsize = rd16(g_mem + 46u);
    shnum = rd16(g_mem + 48u);
    /* Relocs are applied after the segments move, from the bounce copy
     * of the section bytes. Copy the reloc records first: place_segments
     * wipes the file image. */
    {
        uint8_t *saved = NULL;
        uint32_t saved_n = 0;
        uint16_t i;

        for (i = 0; i < shnum; i++) {
            const uint8_t *sh = g_mem + shoff + (uint32_t)i * shentsize;
            if (rd32(sh + 4u) == SHT_RELA)
                saved_n += rd32(sh + 20u);
        }
#ifdef ZZAPP_HOST_TEST
        saved = saved_n ? (uint8_t *)malloc(saved_n) : NULL;
#else
        saved = saved_n ? (uint8_t *)malloc(saved_n) : NULL;
#endif
        if (saved_n && !saved) {
            drop_arena();
            zzapp_loader_set_state(ZZAPP_ST_IDLE);
            return ZZ_ENOMEM;
        }
        if (saved_n) {
            uint32_t at = 0;
            for (i = 0; i < shnum; i++) {
                const uint8_t *sh = g_mem + shoff + (uint32_t)i * shentsize;
                uint32_t size;

                if (rd32(sh + 4u) != SHT_RELA)
                    continue;
                size = rd32(sh + 20u);
                memcpy(saved + at, g_mem + rd32(sh + 16u), size);
                at += size;
            }
        }
        rc = place_segments(&img);
        if (rc != ZZ_OK) {
            free(saved);
            drop_arena();
            zzapp_loader_set_state(ZZAPP_ST_IDLE);
            return rc;
        }
        if (saved_n) {
            uint32_t at = 0;
            uint32_t r;
            uint32_t n = saved_n / 12u;

            for (r = 0; r < n; r++) {
                uint32_t roff = rd32(saved + at);
                uint32_t addend = rd32(saved + at + 8u);
                uint32_t place = addend + g_card;

                memcpy(g_mem + roff, &place, 4u);
                at += 12u;
            }
            free(saved);
        }
    }

    cache_image(&img);
    zzapp_loader_set_image(img.entry, img.image_end, img.stack_bytes,
                           img.note_flags);
    g_ready = 1;
    /* STARTING is published only by APP_START. */
    return ZZ_OK;
}

int zzapp_loader_abort(void)
{
    uint32_t state = zzapp_loader_state();

    if (state != ZZAPP_ST_LOADING && state != ZZAPP_ST_STARTING)
        return ZZ_EBUSY;
    drop_arena();
    zzapp_loader_set_state(ZZAPP_ST_IDLE);
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
    /* Same class cap as the arena. A live 96 MiB arena leaves no room;
     * fail closed rather than borrow the Fast RAM tail. */
    addr = pool_alloc(bytes, g_owner);
    if (!addr)
        return ZZ_ENOMEM;
    g_back = addr;
    g_back_owner = g_owner;
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
#endif
