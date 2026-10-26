#include <stdio.h>
#include <string.h>
#include "zzm_loader.h"
#include "zzm_manifest.h"
#include "ff.h"
#include "zzm_runtime.h"

static unsigned loader_open_calls;
static unsigned loader_close_calls;
static FRESULT loader_open_result;

FRESULT f_open(FIL *file, const char *path, unsigned char mode)
{
    (void)file;
    (void)path;
    (void)mode;
    loader_open_calls++;
    return loader_open_result;
}

FRESULT f_lseek(FIL *file, FSIZE_t offset)
{
    (void)file;
    (void)offset;
    return FR_OK;
}

FRESULT f_read(FIL *file, void *buffer, UINT length, UINT *read)
{
    (void)file;
    (void)buffer;
    (void)length;
    *read = 0;
    return FR_NOT_READY;
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
    header.memory_quota = 1U;
    strcpy(header.name, "MPEG2");
    return header;
}

int main(void)
{
    uint8_t image[16] = { 1, 2, 3, 4 };
    zzm_header_t header = valid_header(sizeof(image));
    test_hash(image, sizeof(image), header.image_sha256);
    CHECK(zzm_verify_image(&header, image, ZZM_HEADER_SIZE + sizeof(image), test_hash) == ZZM_STATE_LOADED);
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
