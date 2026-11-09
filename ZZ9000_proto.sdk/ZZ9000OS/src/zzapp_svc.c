/*
 * App SVC veneer tables and checked dispatch.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "zzapp_runtime.h"
#include "zzapp_svc.h"

#define L1_TYPE_MASK 3U
#define L1_SECTION 2U
#define L1_TABLE 1U
#define L1_AP_MASK (3U << 10)
#define L2_AP_MASK (3U << 4)
#define PAGE_BYTES 4096U
#define VENEER_BYTES 8U
#define VENEER_PAGE_BYTES 4096U
#define STRING_BYTES 512U
#define COPY_BYTES sizeof(zzapp_dirent)
#define API_FIRST offsetof(zzapp_api, exit)
#define GL_FIRST offsetof(zzapp_gl, Begin)
#define API_SLOT(field) ((uint32_t)((offsetof(zzapp_api, field) - API_FIRST) / sizeof(void *)))
#define GL_SLOT(field) ((uint32_t)((offsetof(zzapp_gl, field) - GL_FIRST) / sizeof(void *)))
#define API_COUNT ((uint32_t)((sizeof(zzapp_api) - API_FIRST) / sizeof(void *)))
#define GL_COUNT ((uint32_t)((sizeof(zzapp_gl) - GL_FIRST) / sizeof(void *)))

#ifdef ZZAPP_HOST_TEST
static uint8_t host_veneer_page[VENEER_PAGE_BYTES]
    __attribute__((aligned(VENEER_PAGE_BYTES)));
#define VENEER_PAGE host_veneer_page
#else
extern uint8_t zzapp_svc_veneer_page[];
#define VENEER_PAGE zzapp_svc_veneer_page
#endif

#ifdef ZZAPP_HOST_TEST
__attribute__((weak)) int zzapp_kernel_current_thread(void) { return -1; }
__attribute__((weak)) zzapp_proc_t *zzapp_proc_slot(uint32_t slot)
{ (void)slot; return NULL; }
__attribute__((weak)) const zzapp_api *zzapp_api_table(void) { return NULL; }
#endif

struct user_api_table {
    zzapp_api table;
    char build_id[128];
} __attribute__((aligned(VENEER_PAGE_BYTES)));
struct user_gl_table {
    zzapp_gl table;
    char build_id[128];
} __attribute__((aligned(VENEER_PAGE_BYTES)));
static struct user_api_table user_api[ZZAPP_AS_SLOT_COUNT];
static struct user_gl_table user_gl[ZZAPP_AS_SLOT_COUNT];
static const zzapp_gl *direct_gl[ZZAPP_AS_SLOT_COUNT];

#define P(a, d, l, n, e, c) { a, d, l, n, e, c }
#define N P(0U, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_FIXED, 0U, 0U, 0U)
#define D(a, r, p0, p1, p2) { a, 0U, r, { p0, p1, p2 } }
#define DF(a, v, r, p0, p1, p2) { a, v, r, { p0, p1, p2 } }
#define IN_FIXED(a, n, c) P(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_FIXED, 0U, n, c)
#define OUT_FIXED(a, n, c) P(a, ZZAPP_SVC_POINTER_OUT, ZZAPP_SVC_LENGTH_FIXED, 0U, n, c)
#define IO_FIXED(a, n, c) P(a, ZZAPP_SVC_POINTER_INOUT, ZZAPP_SVC_LENGTH_FIXED, 0U, n, c)
#define IN_ARG(a, n) P(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_ARGUMENT, n, 1U, 0U)
#define OUT_ARG(a, n) P(a, ZZAPP_SVC_POINTER_OUT, ZZAPP_SVC_LENGTH_ARGUMENT, n, 1U, 0U)
#define IN_COUNT(a, n, e) P(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_COUNT, n, e, 0U)
#define OUT_COUNT(a, n, e) P(a, ZZAPP_SVC_POINTER_OUT, ZZAPP_SVC_LENGTH_COUNT, n, e, 0U)
#define IN_STRING(a) P(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_STRING, 0U, 1U, 1U)
#define IN_PIXELS(a, n) P(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_GL_PIXELS, n, 0U, 0U)
#define OUT_PIXELS(a, n) P(a, ZZAPP_SVC_POINTER_OUT, ZZAPP_SVC_LENGTH_GL_PIXELS, n, 0U, 0U)
#define IN_INDICES(a, n) P(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_GL_INDICES, n, 0U, 0U)

/* The descriptors are the ABI audit: argument words include 64-bit values and
 * stack words, and every user pointer has its direction and extent. */
