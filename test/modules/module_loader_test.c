#include <stdio.h>
#include <string.h>
#include "zzm_loader.h"
#include "zzm_manifest.h"

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
    return failures != 0;
}
