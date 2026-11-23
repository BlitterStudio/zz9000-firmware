#include <sys/mman.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdk_crypto.h"
#include "sdk_image_stream.h"
#include "sdk_video_stream.h"
#include "zzm_loader.h"
#include "zzm_manifest.h"
#include "zzm_runtime.h"
#include "zzm_registry.h"
#include "ff.h"

/* Host backing for the loader's cross-core session lock. The test picks the
 * "core" that runs next; a raw spin on a held lock would hang the real
 * firmware, so the mock counts it instead of looping. */
static int mock_cpu;
static unsigned mock_blocked_spins;
int smp_cpu_id(void) { return mock_cpu; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t flags) { (void)flags; }
void smp_raw_spin_lock(volatile uint32_t *word)
{
    if (*word)
        mock_blocked_spins++;
    *word = 1U;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0U; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word) return 0;
    *word = 1U;
    return 1;
}
static unsigned loader_open_calls;
static unsigned loader_close_calls;
static FRESULT loader_open_result;
struct loader_file {
    const uint8_t *data;
    uint32_t size;
};
static struct loader_file loader_files[8];
static uint32_t loader_file_offset;
static int loader_minigl_file_only;
static uint32_t loader_read_limit = UINT32_MAX;
static uint32_t loader_fail_offset = UINT32_MAX;

static int loader_file_index(const char *path)
{
    uint32_t i;

    for (i = 0U; i < zzm_slot_count; i++)
        if (strstr(path, zzm_slots[i].name))
            return (int)i;
    return -1;
}

static void set_loader_module_file(uint32_t module_id, const uint8_t *data,
                                   uint32_t size)
{
    uint32_t i;

    for (i = 0U; i < zzm_slot_count; i++)
        if (zzm_slots[i].id == module_id) {
            loader_files[i].data = data;
            loader_files[i].size = size;
            return;
        }
}

static void set_loader_file(const uint8_t *data, uint32_t size,
                            int minigl_file_only)
{
    uint32_t i;

    for (i = 0U; i < zzm_slot_count; i++) {
        loader_files[i].data = data;
        loader_files[i].size = size;
    }
    loader_minigl_file_only = minigl_file_only;
    loader_read_limit = UINT32_MAX;
    loader_fail_offset = UINT32_MAX;
}

FRESULT f_open(FIL *file, const char *path, unsigned char mode)
{
    int index;

    (void)mode;
    loader_open_calls++;
    if (loader_minigl_file_only && strstr(path, "/MINIGL.ZZM") == 0)
        return FR_NO_FILE;
    if (loader_open_result != FR_OK)
        return loader_open_result;
    index = loader_file_index(path);
    if (index < 0 || !loader_files[index].data)
        return FR_NO_FILE;
    file->id = (uint32_t)index;
    file->size = loader_files[index].size;
    loader_file_offset = 0U;
    return FR_OK;
}

FRESULT f_lseek(FIL *file, FSIZE_t offset)
{
    if (offset > file->size)
        return FR_NOT_READY;
    loader_file_offset = offset;
    return FR_OK;
}

FRESULT f_read(FIL *file, void *buffer, UINT length, UINT *read)
{
    const struct loader_file *source = &loader_files[file->id];

    if (!source->data || loader_file_offset > file->size ||
        length > loader_read_limit || loader_file_offset >= loader_fail_offset ||
        length > file->size - loader_file_offset) {
        *read = 0U;
        return FR_NOT_READY;
    }
    memcpy(buffer, source->data + loader_file_offset, length);
    loader_file_offset += length;
    *read = length;
    return FR_OK;
}

FRESULT f_close(FIL *file)
{
    (void)file;
    loader_close_calls++;
    return FR_OK;
}

void sdk_sha256_init(sdk_sha256_context_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void sdk_sha256_update(sdk_sha256_context_t *ctx, const uint8_t *data,
                       uint32_t length)
{
    (void)ctx;
    (void)data;
    (void)length;
}

void sdk_sha256_final(sdk_sha256_context_t *ctx, uint8_t digest[32])
{
    memset(ctx, 0, sizeof(*ctx));
    memset(digest, 0, 32U);
}

void sdk_sha256(const uint8_t *data, uint32_t length, uint8_t digest[32])
{
    (void)data;
    (void)length;
    memset(digest, 0, 32);
}

const zzm_services_t *zzm_runtime_services(void)
{
    return 0;
}

static const struct SDKVideoDecoderOps test_video_ops;

const struct SDKVideoDecoderOps *sdk_video_backend_find(uint32_t codec,
                                                        uint32_t container)
{
    return codec == SDK_VIDEO_CODEC_MPEG2 &&
           container == SDK_VIDEO_CONTAINER_MPEG_PS ? &test_video_ops : 0;
}

void overlay_video_session_closed(uint32_t session)
{
    (void)session;
}

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d\n", __FILE__, __LINE__); } } while (0)

static void test_hash(const uint8_t *image, uint32_t length, uint8_t digest[32])
{
    uint8_t value = 0;
    for (uint32_t i = 0; i < length; i++) value ^= image[i];
    memset(digest, value, 32);
}

static zzm_header_t valid_header(uint32_t image_size)
{
    zzm_header_t header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, ZZM_MAGIC, 4);
    header.header_size = ZZM_HEADER_SIZE;
    header.format_version = ZZM_FORMAT_VERSION;
    header.module_id = ZZM_MODULE_MPEG2;
    header.module_abi_version = ZZM_MODULE_ABI_VERSION;
    header.slot_load_address = zzm_slots[0].address;
    header.image_size = image_size;
    header.init_entry = header.slot_load_address;
    header.idle_unload_ms = ZZM_DEFAULT_IDLE_UNLOAD_MS;
    header.memory_class = ZZM_MEMORY_CLASS_MEDIA;
    header.memory_quota = 1U;
    strcpy(header.name, "MPEG2");
    return header;
}

