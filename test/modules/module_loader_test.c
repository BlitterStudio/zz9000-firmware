#include <stdio.h>
#include <string.h>
#include "zzm_loader.h"
#include "zzm_manifest.h"
#include "ff.h"
#include "zzm_runtime.h"


/* Single-threaded host backing for the loader's cross-core session lock. */
int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t flags) { (void)flags; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1U; }
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
static const uint8_t *loader_file_data;
static uint32_t loader_file_size;
static uint32_t loader_file_offset;
static int loader_minigl_file_only;

static void set_loader_file(const uint8_t *data, uint32_t size,
                            int minigl_file_only)
{
    loader_file_data = data;
    loader_file_size = size;
    loader_minigl_file_only = minigl_file_only;
}

FRESULT f_open(FIL *file, const char *path, unsigned char mode)
{
    (void)mode;
    loader_open_calls++;
    if (loader_minigl_file_only && strstr(path, "/MINIGL.ZZM") == 0)
        return FR_NO_FILE;
    if (loader_open_result != FR_OK)
        return loader_open_result;
    file->size = loader_file_size;
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
    if (!loader_file_data || loader_file_offset > file->size ||
        length > file->size - loader_file_offset) {
        *read = 0U;
        return FR_NOT_READY;
    }
    memcpy(buffer, loader_file_data + loader_file_offset, length);
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

int main(void)
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