static const zzapp_svc_descriptor_t api_descriptors[API_COUNT] = {
    [API_SLOT(exit)] = D(1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(yield)] = D(0, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(should_stop)] = D(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(time_us)] = D(0, ZZAPP_SVC_RETURN_U64, N, N, N),
    [API_SLOT(sleep_us)] = D(1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(alloc)] = D(3, ZZAPP_SVC_RETURN_POINTER, N, N, N),
    [API_SLOT(free)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 1U, 0U), N, N),
    [API_SLOT(heap_range)] = D(2, ZZAPP_SVC_RETURN_VOID, OUT_FIXED(0, 4U, 1U), OUT_FIXED(1, 4U, 1U), N),
    [API_SLOT(log)] = D(2, ZZAPP_SVC_RETURN_VOID, IN_ARG(0, 1), N, N),
    [API_SLOT(file_open)] = D(2, ZZAPP_SVC_RETURN_INT, IN_STRING(0), N, N),
    [API_SLOT(file_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(file_read)] = D(4, ZZAPP_SVC_RETURN_INT, OUT_ARG(1, 2), OUT_FIXED(3, 4U, 1U), N),
    [API_SLOT(file_write)] = D(4, ZZAPP_SVC_RETURN_INT, IN_ARG(1, 2), OUT_FIXED(3, 4U, 1U), N),
    [API_SLOT(file_seek)] = D(6, ZZAPP_SVC_RETURN_INT, OUT_FIXED(5, 8U, 1U), N, N),
    [API_SLOT(file_stat)] = D(2, ZZAPP_SVC_RETURN_INT, IN_STRING(0), OUT_FIXED(1, sizeof(zzapp_stat), 1U), N),
    [API_SLOT(file_list_open)] = D(1, ZZAPP_SVC_RETURN_INT, IN_STRING(0), N, N),
    [API_SLOT(file_list_next)] = D(2, ZZAPP_SVC_RETURN_INT, OUT_FIXED(1, sizeof(zzapp_dirent), 1U), N, N),
    [API_SLOT(file_list_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_open)] = D(5, ZZAPP_SVC_RETURN_INT, OUT_FIXED(4, sizeof(zzapp_surface), 1U), N, N),
    [API_SLOT(display_flip)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_vsync)] = D(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(input_poll)] = D(3, ZZAPP_SVC_RETURN_INT, OUT_COUNT(0, 1, sizeof(zzapp_event)), OUT_FIXED(2, 4U, 1U), N),
    [API_SLOT(audio_open)] = D(3, ZZAPP_SVC_RETURN_INT, OUT_FIXED(2, sizeof(zzapp_audio), 1U), N, N),
    [API_SLOT(audio_close)] = D(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(gl_bind)] = D(1, ZZAPP_SVC_RETURN_POINTER, N, N, N),
    [API_SLOT(gl_swap)] = D(0, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(gl_unbind)] = D(0, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(display_flip_async)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_flip_idle)] = D(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_refresh)] = D(1, ZZAPP_SVC_RETURN_INT, IO_FIXED(0, sizeof(zzapp_surface), 1U), N, N),
    [API_SLOT(gl_bind_flags)] = D(2, ZZAPP_SVC_RETURN_POINTER, N, N, N),
    [API_SLOT(display_set_palette)] = D(4, ZZAPP_SVC_RETURN_INT, IN_COUNT(3, 2, 4U), N, N),
    [API_SLOT(display_mailbox_replaced)] = D(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(host_address)] = D(2, ZZAPP_SVC_RETURN_INT, IN_FIXED(0, 1U, 0U), OUT_FIXED(1, 4U, 1U), N),
    [API_SLOT(mem_info)] = D(1, ZZAPP_SVC_RETURN_INT, OUT_FIXED(0, sizeof(zzapp_mem_info), 1U), N, N),
    [API_SLOT(map_file)] = D(3, ZZAPP_SVC_RETURN_INT, IN_STRING(0), OUT_FIXED(1, 4U, 1U), OUT_FIXED(2, 4U, 1U)),
    [API_SLOT(unmap_file)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 1U, 0U), N, N),
    [API_SLOT(sha256)] = D(3, ZZAPP_SVC_RETURN_INT, IN_ARG(0, 1), OUT_FIXED(2, 32U, 1U), N),
    [API_SLOT(decompress)] = D(7, ZZAPP_SVC_RETURN_INT, IN_ARG(2, 3), OUT_ARG(4, 5), OUT_FIXED(6, sizeof(zzapp_service_result), 1U)),
    [API_SLOT(image_open)] = D(3, ZZAPP_SVC_RETURN_INT, IN_FIXED(0, sizeof(zzapp_image_open), 1U), OUT_FIXED(1, 4U, 1U), OUT_FIXED(2, sizeof(zzapp_image_result), 1U)),
    [API_SLOT(image_decode)] = D(5, ZZAPP_SVC_RETURN_INT, IN_ARG(1, 2), OUT_FIXED(4, sizeof(zzapp_image_result), 1U), N),
    [API_SLOT(image_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(audio_decode_open)] = D(2, ZZAPP_SVC_RETURN_INT, OUT_FIXED(1, 4U, 1U), N, N),
    [API_SLOT(audio_decode)] = D(7, ZZAPP_SVC_RETURN_INT, IN_ARG(1, 2), OUT_COUNT(4, 5, 4U), OUT_FIXED(6, sizeof(zzapp_audio_decode_result), 1U)),
    [API_SLOT(audio_decode_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(console_write)] = D(3, ZZAPP_SVC_RETURN_INT, IN_ARG(0, 1), OUT_FIXED(2, 4U, 1U), N),
    [API_SLOT(console_read)] = D(3, ZZAPP_SVC_RETURN_INT, OUT_ARG(0, 1), OUT_FIXED(2, 4U, 1U), N),
    [API_SLOT(msg_send)] = D(1, ZZAPP_SVC_RETURN_INT, IN_FIXED(0, 32U, 1U), N, N),
    [API_SLOT(msg_poll)] = D(1, ZZAPP_SVC_RETURN_INT, OUT_FIXED(0, 32U, 1U), N, N),
    [API_SLOT(heap_grow)] = D(2, ZZAPP_SVC_RETURN_INT, OUT_FIXED(1, 4U, 1U), N, N),
    [API_SLOT(heap_shrink)] = D(2, ZZAPP_SVC_RETURN_INT, OUT_FIXED(1, 4U, 1U), N, N),
    [API_SLOT(heap_info)] = D(1, ZZAPP_SVC_RETURN_INT, OUT_FIXED(0, sizeof(zzapp_heap_info), 1U), N, N)
};

#define G0(f, r) [GL_SLOT(f)] = D(0, r, N, N, N)
#define G1(f, r) [GL_SLOT(f)] = D(1, r, N, N, N)
#define G2(f, r) [GL_SLOT(f)] = D(2, r, N, N, N)
#define G3(f, r) [GL_SLOT(f)] = D(3, r, N, N, N)
#define G4(f, r) [GL_SLOT(f)] = D(4, r, N, N, N)
#define G6(f, r) [GL_SLOT(f)] = D(6, r, N, N, N)
#define G8(f, r) [GL_SLOT(f)] = D(8, r, N, N, N)
#define G9(f, r) [GL_SLOT(f)] = D(9, r, N, N, N)
static const zzapp_svc_descriptor_t gl_descriptors[GL_COUNT] = {
    G1(Begin, ZZAPP_SVC_RETURN_VOID), G0(End, ZZAPP_SVC_RETURN_VOID),
    DF(0, 2, ZZAPP_SVC_RETURN_VOID, N, N, N), DF(0, 3, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(Vertex3fv)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 12U, 1U), N, N),
    DF(0, 2, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(TexCoord2fv)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 8U, 1U), N, N),
    DF(0, 3, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(Color3fv)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 12U, 1U), N, N),
    [GL_SLOT(Color3ubv)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 3U, 1U), N, N),
    DF(0, 4, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(Color4fv)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 16U, 1U), N, N),
    G4(Color4ub, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(Color4ubv)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 4U, 1U), N, N),
    G1(Enable, ZZAPP_SVC_RETURN_VOID), G1(Disable, ZZAPP_SVC_RETURN_VOID),
    G1(IsEnabled, ZZAPP_SVC_RETURN_INT), DF(1, 1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    G2(BlendFunc, ZZAPP_SVC_RETURN_VOID), G1(DepthFunc, ZZAPP_SVC_RETURN_VOID),
    G1(DepthMask, ZZAPP_SVC_RETURN_VOID), DF(0, 4, ZZAPP_SVC_RETURN_VOID, N, N, N),
    G1(CullFace, ZZAPP_SVC_RETURN_VOID), G1(FrontFace, ZZAPP_SVC_RETURN_VOID),
    G1(ShadeModel, ZZAPP_SVC_RETURN_VOID), G2(PolygonMode, ZZAPP_SVC_RETURN_VOID),
    G1(Clear, ZZAPP_SVC_RETURN_VOID), DF(0, 4, ZZAPP_SVC_RETURN_VOID, N, N, N),
    G4(Viewport, ZZAPP_SVC_RETURN_VOID), G4(Scissor, ZZAPP_SVC_RETURN_VOID),
    G1(MatrixMode, ZZAPP_SVC_RETURN_VOID), G0(LoadIdentity, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(LoadMatrixf)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 64U, 1U), N, N),
    G0(PushMatrix, ZZAPP_SVC_RETURN_VOID), G0(PopMatrix, ZZAPP_SVC_RETURN_VOID),
    DF(0, 4, ZZAPP_SVC_RETURN_VOID, N, N, N), DF(0, 3, ZZAPP_SVC_RETURN_VOID, N, N, N),
    DF(0, 3, ZZAPP_SVC_RETURN_VOID, N, N, N), DF(0, 12, ZZAPP_SVC_RETURN_VOID, N, N, N),
    DF(0, 12, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(TexImage2D)] = D(9, ZZAPP_SVC_RETURN_VOID, IN_PIXELS(8, 3), N, N),
    [GL_SLOT(TexSubImage2D)] = D(9, ZZAPP_SVC_RETURN_VOID, IN_PIXELS(8, 4), N, N),
    G3(TexParameteri, ZZAPP_SVC_RETURN_VOID), DF(2, 1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    G3(TexEnvi, ZZAPP_SVC_RETURN_VOID), DF(2, 1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(GenTextures)] = D(2, ZZAPP_SVC_RETURN_VOID, OUT_COUNT(1, 0, 4U), N, N),
    [GL_SLOT(DeleteTextures)] = D(2, ZZAPP_SVC_RETURN_VOID, IN_COUNT(1, 0, 4U), N, N),
    G2(BindTexture, ZZAPP_SVC_RETURN_VOID), G0(Finish, ZZAPP_SVC_RETURN_VOID),
    G0(Flush, ZZAPP_SVC_RETURN_VOID), G1(DrawBuffer, ZZAPP_SVC_RETURN_VOID),
    G1(ReadBuffer, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(ReadPixels)] = D(7, ZZAPP_SVC_RETURN_VOID, OUT_PIXELS(6, 2), N, N),
    G1(GetString, ZZAPP_SVC_RETURN_POINTER),
    [GL_SLOT(GetIntegerv)] = D(2, ZZAPP_SVC_RETURN_VOID, OUT_FIXED(1, 4U, 0U), N, N),
    [GL_SLOT(GetFloatv)] = D(2, ZZAPP_SVC_RETURN_VOID, OUT_FIXED(1, 4U, 0U), N, N),
    G0(GetError, ZZAPP_SVC_RETURN_INT), G2(Hint, ZZAPP_SVC_RETURN_VOID),
    G2(PixelStorei, ZZAPP_SVC_RETURN_VOID), DF(0, 8, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(Build2DMipmaps)] = D(7, ZZAPP_SVC_RETURN_INT, IN_PIXELS(6, 2), N, N),
    [GL_SLOT(VertexPointer)] = D(4, ZZAPP_SVC_RETURN_VOID, IN_FIXED(3, 1U, 0U), N, N),
    [GL_SLOT(TexCoordPointer)] = D(4, ZZAPP_SVC_RETURN_VOID, IN_FIXED(3, 1U, 0U), N, N),
    [GL_SLOT(ColorPointer)] = D(4, ZZAPP_SVC_RETURN_VOID, IN_FIXED(3, 1U, 0U), N, N),
    G1(EnableClientState, ZZAPP_SVC_RETURN_VOID), G1(DisableClientState, ZZAPP_SVC_RETURN_VOID),
    G1(ClientActiveTextureARB, ZZAPP_SVC_RETURN_VOID), G3(DrawArrays, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(DrawElements)] = D(4, ZZAPP_SVC_RETURN_VOID, IN_INDICES(3, 1), N, N),
    G2(LockArrays, ZZAPP_SVC_RETURN_VOID), G0(UnlockArrays, ZZAPP_SVC_RETURN_VOID)
};