static zzm_v3_header_t valid_v3_header(uint32_t image_size)
{
    zzm_v3_header_t header;

    memset(&header, 0, sizeof(header));
    memcpy(header.magic, ZZM_V3_MAGIC, 4);
    header.header_size = ZZM_V3_HEADER_SIZE;
    header.format_version = ZZM_V3_FORMAT_VERSION;
    header.module_id = ZZM_MODULE_MINIGL;
    header.abi_major = ZZM_V3_ABI_MAJOR;
    header.abi_minor = ZZM_V3_ABI_MINOR;
    header.slot_load_address = zzm_slots[3].address;
    header.image_size = image_size;
    header.init_entry = header.slot_load_address;
    header.load_policy = ZZM_LOAD_ON_DEMAND;
    header.idle_unload_ms = ZZM_DEFAULT_IDLE_UNLOAD_MS;
    header.resident_quota = 1U;
    header.per_context_quota = 1U;
    header.owner_class_mask = ZZM_OWNER_CLASS_MINIGL_CONTEXT |
                              ZZM_OWNER_CLASS_MINIGL_APP_CONTEXT |
                              ZZM_OWNER_CLASS_MINIGL_TRANSPORT |
                              ZZM_OWNER_CLASS_MINIGL_RIX;
    header.image_flags = ZZM_IMAGE_NEEDS_CORE1 |
                         ZZM_IMAGE_NEEDS_HOST_VISIBLE |
                         ZZM_IMAGE_NEEDS_VFP_NEON;
    header.hardware_requirement = ZZM_HARDWARE_RASTERIX_EF_OPTIONAL;
    header.worker_ops_version = 1U;
    header.required_caps.wire_version = 9U;
    header.required_caps.transport_descriptor_major = 1U;
    header.required_caps.host_window_layout_major = 1U;
    strcpy(header.name, "MINIGL");
    return header;
}

static zzm_v3_capabilities_t valid_v3_capabilities(void)
{
    zzm_v3_capabilities_t caps;

    memset(&caps, 0, sizeof(caps));
    caps.image_flags = ZZM_IMAGE_NEEDS_CORE1 |
                       ZZM_IMAGE_NEEDS_HOST_VISIBLE |
                       ZZM_IMAGE_NEEDS_VFP_NEON;
    caps.wire_version = 9U;
    caps.transport_descriptor_major = 1U;
    caps.host_window_layout_major = 1U;
    return caps;
}

