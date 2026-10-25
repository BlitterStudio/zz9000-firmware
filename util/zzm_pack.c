#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zzm_format.h"
#include "zzm_manifest.h"

#define ELF32_EHDR_SIZE 52U
#define ELF32_SHDR_SIZE 40U
#define ELFCLASS32 1U
#define ELFDATA2LSB 1U
#define EV_CURRENT 1U
#define SHF_ALLOC 0x2U
#define SHT_RELA 4U
#define SHT_NOBITS 8U
#define SHT_REL 9U
#define SHT_RELR 19U

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_used;
} sha256_ctx_t;

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static uint32_t rotr32(uint32_t value, unsigned int count)
{
    return (value >> count) | (value << (32U - count));
}

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t block[64])
{
    static const uint32_t constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    unsigned int i;

    for (i = 0; i < 16; i++) {
        words[i] = ((uint32_t)block[i * 4] << 24) |
                   ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) |
                   block[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(words[i - 15], 7) ^ rotr32(words[i - 15], 18) ^
                      (words[i - 15] >> 3);
        uint32_t s1 = rotr32(words[i - 2], 17) ^ rotr32(words[i - 2], 19) ^
                      (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t temp1 = h + s1 + choice + constants[i] + words[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    static const uint32_t initial_state[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    memcpy(ctx->state, initial_state, sizeof(ctx->state));
    ctx->bit_count = 0;
    ctx->block_used = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t size)
{
    while (size != 0) {
        size_t count = 64U - ctx->block_used;
        if (count > size) count = size;
        memcpy(ctx->block + ctx->block_used, data, count);
        ctx->block_used += count;
        data += count;
        size -= count;
        if (ctx->block_used == sizeof(ctx->block)) {
            sha256_transform(ctx, ctx->block);
            ctx->bit_count += 512U;
            ctx->block_used = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t digest[ZZM_SHA256_SIZE])
{
    uint64_t bit_count = ctx->bit_count + (uint64_t)ctx->block_used * 8U;
    unsigned int i;

    ctx->block[ctx->block_used++] = 0x80U;
    if (ctx->block_used > 56U) {
        memset(ctx->block + ctx->block_used, 0, 64U - ctx->block_used);
        sha256_transform(ctx, ctx->block);
        ctx->block_used = 0;
    }
    memset(ctx->block + ctx->block_used, 0, 56U - ctx->block_used);
    for (i = 0; i < 8; i++)
        ctx->block[63U - i] = (uint8_t)(bit_count >> (i * 8U));
    sha256_transform(ctx, ctx->block);
    for (i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
}

static int read_at(FILE *input, long file_size, uint32_t offset, void *buffer,
                   size_t size)
{
    if ((uint64_t)offset > (uint64_t)file_size ||
        size > (uint64_t)file_size - offset ||
        fseek(input, (long)offset, SEEK_SET) != 0 ||
        fread(buffer, 1, size, input) != size)
        return 0;
    return 1;
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || *text == '\0' || *end != '\0' || parsed > UINT32_MAX)
        return 0;
    *value = (uint32_t)parsed;
    return 1;
}

static const zzm_slot_t *find_slot(uint32_t id)
{
    uint32_t index;

    for (index = 0; index < zzm_slot_count; index++) {
        if (zzm_slots[index].id == id)
            return &zzm_slots[index];
    }
    return 0;
}

static int section_in_slot(uint32_t address, uint32_t size,
                           const zzm_slot_t *slot)
{
    uint64_t slot_start = slot->address;
    uint64_t slot_end = slot_start + slot->size;
    uint64_t section_start = address;
    uint64_t section_end = section_start + size;

    return section_start >= slot_start && section_start <= slot_end &&
           section_end <= slot_end;
}

static int pack_elf(uint32_t module_id, uint32_t module_version,
                    const char *input_path, const char *output_path)
{
    uint8_t elf_header[ELF32_EHDR_SIZE];
    uint8_t section[ELF32_SHDR_SIZE];
    const zzm_slot_t *slot = find_slot(module_id);
    FILE *input = 0;
    FILE *output = 0;
    long file_size;
    uint32_t section_offset;
    uint32_t section_table_offset;
    uint32_t image_end;
    uint32_t allocation_end;
    uint32_t entry;
    uint16_t section_count;
    uint16_t section_size;
    uint16_t index;
    uint8_t *image = 0;
    uint8_t header[ZZM_HEADER_SIZE];
    uint32_t image_size;
    uint32_t bss_size;
    sha256_ctx_t sha256;
    int result = 1;

    if (!slot) {
        fprintf(stderr, "zzm_pack: module id %lu is not in the manifest\n",
                (unsigned long)module_id);
        return 0;
    }
    if (strlen(slot->name) >= ZZM_NAME_SIZE) {
        fprintf(stderr, "zzm_pack: manifest name is too long\n");
        return 0;
    }

    input = fopen(input_path, "rb");
    if (!input) {
        fprintf(stderr, "zzm_pack: cannot open %s\n", input_path);
        goto done;
    }
    if (fseek(input, 0, SEEK_END) != 0 || (file_size = ftell(input)) < 0 ||
        fseek(input, 0, SEEK_SET) != 0 || file_size < ELF32_EHDR_SIZE ||
        !read_at(input, file_size, 0, elf_header, sizeof(elf_header))) {
        fprintf(stderr, "zzm_pack: cannot read ELF header\n");
        goto done;
    }
    if (memcmp(elf_header, "\177ELF", 4) != 0 || elf_header[4] != ELFCLASS32 ||
        elf_header[5] != ELFDATA2LSB || elf_header[6] != EV_CURRENT ||
        read_le16(elf_header + 40) < ELF32_EHDR_SIZE) {
        fprintf(stderr, "zzm_pack: input is not a little-endian ELF32 file\n");
        goto done;
    }

    entry = read_le32(elf_header + 24);
    section_table_offset = read_le32(elf_header + 32);
    section_size = read_le16(elf_header + 46);
    section_count = read_le16(elf_header + 48);
    if (section_count == 0 || section_size < ELF32_SHDR_SIZE ||
        (uint64_t)section_table_offset + (uint64_t)section_size * section_count >
            (uint64_t)file_size) {
        fprintf(stderr, "zzm_pack: invalid ELF section table\n");
        goto done;
    }

    image_end = slot->address;
    allocation_end = slot->address;
    for (index = 0; index < section_count; index++) {
        uint32_t type;
        uint32_t flags;
        uint32_t address;
        uint32_t size;
        uint64_t end;

        section_offset = section_table_offset + (uint32_t)index * section_size;
        if (!read_at(input, file_size, section_offset, section, sizeof(section))) {
            fprintf(stderr, "zzm_pack: cannot read ELF section table\n");
            goto done;
        }
        type = read_le32(section + 4);
        flags = read_le32(section + 8);
        address = read_le32(section + 12);
        size = read_le32(section + 20);
        if (type == SHT_REL || type == SHT_RELA || type == SHT_RELR) {
            fprintf(stderr, "zzm_pack: relocation sections are not supported\n");
            goto done;
        }
        if ((flags & SHF_ALLOC) == 0)
            continue;
        if (!section_in_slot(address, size, slot)) {
            fprintf(stderr, "zzm_pack: allocated section is outside the module slot\n");
            goto done;
        }
        end = (uint64_t)address + size;
        if (end > allocation_end)
            allocation_end = (uint32_t)end;
        if (type != SHT_NOBITS && end > image_end)
            image_end = (uint32_t)end;
    }
    if (image_end == slot->address || entry < slot->address || entry >= image_end) {
        fprintf(stderr, "zzm_pack: ELF entry is not in the initialized module image\n");
        goto done;
    }
    if (allocation_end < image_end) {
        fprintf(stderr, "zzm_pack: invalid allocated section layout\n");
        goto done;
    }

    image = calloc((size_t)(image_end - slot->address), 1);
    if (!image) {
        fprintf(stderr, "zzm_pack: cannot allocate image buffer\n");
        goto done;
    }
    for (index = 0; index < section_count; index++) {
        uint32_t type;
        uint32_t flags;
        uint32_t address;
        uint32_t file_offset;
        uint32_t size;

        section_offset = section_table_offset + (uint32_t)index * section_size;
        if (!read_at(input, file_size, section_offset, section, sizeof(section))) {
            fprintf(stderr, "zzm_pack: cannot read ELF section table\n");
            goto done;
        }
        type = read_le32(section + 4);
        flags = read_le32(section + 8);
        address = read_le32(section + 12);
        file_offset = read_le32(section + 16);
        size = read_le32(section + 20);
        if ((flags & SHF_ALLOC) == 0 || type == SHT_NOBITS || size == 0)
            continue;
        if (!read_at(input, file_size, file_offset, image + address - slot->address,
                     size)) {
            fprintf(stderr, "zzm_pack: allocated section exceeds ELF file\n");
            goto done;
        }
    }

    image_size = image_end - slot->address;
    bss_size = allocation_end - image_end;
    memset(header, 0, sizeof(header));
    memcpy(header, ZZM_MAGIC, 4);
    write_le16(header + 4, ZZM_HEADER_SIZE);
    write_le16(header + 6, ZZM_FORMAT_VERSION);
    write_le32(header + 8, module_id);
    write_le32(header + 12, module_version);
    write_le32(header + 16, ZZM_MODULE_ABI_VERSION);
    write_le32(header + 20, slot->address);
    write_le32(header + 24, image_size);
    write_le32(header + 28, bss_size);
    write_le32(header + 32, entry);
    memcpy(header + 36, slot->name, strlen(slot->name) + 1);
    sha256_init(&sha256);
    sha256_update(&sha256, image, image_size);
    sha256_final(&sha256, header + 52);

    output = fopen(output_path, "wb");
    if (!output) {
        fprintf(stderr, "zzm_pack: cannot write %s\n", output_path);
        goto done;
    }
    if (fwrite(header, 1, sizeof(header), output) != sizeof(header) ||
        fwrite(image, 1, image_size, output) != image_size) {
        fprintf(stderr, "zzm_pack: cannot write %s\n", output_path);
        goto done;
    }
    if (fclose(output) != 0) {
        output = 0;
        fprintf(stderr, "zzm_pack: cannot write %s\n", output_path);
        goto done;
    }
    output = 0;
    result = 0;

done:
    if (output) fclose(output);
    if (input) fclose(input);
    free(image);
    return result == 0;
}

int main(int argc, char **argv)
{
    uint32_t module_id;
    uint32_t module_version;

    if (argc != 5 || !parse_u32(argv[1], &module_id) ||
        !parse_u32(argv[2], &module_version)) {
        fprintf(stderr, "usage: %s MODULE_ID MODULE_VERSION INPUT.elf OUTPUT.ZZM\n",
                argv[0]);
        return 2;
    }
    return pack_elf(module_id, module_version, argv[3], argv[4]) ? 0 : 1;
}