uint32_t zzapp_svc_api_count(void) { return API_COUNT; }
uint32_t zzapp_svc_gl_count(void) { return GL_COUNT; }
const zzapp_svc_descriptor_t *zzapp_svc_api_descriptor(uint32_t slot)
{ return slot < API_COUNT && slot != API_SLOT(build_id) ? &api_descriptors[slot] : NULL; }
const zzapp_svc_descriptor_t *zzapp_svc_gl_descriptor(uint32_t slot)
{ return slot < GL_COUNT && slot != GL_SLOT(build_id) ? &gl_descriptors[slot] : NULL; }

uintptr_t zzapp_svc_api_veneer(uint32_t slot)
{ return slot < API_COUNT ? (uintptr_t)VENEER_PAGE + slot * VENEER_BYTES : 0U; }
uintptr_t zzapp_svc_gl_veneer(uint32_t slot)
{ return slot < GL_COUNT ? (uintptr_t)VENEER_PAGE + (ZZAPP_SVC_GL_FIRST + slot) * VENEER_BYTES : 0U; }
int zzapp_svc_is_veneer(uintptr_t address)
{
    uintptr_t first = (uintptr_t)VENEER_PAGE;
    uintptr_t end = first + (ZZAPP_SVC_GL_FIRST + GL_COUNT) * VENEER_BYTES;
    return address >= first && address < end && (address - first) % VENEER_BYTES == 0U;
}