static void check_kit_minigl(const char *path)
{
    FILE *file;
    long size;
    uint8_t *contents;
    zzm_v3_capabilities_t caps = valid_v3_capabilities();
    zzm_status_t status;

    file = fopen(path, "rb");
    CHECK(file != NULL);
    if (!file)
        return;
    CHECK(fseek(file, 0L, SEEK_END) == 0);
    size = ftell(file);
    CHECK(size >= ZZM_V3_HEADER_SIZE);
    CHECK(size <= UINT32_MAX);
    CHECK(fseek(file, 0L, SEEK_SET) == 0);
    if (size < (long)ZZM_V3_HEADER_SIZE || size > (long)UINT32_MAX ||
        fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return;
    }
    contents = malloc((size_t)size);
    CHECK(contents != NULL);
    if (!contents) {
        fclose(file);
        return;
    }
    CHECK(fread(contents, 1U, (size_t)size, file) == (size_t)size);
    fclose(file);

    loader_open_calls = 0U;
    loader_close_calls = 0U;
    loader_open_result = FR_OK;
    set_loader_file(contents, (uint32_t)size, 1);
    zzm_loader_set_v3_capabilities(&caps);
    zzm_loader_boot_load();
    CHECK(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
    CHECK(status.state == ZZM_STATE_AVAILABLE);
    free(contents);
}

static void test_legacy_module_load_is_sliced(void)
{
    const uint32_t image_size = 128U * 1024U;
    const uint32_t file_size = ZZM_HEADER_SIZE + image_size;
    uint8_t *file = calloc(1U, file_size);
    zzm_header_t header = valid_header(image_size);
    zzm_status_t status;
    void *slot;

    CHECK(file != NULL);
    if (!file)
        return;
    slot = mmap((void *)(uintptr_t)header.slot_load_address, 2U << 20,
                PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(slot != MAP_FAILED);
    if (slot == MAP_FAILED) {
        free(file);
        return;
    }
    memcpy(file, &header, sizeof(header));
    loader_open_calls = 0U;
    loader_close_calls = 0U;
    loader_open_result = FR_OK;
    set_loader_file(file, file_size, 0);
    loader_read_limit = 12U * 1024U;
    zzm_loader_boot_load();
    CHECK(zzm_loader_acquire(ZZM_MODULE_MPEG2) == ZZM_LOAD_IN_PROGRESS);
    CHECK(zzm_loader_acquire(ZZM_MODULE_MPEG2) == ZZM_LOAD_IN_PROGRESS);
    CHECK(zzm_loader_test_work_kind() == ZZM_LOADER_TEST_WORK_READ_HASH);
    CHECK(zzm_loader_test_work_bytes() == 12U * 1024U);
    CHECK(loader_file_offset == ZZM_HEADER_SIZE + 12U * 1024U);
    zzm_loader_cancel_loading(ZZM_MODULE_MPEG2);
    CHECK(zzm_loader_status_by_id(ZZM_MODULE_MPEG2, &status));
    CHECK(status.state == ZZM_STATE_AVAILABLE);
    CHECK(status.ref_count == 0U);
    CHECK(munmap(slot, 2U << 20) == 0);
    free(file);
}

static void check_v1_poison(const uint8_t *slot, uint32_t bytes)
{
    uint32_t offset;

    for (offset = 0U; offset + sizeof(uint32_t) <= bytes;
         offset += sizeof(uint32_t))
        CHECK(*(const uint32_t *)(const void *)(slot + offset) ==
              ZZM_POISON_WORD);
    for (; offset < bytes; offset++)
        CHECK(slot[offset] == 0xf0U);
}

static void check_zero(const uint8_t *bytes, uint32_t count)
{
    uint32_t offset;

    for (offset = 0U; offset < count; offset++)
        CHECK(bytes[offset] == 0U);
}

static void check_loader_work_bound(void)
{
    uint32_t bytes = zzm_loader_test_work_bytes();

    switch (zzm_loader_test_work_kind()) {
    case ZZM_LOADER_TEST_WORK_READ_HASH:
        CHECK(bytes <= 12U * 1024U);
        break;
    case ZZM_LOADER_TEST_WORK_BSS_ZERO:
    case ZZM_LOADER_TEST_WORK_DCACHE_FLUSH:
    case ZZM_LOADER_TEST_WORK_ICACHE_INVALIDATE:
    case ZZM_LOADER_TEST_WORK_POISON:
        CHECK(bytes <= 64U * 1024U);
        break;
    default:
        break;
    }
}

static void test_legacy_module_load_lifecycle(void)
{
    const uint32_t image_size = 128U * 1024U;
    const uint32_t bss_size = 128U * 1024U;
    const uint32_t file_size = ZZM_HEADER_SIZE + image_size;
    uint8_t *file = calloc(1U, file_size);
    zzm_header_t header = valid_header(image_size);
    zzm_status_t status;
    void *slot;
    int result = ZZM_LOAD_IN_PROGRESS;
    unsigned passes = 0U;

    CHECK(file != NULL);
    if (!file)
        return;
    header.bss_size = bss_size;
    slot = mmap((void *)(uintptr_t)header.slot_load_address, 2U << 20,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(slot != MAP_FAILED);
    if (slot == MAP_FAILED) {
        free(file);
        return;
    }
    memset(slot, 0xa5, image_size + bss_size);
    memcpy(file, &header, sizeof(header));
    file[ZZM_HEADER_SIZE] = 0x31U; /* xor %eax, %eax */
    file[ZZM_HEADER_SIZE + 1U] = 0xc0U;
    file[ZZM_HEADER_SIZE + 2U] = 0xc3U; /* ret */
    loader_open_result = FR_OK;
    set_loader_file(file, file_size, 0);
    zzm_loader_boot_load();
    do {
        result = zzm_loader_acquire(ZZM_MODULE_MPEG2);
        check_loader_work_bound();
        passes++;
    } while (result == ZZM_LOAD_IN_PROGRESS && passes < 64U);
    CHECK(result == ZZM_LOAD_READY);
    CHECK(zzm_loader_status_by_id(ZZM_MODULE_MPEG2, &status));
    CHECK(status.state == ZZM_STATE_LOADED);
    CHECK(status.ref_count == 1U);
    check_zero((const uint8_t *)slot + image_size, bss_size);
    zzm_loader_release(ZZM_MODULE_MPEG2);
    zzm_loader_poll(header.idle_unload_ms);
    CHECK(zzm_loader_status_by_id(ZZM_MODULE_MPEG2, &status));
    CHECK(status.state == ZZM_STATE_AVAILABLE);
    check_v1_poison(slot, image_size + bss_size);
    CHECK(munmap(slot, 2U << 20) == 0);
    free(file);
}

static void test_legacy_module_failure_poisoning(void)
{
    const uint32_t image_size = 128U * 1024U;
    const uint32_t bss_size = 64U;
    const uint32_t file_size = ZZM_HEADER_SIZE + image_size;
    uint8_t *file = calloc(1U, file_size);
    zzm_header_t header = valid_header(image_size);
    zzm_status_t status;
    void *slot;
    int result = ZZM_LOAD_IN_PROGRESS;
    unsigned passes = 0U;

    CHECK(file != NULL);
    if (!file)
        return;
    header.bss_size = bss_size;
    slot = mmap((void *)(uintptr_t)header.slot_load_address, 2U << 20,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(slot != MAP_FAILED);
    if (slot == MAP_FAILED) {
        free(file);
        return;
    }
    memset(slot, 0xa5, image_size + bss_size);
    memcpy(file, &header, sizeof(header));
    loader_open_result = FR_OK;
    set_loader_file(file, file_size, 0);
    loader_fail_offset = ZZM_HEADER_SIZE + 24U * 1024U;
    zzm_loader_boot_load();
    do {
        result = zzm_loader_acquire(ZZM_MODULE_MPEG2);
        passes++;
    } while (result == ZZM_LOAD_IN_PROGRESS && passes < 64U);
    CHECK(result == ZZM_LOAD_FAILED);
    CHECK(zzm_loader_status_by_id(ZZM_MODULE_MPEG2, &status));
    CHECK(status.state == ZZM_STATE_LOAD_FAILED);
    CHECK(status.reason == ZZM_REASON_FILE_READ);
    check_v1_poison(slot, image_size + bss_size);
    CHECK(munmap(slot, 2U << 20) == 0);
    free(file);
}

static void test_webp_begin_requeues_for_loading_dependency(void)
{
    const uint32_t image_size = 16U;
    uint8_t provider_file[ZZM_HEADER_SIZE + 16U] = { 0 };
    uint8_t webp_file[ZZM_HEADER_SIZE + 16U] = { 0 };
    zzm_header_t provider = valid_header(image_size);
    zzm_header_t webp = valid_header(image_size);
    struct SDKImageStreamBegin begin = {
        .codec = SDK_IMAGE_CODEC_WEBP,
        .output_mode = SDK_IMAGE_OUTPUT_TILE_BUFFER,
        .output_format = SDK_SURFACE_FORMAT_BGRA8888,
        .tile_handle = 1U,
        .tile_stride = 4U,
        .tile_rows = 1U,
        .tile_address = (uintptr_t)provider_file,
        .tile_length = 4U,
        .core1_affine = 1U,
    };
    struct SDKImageStreamResult result;
    void *slot;

    provider.provided_count = 1U;
    provider.provided[0].service_id = 0xfaceU;
    provider.provided[0].min_version = 1U;
    webp.module_id = ZZM_MODULE_WEBP;
    webp.slot_load_address = zzm_slots[2].address;
    webp.init_entry = webp.slot_load_address;
    strcpy(webp.name, "WEBP");
    webp.required_count = 1U;
    webp.required[0] = provider.provided[0];
    memcpy(provider_file, &provider, sizeof(provider));
    memcpy(webp_file, &webp, sizeof(webp));
    slot = mmap((void *)(uintptr_t)provider.slot_load_address, 2U << 20,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(slot != MAP_FAILED);
    if (slot == MAP_FAILED)
        return;
    loader_open_result = FR_OK;
    set_loader_file(0, 0U, 0);
    set_loader_module_file(ZZM_MODULE_MPEG2, provider_file,
                           sizeof(provider_file));
    set_loader_module_file(ZZM_MODULE_WEBP, webp_file, sizeof(webp_file));
    zzm_loader_boot_load();
    CHECK(sdk_image_stream_begin(&begin, &result) == SDK_TASK_IN_PROGRESS);
    CHECK(munmap(slot, 2U << 20) == 0);
}

static void test_v1_codec_begin_requeues_until_loaded(void)
{
    const uint32_t image_size = 128U * 1024U;
    const uint32_t file_size = ZZM_HEADER_SIZE + image_size;
    uint8_t *file = calloc(1U, file_size);
    zzm_header_t header = valid_header(image_size);
    struct SDKVideoStreamBegin begin = {
        .codec = SDK_VIDEO_CODEC_MPEG2,
        .container = SDK_VIDEO_CONTAINER_MPEG_PS,
        .width = 16U,
        .height = 16U,
        .output_format = SDK_VIDEO_OUTPUT_DIRECT_OVERLAY,
    };
    struct SDKVideoStreamResult result;
    uint16_t status;
    void *slot;
    unsigned passes = 0U;

    CHECK(file != NULL);
    if (!file)
        return;
    slot = mmap((void *)(uintptr_t)header.slot_load_address, 2U << 20,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(slot != MAP_FAILED);
    if (slot == MAP_FAILED) {
        free(file);
        return;
    }
    memcpy(file, &header, sizeof(header));
    file[ZZM_HEADER_SIZE] = 0x31U; /* xor %eax, %eax */
    file[ZZM_HEADER_SIZE + 1U] = 0xc0U;
    file[ZZM_HEADER_SIZE + 2U] = 0xc3U; /* ret */
    loader_open_result = FR_OK;
    set_loader_file(file, file_size, 0);
    zzm_loader_boot_load();
    sdk_video_stream_init();
    status = sdk_video_stream_begin(&begin, &result);
    CHECK(status == SDK_TASK_IN_PROGRESS);
    do {
        status = sdk_video_stream_begin(&begin, &result);
        passes++;
    } while (status == SDK_TASK_IN_PROGRESS && passes < 64U);
    CHECK(status == SDK_STATUS_OK);
    CHECK(result.session != 0U);
    CHECK(sdk_video_stream_close(result.session, &result) == SDK_STATUS_OK);
    CHECK(munmap(slot, 2U << 20) == 0);
    free(file);
}

/* Core 1 holds the session-begin guard for a load step; core 0 runs next. */
static void hold_guard_on_core1(void)
{
    mock_cpu = 1;
    CHECK(zzm_loader_session_begin_enter() == 1);
    mock_cpu = 0;
}

static void release_guard_on_core1(void)
{
    mock_cpu = 1;
    zzm_loader_session_begin_unlock();
    mock_cpu = 0;
}

static unsigned fake_dispatch_calls;
static int fake_dispatch_queue_only(uint32_t module_id)
{
    (void)module_id;
    fake_dispatch_calls++;
    return ZZM_LOAD_FAILED;
}

static uint32_t module_ref_count(uint32_t module_id)
{
    zzm_status_t status;

    CHECK(zzm_loader_status_by_id(module_id, &status));
    return status.ref_count;
}

static void test_core0_video_begin_never_waits_for_loader_guard(void)
{
    const uint32_t image_size = 128U * 1024U;
    const uint32_t file_size = ZZM_HEADER_SIZE + image_size;
    uint8_t *file = calloc(1U, file_size);
    zzm_header_t header = valid_header(image_size);
    struct SDKVideoStreamBegin begin = {
        .codec = SDK_VIDEO_CODEC_MPEG2,
        .container = SDK_VIDEO_CONTAINER_MPEG_PS,
        .width = 16U,
        .height = 16U,
        .output_format = SDK_VIDEO_OUTPUT_DIRECT_OVERLAY,
    };
    struct SDKVideoStreamResult result;
    zzm_status_t module;
    unsigned blocked;
    unsigned opens;
    unsigned passes = 0U;
    uint16_t status;
    void *slot;
    int i;

    CHECK(file != NULL);
    if (!file)
        return;
    slot = mmap((void *)(uintptr_t)header.slot_load_address, 2U << 20,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(slot != MAP_FAILED);
    if (slot == MAP_FAILED) {
        free(file);
        return;
    }
    memcpy(file, &header, sizeof(header));
    file[ZZM_HEADER_SIZE] = 0x31U; /* xor %eax, %eax */
    file[ZZM_HEADER_SIZE + 1U] = 0xc0U;
    file[ZZM_HEADER_SIZE + 2U] = 0xc3U; /* ret */
    loader_open_result = FR_OK;
    set_loader_file(file, file_size, 0);
    zzm_loader_boot_load();
    sdk_video_stream_init();

    /* Core 1 is mid-load. Core 0 must answer "in progress" at once: no
     * wait on the guard, no load step, no registry use. */
    hold_guard_on_core1();
    blocked = mock_blocked_spins;
    opens = loader_open_calls;
    for (i = 0; i < 3; i++) {
        status = sdk_video_stream_begin(&begin, &result);
        CHECK(status == SDK_TASK_IN_PROGRESS);
    }
    CHECK(mock_blocked_spins == blocked);
    CHECK(loader_open_calls == opens);
    CHECK(zzm_loader_status_by_id(ZZM_MODULE_MPEG2, &module));
    CHECK(module.state == ZZM_STATE_AVAILABLE);

    /* The same holds for the loader entry points a core-0 caller can reach
     * directly. */
    CHECK(zzm_loader_ensure_loaded(ZZM_MODULE_MPEG2) == ZZM_LOAD_IN_PROGRESS);
    CHECK(zzm_loader_acquire(ZZM_MODULE_MPEG2) == ZZM_LOAD_IN_PROGRESS);
    CHECK(zzm_registry_sdk_status(ZZM_MODULE_MPEG2) == SDK_TASK_IN_PROGRESS);
    CHECK(mock_blocked_spins == blocked);
    CHECK(loader_open_calls == opens);
    release_guard_on_core1();

    /* Once core 1 lets go, begin loads the module and succeeds. */
    do {
        status = sdk_video_stream_begin(&begin, &result);
        passes++;
    } while (status == SDK_TASK_IN_PROGRESS && passes < 64U);
    CHECK(status == SDK_STATUS_OK);
    CHECK(result.session != 0U);
    CHECK(loader_open_calls > opens);
    CHECK(module_ref_count(ZZM_MODULE_MPEG2) == 1U);

    /* A close on core 0 while core 1 holds the guard keeps the reference
     * and hands the release to the next poll pass. */
    hold_guard_on_core1();
    CHECK(sdk_video_stream_close(result.session, &result) == SDK_STATUS_OK);
    CHECK(mock_blocked_spins == blocked);
    CHECK(module_ref_count(ZZM_MODULE_MPEG2) == 1U);
    zzm_loader_poll(1U);
    CHECK(module_ref_count(ZZM_MODULE_MPEG2) == 1U);
    release_guard_on_core1();
    zzm_loader_poll(2U);
    CHECK(module_ref_count(ZZM_MODULE_MPEG2) == 0U);
    CHECK(mock_blocked_spins == blocked);

    /* With core 1 online the dispatcher only queues the load; core 0 must
     * not read that as a failure while the module is still available. */
    fake_dispatch_calls = 0U;
    zzm_loader_set_load_dispatch(fake_dispatch_queue_only);
    zzm_loader_boot_load();
    CHECK(zzm_loader_ensure_loaded(ZZM_MODULE_MPEG2) == ZZM_LOAD_IN_PROGRESS);
    CHECK(fake_dispatch_calls == 1U);
    zzm_loader_set_load_dispatch(0);
    CHECK(munmap(slot, 2U << 20) == 0);
    free(file);
}

static void test_core0_image_begin_never_waits_for_loader_guard(void)
{
    const uint32_t image_size = 16U;
    uint8_t provider_file[ZZM_HEADER_SIZE + 16U] = { 0 };
    uint8_t webp_file[ZZM_HEADER_SIZE + 16U] = { 0 };
    zzm_header_t provider = valid_header(image_size);
    zzm_header_t webp = valid_header(image_size);
    struct SDKImageStreamBegin begin = {
        .codec = SDK_IMAGE_CODEC_WEBP,
        .output_mode = SDK_IMAGE_OUTPUT_TILE_BUFFER,
        .output_format = SDK_SURFACE_FORMAT_BGRA8888,
        .tile_handle = 1U,
        .tile_stride = 4U,
        .tile_rows = 1U,
        .tile_address = (uintptr_t)provider_file,
        .tile_length = 4U,
        .core1_affine = 1U,
    };
    struct SDKImageStreamResult result;
    unsigned blocked;
    unsigned opens;
    void *slot;
    int i;

    provider.provided_count = 1U;
    provider.provided[0].service_id = 0xfaceU;
    provider.provided[0].min_version = 1U;
    webp.module_id = ZZM_MODULE_WEBP;
    webp.slot_load_address = zzm_slots[2].address;
    webp.init_entry = webp.slot_load_address;
    strcpy(webp.name, "WEBP");
    webp.required_count = 1U;
    webp.required[0] = provider.provided[0];
    memcpy(provider_file, &provider, sizeof(provider));
    memcpy(webp_file, &webp, sizeof(webp));
    slot = mmap((void *)(uintptr_t)provider.slot_load_address, 2U << 20,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(slot != MAP_FAILED);
    if (slot == MAP_FAILED)
        return;
    loader_open_result = FR_OK;
    set_loader_file(0, 0U, 0);
    set_loader_module_file(ZZM_MODULE_MPEG2, provider_file,
                           sizeof(provider_file));
    set_loader_module_file(ZZM_MODULE_WEBP, webp_file, sizeof(webp_file));
    zzm_loader_boot_load();

    hold_guard_on_core1();
    blocked = mock_blocked_spins;
    opens = loader_open_calls;
    for (i = 0; i < 3; i++)
        CHECK(sdk_image_stream_begin(&begin, &result) == SDK_TASK_IN_PROGRESS);
    CHECK(mock_blocked_spins == blocked);
    CHECK(loader_open_calls == opens);
    release_guard_on_core1();

    /* Released: the begin now takes the guard and starts the load. */
    CHECK(sdk_image_stream_begin(&begin, &result) == SDK_TASK_IN_PROGRESS);
    CHECK(loader_open_calls > opens);
    CHECK(mock_blocked_spins == blocked);
    /* The load is left half done with its file open; a boot scan drops it. */
    zzm_loader_boot_load();
    CHECK(munmap(slot, 2U << 20) == 0);
}

const zzm_services_t *zzm_runtime_services_v3(void)
{
    static zzm_services_t services;

    return &services;
}

static void acquire_minigl(void)
{
    unsigned passes = 0U;
    int result;

    do {
        result = zzm_loader_acquire(ZZM_MODULE_MINIGL);
        passes++;
    } while (result == ZZM_LOAD_IN_PROGRESS && passes < 64U);
    CHECK(result == ZZM_LOAD_READY);
}

/* A release core 0 deferred belongs to the module lifetime it was made in.
 * When core 1 restarts before a poll applies it, the new lifetime must keep
 * every reference its clients took. */
static void test_core0_deferred_release_dies_with_its_module_lifetime(void)
{
    const uint32_t image_size = 16U;
    uint8_t file[ZZM_V3_HEADER_SIZE + 16U] = { 0 };
    zzm_v3_header_t header = valid_v3_header(image_size);
    zzm_v3_capabilities_t caps = valid_v3_capabilities();
    void *slot;

    memcpy(file, &header, sizeof(header));
    file[ZZM_V3_HEADER_SIZE] = 0x31U; /* xor %eax, %eax */
    file[ZZM_V3_HEADER_SIZE + 1U] = 0xc0U;
    file[ZZM_V3_HEADER_SIZE + 2U] = 0xc3U; /* ret */
    slot = mmap((void *)(uintptr_t)header.slot_load_address, 2U << 20,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(slot != MAP_FAILED);
    if (slot == MAP_FAILED)
        return;
    loader_open_result = FR_OK;
    set_loader_file(file, sizeof(file), 1);
    zzm_loader_set_v3_capabilities(&caps);
    zzm_loader_boot_load();
    acquire_minigl();
    CHECK(module_ref_count(ZZM_MODULE_MINIGL) == 1U);

    /* Core 0 releases while core 1 holds the guard; core 1 then faults, and
     * the restart discards the module before any poll applies the release. */
    hold_guard_on_core1();
    zzm_loader_release(ZZM_MODULE_MINIGL);
    CHECK(module_ref_count(ZZM_MODULE_MINIGL) == 1U);
    release_guard_on_core1();
    zzm_loader_forget_core1_modules();
    CHECK(module_ref_count(ZZM_MODULE_MINIGL) == 0U);
    acquire_minigl();
    CHECK(module_ref_count(ZZM_MODULE_MINIGL) == 1U);
    zzm_loader_poll(1U);
    CHECK(module_ref_count(ZZM_MODULE_MINIGL) == 1U);

    /* A release that finds the module no longer loaded has no reference
     * left to drop and must not wait for the next lifetime either. */
    zzm_loader_forget_core1_modules();
    hold_guard_on_core1();
    zzm_loader_release(ZZM_MODULE_MINIGL);
    release_guard_on_core1();
    acquire_minigl();
    zzm_loader_poll(2U);
    CHECK(module_ref_count(ZZM_MODULE_MINIGL) == 1U);

    /* A release deferred in the live lifetime still lands. */
    hold_guard_on_core1();
    zzm_loader_release(ZZM_MODULE_MINIGL);
    release_guard_on_core1();
    zzm_loader_poll(3U);
    CHECK(module_ref_count(ZZM_MODULE_MINIGL) == 0U);

    zzm_loader_boot_load();
    zzm_loader_set_v3_capabilities(NULL);
    CHECK(munmap(slot, 2U << 20) == 0);
}

int main(int argc, char **argv)
{
    uint8_t image[16] = { 1, 2, 3, 4 };
    zzm_header_t header = valid_header(sizeof(image));
    test_hash(image, sizeof(image), header.image_sha256);
    CHECK(zzm_verify_image(&header, image, ZZM_HEADER_SIZE + sizeof(image), test_hash) == ZZM_STATE_LOADED);
    {
        zzm_v3_header_t v3 = valid_v3_header(sizeof(image));
        zzm_v3_capabilities_t caps = valid_v3_capabilities();

        test_hash(image, sizeof(image), v3.image_sha256);
        CHECK(zzm_verify_v3_image(&v3, image,
                                  ZZM_V3_HEADER_SIZE + sizeof(image),
                                  &caps, test_hash) == ZZM_STATE_LOADED);
        v3.format_version++;
        CHECK(zzm_verify_v3_image(&v3, image,
                                  ZZM_V3_HEADER_SIZE + sizeof(image),
                                  &caps, test_hash) == ZZM_STATE_REJECTED_SLOT);
        v3 = valid_v3_header(sizeof(image));
        test_hash(image, sizeof(image), v3.image_sha256);
        v3.required_caps.wire_version++;
        CHECK(zzm_verify_v3_image(&v3, image,
                                  ZZM_V3_HEADER_SIZE + sizeof(image),
                                  &caps, test_hash) == ZZM_STATE_REJECTED_CAPABILITY);
        v3 = valid_v3_header(sizeof(image));
        test_hash(image, sizeof(image), v3.image_sha256);
        v3.slot_load_address++;
        CHECK(zzm_verify_v3_image(&v3, image,
                                  ZZM_V3_HEADER_SIZE + sizeof(image),
                                  &caps, test_hash) == ZZM_STATE_REJECTED_SLOT);
    }
    {
        uint8_t boot_file[ZZM_V3_HEADER_SIZE + sizeof(image)];
        zzm_v3_header_t v3 = valid_v3_header(sizeof(image));
        zzm_v3_capabilities_t caps = valid_v3_capabilities();
        zzm_status_t status;

        test_hash(image, sizeof(image), v3.image_sha256);
        memcpy(boot_file, &v3, sizeof(v3));
        memcpy(boot_file + sizeof(v3), image, sizeof(image));
        zzm_loader_set_v3_capabilities(NULL);
        loader_open_calls = 0U;
        loader_close_calls = 0U;
        loader_open_result = FR_OK;
        set_loader_file(boot_file, sizeof(boot_file), 1);
        zzm_loader_boot_load();
        CHECK(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
        CHECK(status.state == ZZM_STATE_REJECTED_CAPABILITY);

        zzm_loader_set_v3_capabilities(&caps);
        loader_open_calls = 0U;
        loader_close_calls = 0U;
        loader_open_result = FR_OK;
        set_loader_file(boot_file, sizeof(boot_file), 1);
        zzm_loader_boot_load();
        CHECK(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
        CHECK(status.state == ZZM_STATE_AVAILABLE);
        CHECK(zzm_loader_status_by_id(ZZM_MODULE_MPEG2, &status));
        CHECK(status.state == ZZM_STATE_MISSING);

        v3.module_id = ZZM_MODULE_MPEG2;
        v3.slot_load_address = zzm_slots[0].address;
        v3.init_entry = v3.slot_load_address;
        strcpy(v3.name, "MPEG2");
        memcpy(boot_file, &v3, sizeof(v3));
        zzm_loader_boot_load();
        CHECK(zzm_loader_status_by_id(ZZM_MODULE_MINIGL, &status));
        CHECK(status.state == ZZM_STATE_REJECTED_SLOT);
    }
    header.module_abi_version++;
    CHECK(zzm_verify_image(&header, image, ZZM_HEADER_SIZE + sizeof(image), test_hash) == ZZM_STATE_REJECTED_ABI);
    header = valid_header(sizeof(image));
    header.required_count = 1U;
    header.required[0].service_id = 1U;
    header.required[0].min_version = 1U;
    CHECK(zzm_verify_image(&header, image, ZZM_HEADER_SIZE + sizeof(image), test_hash) == ZZM_STATE_REJECTED_SERVICE);
    header = valid_header(sizeof(image));
    test_hash(image, sizeof(image), header.image_sha256);
    header.slot_load_address++;
    CHECK(zzm_verify_image(&header, image, ZZM_HEADER_SIZE + sizeof(image), test_hash) == ZZM_STATE_REJECTED_SLOT);
    header = valid_header(sizeof(image));
    test_hash(image, sizeof(image), header.image_sha256);
    header.image_sha256[0]++;
    CHECK(zzm_verify_image(&header, image, ZZM_HEADER_SIZE + sizeof(image), test_hash) == ZZM_STATE_REJECTED_HASH);
    header = valid_header(sizeof(image));
    test_hash(image, sizeof(image), header.image_sha256);
    CHECK(zzm_verify_image(&header, image, ZZM_HEADER_SIZE + sizeof(image) - 1, test_hash) == ZZM_STATE_REJECTED_SLOT);
    test_legacy_module_load_is_sliced();
    test_legacy_module_load_lifecycle();
    test_legacy_module_failure_poisoning();
    test_webp_begin_requeues_for_loading_dependency();
    test_v1_codec_begin_requeues_until_loaded();
    test_core0_video_begin_never_waits_for_loader_guard();
    test_core0_image_begin_never_waits_for_loader_guard();
    test_core0_deferred_release_dies_with_its_module_lifetime();
    if (argc == 2)
        check_kit_minigl(argv[1]);
    loader_open_calls = 0U;
    loader_close_calls = 0U;
    set_loader_file(0, 0U, 0);
    loader_open_result = FR_NOT_READY;
    zzm_loader_boot_load();
    CHECK(loader_open_calls == zzm_slot_count);
    CHECK(loader_close_calls == 0);
    loader_open_calls = 0;
    loader_open_result = FR_NO_FILESYSTEM;
    zzm_loader_boot_load();
    CHECK(loader_open_calls == zzm_slot_count);
    CHECK(loader_close_calls == 0);
    return failures != 0;
}
