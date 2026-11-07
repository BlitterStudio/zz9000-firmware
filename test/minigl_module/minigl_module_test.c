/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "card_pool.h"
#include "host_card_pool.h"
#include "memorymap.h"
#include "surface_allocator.h"
#include "zzm_manifest.h"
#include "zzm_loader.h"
#include "zzm_minigl_host.h"
#include "zzm_runtime.h"
#include "ff.h"
#include "zzapp.h"
#include "mw_ctx.h"


extern int zzm_module_entry(const zzm_services_t *services);
extern void zzm_module_exit(void);
extern mw_ctx *amiberry_transport_inproc_ctx(void);
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
extern int amiberry_flush(struct AmiberryContext *context);
extern void *mini_CurrentContext;


static int reject_module_open;
static uint8_t module_file[ZZM_V3_HEADER_SIZE + 32U];
static uint32_t module_offset;
static uint64_t now_us;
static uint32_t display_width = 640U;
static uint32_t display_height = 480U;
static uint32_t display_pitch = 640U * 4U;
static uint32_t display_format = 2U;
static uint32_t display_front = 0x08000000U;
static uint32_t display_back = 0x08000000U;
static uint32_t display_layout = 5U;
static uint32_t display_flags;
static int flip_idle = 1;
static int hold_flip;
static unsigned wait_yields;


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
{ (void)address; (void)length; }
void Xil_DCacheInvalidateRange(uint32_t address, uint32_t length)
{ (void)address; (void)length; }
void Xil_ICacheInvalidateRange(uint32_t address, uint32_t length)
{ (void)address; (void)length; }
void XTime_GetTime(uint64_t *time) { *time = now_us++; }
int sdk_aperture_runtime_is_zorro3(void) { return 1; }
int zzm_test_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfprintf(stderr, format, args);
    va_end(args);
    return result;
}

void zzapp_api_enter(void) {}
void zzapp_api_leave(void) {}
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
int zzapp_display_flip_bound(void)
{
    if (hold_flip)
        flip_idle = 0;
    return 0;
}
int zzapp_svc_display_flip(uint32_t surface_id)
{
    (void)surface_id;
    return zzapp_display_flip_bound();
}

int zzapp_svc_display_flip_async(uint32_t surface_id)
{
    return zzapp_svc_display_flip(surface_id);
}

int zzapp_display_flip_idle(void)
{
    return flip_idle;
}
uint32_t zzapp_services_flags(void) { return display_flags; }
int zzapp_services_stop_requested(void) { return 0; }
void zzapp_host_yield_wait(void)
{
    ++wait_yields;
    flip_idle = 1;
}

int zzm_registry_register_ops(uint32_t module_id, const void *ops, uint32_t bytes)
{
    return module_id == ZZM_MODULE_MINIGL && ops != NULL && bytes != 0U;
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
    if (module_offset > file->size || bytes > file->size - module_offset) {
        *read = 0U;
        return FR_NOT_READY;
    }
    memcpy(buffer, module_file + module_offset, bytes);
    module_offset += bytes;
    *read = bytes;
    return FR_OK;
}
FRESULT f_close(FIL *file) { (void)file; return FR_OK; }
void sdk_sha256(const uint8_t *data, uint32_t bytes, uint8_t digest[32])
{ (void)data; (void)bytes; memset(digest, 0, 32U); }

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
    header->image_size = 32U;
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
static void release_transport_from_worker_poll(void)
{
    assert(zzm_minigl_transport_release());
    zzm_loader_release(ZZM_MODULE_MINIGL);
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
    assert(zzm_loader_acquire(ZZM_MODULE_MINIGL));
    assert(zzm_minigl_worker_registered());
    ops = zzm_minigl_ops(&context);
    assert(ops != NULL && context != NULL);

    assert(zzm_minigl_transport_acquire(&generation1));
    assert(generation1 != 0U);
    assert(card_pool.block_count == 0U);
    assert(ops->open_68k(context, &request, &handle));
    ops->close_68k(context, handle);
    assert(ops->app_bind(context, 1U, &gl) == 0 && gl != NULL);
    /* Queued ARM presentation needs two retained frames plus one draw target. */
    assert(amiberry_transport_inproc_ctx()->state.target.buffer_count == 3U);
    {
        const zzapp_gl *draw = (const zzapp_gl *)gl;
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
    ops->app_unbind(context);
    display_width = 640U;
    display_height = 480U;
    display_pitch = 1280U;
    display_format = 1U;
    display_front = 0x08000000U;
    display_back = 0x08100000U;
    display_layout = 2U;
    display_flags = 1U << 15;
    assert(ops->app_bind_flags(context, 1U, ZZ_GL_BIND_DIRECT_PRESENT, &gl) == 0);
    assert(amiberry_transport_inproc_ctx()->state.target.buffer_count == 3U);
    assert(ops->app_swap(context) == 0);
    assert(ops->app_swap(context) == 0);
    ops->app_unbind(context);

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
    display_flags = 0U;
    display_format = 1U;
    display_layout = 1U;
    display_pitch = 8U;
    assert(ops->app_bind_flags(context, 1U, 0U, &gl) == 0);
    {
        const zzapp_gl *draw = (const zzapp_gl *)gl;
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

    display_flags = 0U;
    hold_flip = 1;
    flip_idle = 1;
    wait_yields = 0U;
    assert(ops->app_bind_flags(context, 1U, ZZ_GL_BIND_SYNC_PRESENT, &gl) == 0);
    assert(ops->app_swap(context) == 0);
    assert(wait_yields != 0U);
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
    assert(zzm_loader_acquire(ZZM_MODULE_MINIGL));
    ops = zzm_minigl_ops(&context);
    assert(ops != NULL && context != NULL);
    assert(ops->app_bind(context, 1U, &gl) == 0 && gl != NULL);
    ops->app_unbind(context);
    ops->app_reclaim(context, 1U);
    assert(card_pool.block_count == 0U);
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
    assert(zzm_loader_acquire(ZZM_MODULE_MINIGL));
    ops = zzm_minigl_ops(&context);
    assert(ops != NULL && context != NULL);
    assert(ops->app_bind(context, 1U, &gl) == 0 && gl != NULL);
    ops->app_unbind(context);
    ops->app_reclaim(context, 1U);
    zzm_loader_release(ZZM_MODULE_MINIGL);
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_AVAILABLE);
    assert(card_pool.class_used[CARD_POOL_CLASS_MINIGL_MODULE] == 0U);
    /*
     * The released slot is poison. A following SD-open failure must report
     * LOAD_FAILED without entering its stale fini entry.
     */
    reject_module_open = 1;
    assert(!zzm_loader_acquire(ZZM_MODULE_MINIGL));
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_LOAD_FAILED);
    assert(!zzm_minigl_worker_registered());
    assert(card_pool.block_count == 0U);
    puts("MiniGL real module loader/runtime integration passed");
    return 0;
}