static void copy_build_id(char *destination, const char *source)
{
    uint32_t i;
    if (!source) { destination[0] = '\0'; return; }
    for (i = 0U; i + 1U < 128U && source[i]; ++i)
        destination[i] = source[i];
    destination[i] = '\0';
}

int zzapp_svc_copy_api(zzapp_api *destination, const zzapp_api *source)
{
    uintptr_t *entries;
    uint32_t i;
    if (!destination || !source || source->size > sizeof(*destination) || source->size < API_FIRST)
        return ZZ_EINVAL;
    memcpy(destination, source, source->size);
    entries = (uintptr_t *)(void *)((uint8_t *)destination + API_FIRST);
    for (i = 0U; i < API_COUNT; ++i)
        if (i != API_SLOT(build_id)) entries[i] = zzapp_svc_api_veneer(i);
    copy_build_id(user_api[0].build_id, source->build_id);
    destination->build_id = user_api[0].build_id;
    return ZZ_OK;
}
const zzapp_api *zzapp_svc_api_for_slot(uint32_t slot, const zzapp_api *source)
{
    struct user_api_table *table;
    uintptr_t *entries;
    uint32_t i;
    if (slot >= ZZAPP_AS_SLOT_COUNT || !source || source->size > sizeof(*source) || source->size < API_FIRST)
        return NULL;
    table = &user_api[slot];
    memcpy(&table->table, source, source->size);
    entries = (uintptr_t *)(void *)((uint8_t *)&table->table + API_FIRST);
    for (i = 0U; i < API_COUNT; ++i)
        if (i != API_SLOT(build_id)) entries[i] = zzapp_svc_api_veneer(i);
    copy_build_id(table->build_id, source->build_id);
    table->table.build_id = table->build_id;
    return &table->table;
}
zzapp_gl *zzapp_svc_gl_for_slot(uint32_t slot)
{ return slot < ZZAPP_AS_SLOT_COUNT ? &user_gl[slot].table : NULL; }
int zzapp_svc_copy_gl(zzapp_gl *destination, const zzapp_gl *source)
{
    uintptr_t *entries;
    uint32_t i;
    if (!destination || !source || source->size > sizeof(*destination) || source->size < GL_FIRST)
        return ZZ_EINVAL;
    memcpy(destination, source, source->size);
    entries = (uintptr_t *)(void *)((uint8_t *)destination + GL_FIRST);
    for (i = 0U; i < GL_COUNT; ++i)
        if (i != GL_SLOT(build_id)) entries[i] = zzapp_svc_gl_veneer(i);
    destination->build_id = NULL;
    return ZZ_OK;
}

