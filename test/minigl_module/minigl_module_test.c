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

extern int zzm_module_entry(const zzm_services_t *services);
extern void zzm_module_exit(void);

static uint8_t module_file[ZZM_V3_HEADER_SIZE + 32U];
static uint32_t module_offset;
static uint64_t now_us;


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
    *width = 640U;
    *height = 480U;
    *pitch = 640U * 4U;
    *format = 2U;
    *front = 0x08000000U;
    *back = 0x08000000U;
    return 0;
}
int zzapp_display_flip_bound(void) { return 0; }
int zzapp_svc_display_flip(uint32_t surface_id)
{
    (void)surface_id;
    return zzapp_display_flip_bound();
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
    if (strstr(path, "/MINIGL.ZZM") == NULL)
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
        free(pixels);
    }
    assert(ops->app_swap(context) == 0);
    assert(card_pool.class_used[CARD_POOL_CLASS_MINIGL_CONTEXT] >= (128U << 20));
    ops->app_unbind(context);
    ops->app_reclaim(context, 1U);
    assert(zzm_minigl_begin_reset() == ZZM_MINIGL_RESET_QUIESCENT);
    assert(zzm_minigl_reset_poll() == ZZM_MINIGL_RESET_QUIESCENT);
    assert(card_pool.block_count == 0U);
    assert(zzm_minigl_transport_acquire(&generation2));
    assert(generation2 > generation1);
    assert(zzm_minigl_transport_release());
    assert(card_pool.block_count == 0U);
    zzm_loader_release(ZZM_MODULE_MINIGL);
    assert(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    assert(status.state == ZZM_STATE_AVAILABLE);
    assert(!zzm_minigl_worker_registered());
    assert(card_pool.block_count == 0U);
    puts("MiniGL real module loader/runtime integration passed");
    return 0;
}
