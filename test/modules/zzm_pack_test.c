#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zzm_format.h"

#define ELF_HEADER_SIZE 52U
#define ELF_SECTION_SIZE 40U
#define ELF_SECTION_TABLE 0x80U
#define ELF_FILE_SIZE (ELF_SECTION_TABLE + 4U * ELF_SECTION_SIZE)
#define SLOT_ADDRESS 0x09000000U
#define SLOT_SIZE 0x00400000U
#define SHT_PROGBITS 1U
#define SHT_RELA 4U
#define SHT_NOBITS 8U
#define SHF_ALLOC 0x2U

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

static void put_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void put_section(uint8_t *section, uint32_t type, uint32_t flags,
                        uint32_t address, uint32_t offset, uint32_t size)
{
    put_le32(section + 4, type);
    put_le32(section + 8, flags);
    put_le32(section + 12, address);
    put_le32(section + 16, offset);
    put_le32(section + 20, size);
    put_le32(section + 32, 4);
}

static int write_elf(const char *path, int relocation, int outside_slot)
{
    uint8_t elf[ELF_FILE_SIZE];
    FILE *file;

    memset(elf, 0, sizeof(elf));
    memcpy(elf, "\177ELF", 4);
    elf[4] = 1;
    elf[5] = 1;
    elf[6] = 1;
    put_le16(elf + 16, 2);
    put_le16(elf + 18, 40);
    put_le32(elf + 20, 1);
    put_le32(elf + 24, SLOT_ADDRESS + 4U);
    put_le32(elf + 32, ELF_SECTION_TABLE);
    put_le16(elf + 40, ELF_HEADER_SIZE);
    put_le16(elf + 46, ELF_SECTION_SIZE);
    put_le16(elf + 48, 4);

    elf[0x60] = 0xde;
    elf[0x61] = 0xad;
    elf[0x62] = 0xbe;
    elf[0x63] = 0xef;
    elf[0x64] = 1;
    elf[0x65] = 2;
    elf[0x66] = 3;
    elf[0x68] = 0xaa;
    put_section(elf + ELF_SECTION_TABLE + ELF_SECTION_SIZE,
                SHT_PROGBITS, SHF_ALLOC, SLOT_ADDRESS, 0x60, 4);
    put_section(elf + ELF_SECTION_TABLE + 2U * ELF_SECTION_SIZE,
                SHT_PROGBITS, SHF_ALLOC, SLOT_ADDRESS + 8U, 0x64, 3);
    if (relocation) {
        put_section(elf + ELF_SECTION_TABLE + 3U * ELF_SECTION_SIZE,
                    SHT_RELA, 0, 0, 0, 0);
    } else if (outside_slot) {
        put_section(elf + ELF_SECTION_TABLE + 3U * ELF_SECTION_SIZE,
                    SHT_PROGBITS, SHF_ALLOC, SLOT_ADDRESS + SLOT_SIZE,
                    0x68, 1);
    } else {
        put_section(elf + ELF_SECTION_TABLE + 3U * ELF_SECTION_SIZE,
                    SHT_NOBITS, SHF_ALLOC, SLOT_ADDRESS + 0x10U, 0, 8);
    }

    file = fopen(path, "wb");
    if (!file) return 0;
    if (fwrite(elf, 1, sizeof(elf), file) != sizeof(elf) || fclose(file) != 0)
        return 0;
    return 1;
}

static int run_packer(const char *packer, const char *input, const char *output)
{
    char command[1024];

    if (snprintf(command, sizeof(command), "\"%s\" 1 7 \"%s\" \"%s\"",
                 packer, input, output) >= (int)sizeof(command))
        return 0;
    return system(command) == 0;
}

static int read_file(const char *path, uint8_t *buffer, size_t size)
{
    FILE *file = fopen(path, "rb");
    int success;

    if (!file) return 0;
    success = fread(buffer, 1, size, file) == size && fgetc(file) == EOF;
    fclose(file);
    return success;
}

static int path_exists(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (!file) return 0;
    fclose(file);
    return 1;
}

static void test_success(const char *packer)
{
    const char *input = "build/zzm_pack_success.elf";
    const char *output = "build/zzm_pack_success.ZZM";
    const uint8_t expected_image[] = {
        0xde, 0xad, 0xbe, 0xef, 0, 0, 0, 0, 1, 2, 3
    };
    const uint8_t expected_sha256[ZZM_SHA256_SIZE] = {
        0xa9, 0x28, 0x17, 0x9b, 0xf8, 0x9b, 0xab, 0xa7,
        0x81, 0xcc, 0x9d, 0xec, 0xbf, 0xe4, 0x9f, 0xa4,
        0x8e, 0xaa, 0xbb, 0xbe, 0x63, 0xfb, 0xda, 0x75,
        0x06, 0x59, 0x78, 0xc4, 0x0b, 0x0c, 0xc2, 0xc3
    };
    uint8_t packed[ZZM_HEADER_SIZE + sizeof(expected_image)];
    zzm_header_t header;

    remove(input);
    remove(output);
    CHECK(write_elf(input, 0, 0));
    CHECK(run_packer(packer, input, output));
    if (!read_file(output, packed, sizeof(packed))) {
        CHECK(0);
        remove(input);
        remove(output);
        return;
    }
    memcpy(&header, packed, sizeof(header));
    CHECK(memcmp(header.magic, ZZM_MAGIC, sizeof(header.magic)) == 0);
    CHECK(header.header_size == ZZM_HEADER_SIZE);
    CHECK(header.format_version == ZZM_FORMAT_VERSION);
    CHECK(header.module_id == ZZM_MODULE_MPEG2);
    CHECK(header.module_version == 7U);
    CHECK(header.module_abi_version == ZZM_MODULE_ABI_VERSION);
    CHECK(header.slot_load_address == SLOT_ADDRESS);
    CHECK(header.image_size == sizeof(expected_image));
    CHECK(header.bss_size == 13U);
    CHECK(header.init_entry == SLOT_ADDRESS + 4U);
    CHECK(strcmp(header.name, "MPEG2") == 0);
    CHECK(memcmp(packed + ZZM_HEADER_SIZE, expected_image,
                 sizeof(expected_image)) == 0);
    CHECK(memcmp(header.image_sha256, expected_sha256, sizeof(expected_sha256)) == 0);
    remove(input);
    remove(output);
}

static void test_relocation_rejection(const char *packer)
{
    const char *input = "build/zzm_pack_relocation.elf";
    const char *output = "build/zzm_pack_relocation.ZZM";

    remove(input);
    remove(output);
    CHECK(write_elf(input, 1, 0));
    CHECK(!run_packer(packer, input, output));
    CHECK(!path_exists(output));
    remove(input);
    remove(output);
}

static void test_outside_slot_rejection(const char *packer)
{
    const char *input = "build/zzm_pack_outside.elf";
    const char *output = "build/zzm_pack_outside.ZZM";

    remove(input);
    remove(output);
    CHECK(write_elf(input, 0, 1));
    CHECK(!run_packer(packer, input, output));
    CHECK(!path_exists(output));
    remove(input);
    remove(output);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s PACKER\n", argv[0]);
        return 2;
    }
    test_success(argv[1]);
    test_relocation_rejection(argv[1]);
    test_outside_slot_rejection(argv[1]);
    return failures != 0;
}