static int section_allows(uint32_t descriptor, int write)
{
    uint32_t ap = (descriptor & L1_AP_MASK) >> 10;
    return (descriptor & L1_TYPE_MASK) == L1_SECTION && (ap == 3U || (!write && ap == 2U));
}
static int page_allows(uint32_t descriptor, int write)
{
    uint32_t ap = (descriptor & L2_AP_MASK) >> 4;
    return (descriptor & 2U) != 0U && (ap == 3U || (!write && ap == 2U));
}
int zzapp_svc_check_user(const zzapp_as_t *as, uint32_t address, uint32_t bytes, int write)
{
    uint64_t end;
    uint64_t page;
    if (!as || !as->active) return ZZ_EFAULT;
    if (!bytes) return ZZ_OK;
    end = (uint64_t)address + bytes;
    if (end > UINT32_MAX + 1ULL) return ZZ_EFAULT;
    for (page = address & ~(uint64_t)(PAGE_BYTES - 1U); page < end; page += PAGE_BYTES) {
        uint32_t address_page = (uint32_t)page;
        uint32_t descriptor = as->l1[address_page >> 20];
        if ((descriptor & L1_TYPE_MASK) == L1_TABLE) {
            const uint32_t *l2 = zzapp_as_l2_for(as, address_page);
            if (!l2 || !page_allows(l2[(address_page >> 12) & 0xffU], write)) return ZZ_EFAULT;
        } else if (!section_allows(descriptor, write)) return ZZ_EFAULT;
    }
    return ZZ_OK;
}

static int stack_word(const zzapp_as_t *as, uint32_t stack, uint32_t index, uint32_t *value)
{
    uint32_t address = stack + index * 4U;
    if (!value || address < stack || zzapp_svc_check_user(as, address, 4U, 0) != ZZ_OK) return ZZ_EFAULT;
    *value = *(const uint32_t *)(uintptr_t)address;
    return ZZ_OK;
}
static int frame_arguments(const zzapp_as_t *as, const zzapp_arm_frame_t *frame,
                           uint32_t words, uint32_t args[9])
{
    uint32_t i;
    if (words > 9U) return ZZ_EFAULT;
    for (i = 0U; i < words && i < 4U; ++i) args[i] = frame->r[i];
    for (; i < words; ++i)
        if (stack_word(as, frame->user_sp, i - 4U, &args[i]) != ZZ_OK) return ZZ_EFAULT;
    return ZZ_OK;
}
static int gl_pixel_bytes(const uint32_t args[9], uint32_t first,
                          uint32_t *bytes)
{
    uint32_t components;
    uint32_t element;
    uint32_t format = args[first + 3U];
    uint32_t type = args[first + 4U];
    uint64_t pixels;
    uint64_t length;
    int32_t width = (int32_t)args[first];
    int32_t height = (int32_t)args[first + 1U];

    if (width <= 0 || height <= 0) {
        *bytes = 0U;
        return ZZ_OK;
    }
    switch (format) {
    case 0x1903U: case 0x1906U: case 0x1909U: components = 1U; break;
    case 0x190aU: components = 2U; break;
    case 0x1907U: case 0x80e0U: components = 3U; break;
    case 0x1908U: case 0x80e1U: components = 4U; break;
    default: return ZZ_EFAULT;
    }
    switch (type) {
    case 0x1400U: case 0x1401U: element = 1U; break;
    case 0x1402U: case 0x1403U: element = 2U; break;
    case 0x1404U: case 0x1405U: case 0x1406U: element = 4U; break;
    case 0x8033U: case 0x8363U: components = 1U; element = 2U; break;
    case 0x8034U: case 0x8035U: case 0x8367U: components = 1U; element = 4U; break;
    default: return ZZ_EFAULT;
    }
    pixels = (uint64_t)(uint32_t)width * (uint32_t)height;
    length = pixels * components * element;
    if (length > UINT32_MAX) return ZZ_EFAULT;
    *bytes = (uint32_t)length;
    return ZZ_OK;
}
static int gl_indices_bytes(const uint32_t args[9], uint32_t first,
                            uint32_t *bytes)
{
    uint32_t element;
    uint64_t length;
    int32_t count = (int32_t)args[first];

    if (count <= 0) {
        *bytes = 0U;
        return ZZ_OK;
    }
    switch (args[first + 1U]) {
    case 0x1401U: element = 1U; break;
    case 0x1403U: element = 2U; break;
    case 0x1405U: element = 4U; break;
    default: return ZZ_EFAULT;
    }
    length = (uint64_t)(uint32_t)count * element;
    if (length > UINT32_MAX) return ZZ_EFAULT;
    *bytes = (uint32_t)length;
    return ZZ_OK;
}
static int pointer_bytes(const zzapp_svc_pointer_t *pointer, const uint32_t args[9],
                         uint32_t *bytes)
{
    uint64_t length;

    if (pointer->length == ZZAPP_SVC_LENGTH_FIXED) length = pointer->element_bytes;
    else if (pointer->length == ZZAPP_SVC_LENGTH_ARGUMENT) length = args[pointer->length_argument];
    else if (pointer->length == ZZAPP_SVC_LENGTH_COUNT)
        length = (uint64_t)args[pointer->length_argument] * pointer->element_bytes;
    else if (pointer->length == ZZAPP_SVC_LENGTH_GL_PIXELS)
        return gl_pixel_bytes(args, pointer->length_argument, bytes);
    else if (pointer->length == ZZAPP_SVC_LENGTH_GL_INDICES)
        return gl_indices_bytes(args, pointer->length_argument, bytes);
    else return ZZ_OK;
    if (length > UINT32_MAX) return ZZ_EFAULT;
    *bytes = (uint32_t)length;
    return ZZ_OK;
}
static int copy_string(const zzapp_as_t *as, uint32_t address, char destination[STRING_BYTES])
{
    uint32_t i;
    if (!address) return ZZ_EFAULT;
    for (i = 0U; i < STRING_BYTES; ++i) {
        if (zzapp_svc_check_user(as, address + i, 1U, 0) != ZZ_OK) return ZZ_EFAULT;
        destination[i] = *(const char *)(uintptr_t)(address + i);
        if (!destination[i]) return ZZ_OK;
    }
    return ZZ_EFAULT;
}
typedef uint32_t (*word_call_t)(uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static uint32_t call_words(uintptr_t address, const uint32_t args[9])
{
    word_call_t call = (word_call_t)address;
    return call(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
}
static int entry_at(const void *table, uint32_t size, uint32_t first, uint32_t slot,
                    uintptr_t *entry)
{
    uint64_t end = (uint64_t)first + ((uint64_t)slot + 1U) * sizeof(void *);
    if (!table || end > size) return ZZ_ENOSYS;
    *entry = *(const uintptr_t *)(const void *)((const uint8_t *)table + first + slot * sizeof(void *));
    return *entry ? ZZ_OK : ZZ_ENOSYS;
}

static int prepare_pointers(const zzapp_as_t *as, const zzapp_svc_descriptor_t *descriptor,
                            uint32_t args[9], uint8_t copies[3][COPY_BYTES],
                            char strings[3][STRING_BYTES], uint32_t originals[3])
{
    uint32_t i;
    for (i = 0U; i < 3U; ++i) {
        const zzapp_svc_pointer_t *pointer = &descriptor->pointers[i];
        uint32_t bytes = 0U;
        int write;
        if (!pointer->element_bytes) continue;
        originals[i] = args[pointer->argument];
        write = pointer->direction != ZZAPP_SVC_POINTER_IN;
        if (pointer->length == ZZAPP_SVC_LENGTH_STRING) {
            if (copy_string(as, args[pointer->argument], strings[i]) != ZZ_OK) return ZZ_EFAULT;
            args[pointer->argument] = (uint32_t)(uintptr_t)strings[i];
            continue;
        }
        if (pointer_bytes(pointer, args, &bytes) != ZZ_OK ||
            (!args[pointer->argument] && bytes) ||
            zzapp_svc_check_user(as, args[pointer->argument], bytes, write) != ZZ_OK) return ZZ_EFAULT;
        if (pointer->copy) {
            if (bytes > COPY_BYTES) return ZZ_EFAULT;
            if (pointer->direction != ZZAPP_SVC_POINTER_OUT)
                memcpy(copies[i], (const void *)(uintptr_t)args[pointer->argument], bytes);
            args[pointer->argument] = (uint32_t)(uintptr_t)copies[i];
        }
    }
    return ZZ_OK;
}
static void finish_pointers(const zzapp_svc_descriptor_t *descriptor, const uint32_t args[9],
                            uint8_t copies[3][COPY_BYTES], const uint32_t originals[3])
{
    uint32_t i;
    for (i = 0U; i < 3U; ++i) {
        const zzapp_svc_pointer_t *pointer = &descriptor->pointers[i];
        uint32_t bytes = 0U;
        if (!pointer->element_bytes || !pointer->copy || pointer->direction == ZZAPP_SVC_POINTER_IN ||
            pointer_bytes(pointer, args, &bytes) != ZZ_OK) continue;
        memcpy((void *)(uintptr_t)originals[i], copies[i], bytes);
    }
}

static uint32_t float_word(const zzapp_arm_frame_t *frame, uint32_t index)
{ return frame->vfp[32U + index]; }
static float float_argument(const zzapp_arm_frame_t *frame, uint32_t index)
{ float value; uint32_t word = float_word(frame, index); memcpy(&value, &word, sizeof(value)); return value; }
static double double_argument(const zzapp_arm_frame_t *frame, uint32_t index)
{ double value; uint32_t words[2] = { float_word(frame, index * 2U), float_word(frame, index * 2U + 1U) }; memcpy(&value, words, sizeof(value)); return value; }
static int call_gl_float(const zzapp_gl *gl, uint32_t slot, const zzapp_arm_frame_t *frame,
                         const uint32_t args[9], uint32_t *result)
{
    float a = float_argument(frame, 0U), b = float_argument(frame, 1U);
    float c = float_argument(frame, 2U), d = float_argument(frame, 3U);
    if (slot == GL_SLOT(Vertex2f)) gl->Vertex2f(a, b);
    else if (slot == GL_SLOT(Vertex3f)) gl->Vertex3f(a, b, c);
    else if (slot == GL_SLOT(TexCoord2f)) gl->TexCoord2f(a, b);
    else if (slot == GL_SLOT(Color3f)) gl->Color3f(a, b, c);
    else if (slot == GL_SLOT(Color4f)) gl->Color4f(a, b, c, d);
    else if (slot == GL_SLOT(AlphaFunc)) gl->AlphaFunc(args[0], a);
    else if (slot == GL_SLOT(DepthRange)) gl->DepthRange(double_argument(frame, 0U), double_argument(frame, 1U));
    else if (slot == GL_SLOT(ClearColor)) gl->ClearColor(a, b, c, d);
    else if (slot == GL_SLOT(Rotatef)) gl->Rotatef(a, b, c, d);
    else if (slot == GL_SLOT(Translatef)) gl->Translatef(a, b, c);
    else if (slot == GL_SLOT(Scalef)) gl->Scalef(a, b, c);
    else if (slot == GL_SLOT(Ortho)) gl->Ortho(double_argument(frame, 0U), double_argument(frame, 1U), double_argument(frame, 2U), double_argument(frame, 3U), double_argument(frame, 4U), double_argument(frame, 5U));
    else if (slot == GL_SLOT(Frustum)) gl->Frustum(double_argument(frame, 0U), double_argument(frame, 1U), double_argument(frame, 2U), double_argument(frame, 3U), double_argument(frame, 4U), double_argument(frame, 5U));
    else if (slot == GL_SLOT(TexParameterf)) gl->TexParameterf(args[0], args[1], a);
    else if (slot == GL_SLOT(TexEnvf)) gl->TexEnvf(args[0], args[1], a);
    else if (slot == GL_SLOT(Perspective)) gl->Perspective(double_argument(frame, 0U), double_argument(frame, 1U), double_argument(frame, 2U), double_argument(frame, 3U));
    else return 0;
    *result = 0U;
    return 1;
}

static int dispatch_bind(const zzapp_api *api, uint32_t slot, const uint32_t args[9], int flags, uint32_t *result)
{
    const zzapp_gl *gl = flags ? api->gl_bind_flags(args[0], args[1]) :
                                 api->gl_bind(args[0]);
    if (!gl || slot >= ZZAPP_AS_SLOT_COUNT ||
        zzapp_svc_copy_gl(&user_gl[slot].table, gl) != ZZ_OK) {
        if (slot < ZZAPP_AS_SLOT_COUNT)
            direct_gl[slot] = NULL;
        *result = 0U;
        return ZZ_OK;
    }
    direct_gl[slot] = gl;
    copy_build_id(user_gl[slot].build_id, gl->build_id);
    user_gl[slot].table.build_id = user_gl[slot].build_id;
    *result = (uint32_t)(uintptr_t)&user_gl[slot].table;
    return ZZ_OK;
}
static int dispatch_api(const zzapp_api *api, uint32_t slot, const zzapp_arm_frame_t *frame,
                        const zzapp_svc_descriptor_t *descriptor, uint32_t number,
                        uint32_t args[9], uint32_t *result)
{
    (void)descriptor;
    uintptr_t entry;
    if (number == API_SLOT(gl_bind)) return dispatch_bind(api, slot, args, 0, result);
    if (number == API_SLOT(gl_bind_flags)) return dispatch_bind(api, slot, args, 1, result);
    if (number == API_SLOT(time_us)) {
        uint64_t value = api->time_us();

        *result = (uint32_t)value;
        ((zzapp_arm_frame_t *)frame)->r[1] = (uint32_t)(value >> 32);
        return ZZ_OK;
    }
    if (entry_at(api, api->size, API_FIRST, number, &entry) != ZZ_OK)
        return ZZ_ENOSYS;
    *result = call_words(entry, args);
    if (number == API_SLOT(gl_unbind) && slot < ZZAPP_AS_SLOT_COUNT)
        direct_gl[slot] = NULL;
    return ZZ_OK;
}
static int dispatch_gl(uint32_t slot, const zzapp_arm_frame_t *frame,
                       uint32_t number, const uint32_t args[9], uint32_t *result)
{
    const zzapp_gl *gl = slot < ZZAPP_AS_SLOT_COUNT ? direct_gl[slot] : NULL;
    uintptr_t entry;
    if (!gl) return ZZ_ENOSYS;
    if (number == GL_SLOT(GetString)) {
        const uint8_t *string = gl->GetString(args[0]);
        if (!string) { *result = 0U; return ZZ_OK; }
        copy_build_id(user_gl[slot].build_id, (const char *)string);
        *result = (uint32_t)(uintptr_t)user_gl[slot].build_id;
        return ZZ_OK;
    }
    if (call_gl_float(gl, number, frame, args, result)) return ZZ_OK;
    if (entry_at(gl, gl->size, GL_FIRST, number, &entry) != ZZ_OK) return ZZ_ENOSYS;
    *result = call_words(entry, args);
    return ZZ_OK;
}

void zzapp_svc_dispatch(zzapp_arm_frame_t *frame)
{
    const zzapp_api *api;
    const zzapp_svc_descriptor_t *descriptor;
    zzapp_proc_t *proc;
    uint32_t instruction, number, slot, result = 0U, args[9] = { 0U };
    uint8_t copies[3][COPY_BYTES];
    char strings[3][STRING_BYTES];
    uint32_t originals[3] = { 0U };
    int gl;
    if (!frame) return;
    slot = zzapp_kernel_current_thread() >= 0 ? (uint32_t)zzapp_kernel_current_thread() : 0U;
    proc = zzapp_proc_slot(slot);
    api = zzapp_api_table();
    if (!proc || !api || zzapp_svc_check_user(&proc->as, frame->exception_lr - 4U, 4U, 0) != ZZ_OK) { frame->r[0] = (uint32_t)ZZ_EFAULT; return; }
    instruction = *(const uint32_t *)(uintptr_t)(frame->exception_lr - 4U);
    if ((instruction & 0xff000000U) != 0xef000000U) { frame->r[0] = (uint32_t)ZZ_EINVAL; return; }
    number = instruction & 0x00ffffffU;
    gl = number >= ZZAPP_SVC_GL_FIRST;
    number -= gl ? ZZAPP_SVC_GL_FIRST : 0U;
    descriptor = gl ? zzapp_svc_gl_descriptor(number) : zzapp_svc_api_descriptor(number);
    if (!descriptor) { frame->r[0] = (uint32_t)ZZ_ENOSYS; return; }
    if (frame_arguments(&proc->as, frame, descriptor->arguments, args) != ZZ_OK ||
        prepare_pointers(&proc->as, descriptor, args, copies, strings, originals) != ZZ_OK) { frame->r[0] = (uint32_t)ZZ_EFAULT; return; }
    if (!gl && number == API_SLOT(image_open)) {
        const zzapp_image_open *open = (const zzapp_image_open *)(const void *)copies[0];

        if (zzapp_svc_check_user(&proc->as, (uint32_t)(uintptr_t)open->tile,
                                 open->tile_bytes, 1) != ZZ_OK) {
            frame->r[0] = (uint32_t)ZZ_EFAULT;
            return;
        }
    }
    if ((gl ? dispatch_gl(slot, frame, number, args, &result) : dispatch_api(api, slot, frame, descriptor, number, args, &result)) != ZZ_OK) { frame->r[0] = (uint32_t)ZZ_ENOSYS; return; }
    finish_pointers(descriptor, args, copies, originals);
    if (descriptor->result != ZZAPP_SVC_RETURN_VOID) frame->r[0] = result;
}
