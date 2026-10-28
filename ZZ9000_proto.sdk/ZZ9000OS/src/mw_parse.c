/*
 * ZZ9000 MiniGL Wire Protocol v9 Packet Parser & Validator
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mw_parse.h"
#include <math.h>

static int mw_isfinite(float f)
{
    return isfinite(f);
}

static int mw_finite(const uint8_t *p, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (!mw_isfinite(mw_bef(p + i * 4)))
            return 0;
    }
    return 1;
}

static int mw_compare(uint32_t v)
{
    return v >= 0x200 && v <= 0x207;
}

static int mw_factor(uint32_t v)
{
    return v <= 1 || (v >= 0x300 && v <= 0x308);
}

static int mw_buffer(uint32_t v)
{
    return v == 0 || v == 0x400 || v == 0x402 || v == 0x404 || v == 0x405 || v == 0x408;
}

static int mw_environment(uint32_t v)
{
    return v == 0x1e01 || v == 0x2100 || v == 0x2101 || v == 0xbe2 || v == 0x104;
}

static int mw_internal_format(uint32_t v)
{
    return v == 1 || v == 2 || v == 3 || v == 4 ||
           v == 0x1906 || v == 0x1907 || v == 0x1908 || v == 0x1909 || v == 0x190a ||
           v == 0x8049 || v == 0x803c || v == 0x8040 || v == 0x8045 || v == 0x804b ||
           v == 0x8050 || v == 0x8051 || v == 0x8057 || v == 0x8058;
}

static int mw_raw_layout(uint32_t format, uint32_t type, uint32_t *pixel_bytes)
{
    uint32_t components;

    switch (format) {
    case MW_RAW_ARGB_4444:
        if (type == 0x8033) {
            *pixel_bytes = 2;
            return 1;
        }
        return 0;
    case MW_RAW_UBYTE_ARGB:
        components = 4;
        break;
    case 0x1900:
    case 0x1903:
    case 0x1906:
    case 0x1909:
    case 0x8049:
        components = 1;
        break;
    case 0x190a:
        components = 2;
        break;
    case 0x1907:
        components = 3;
        break;
    case 0x1908:
    case 0x80e1:
        components = 4;
        break;
    default:
        return 0;
    }

    if (type == 0x8363) {
        if (format == 0x1907) {
            *pixel_bytes = 2;
            return 1;
        }
        return 0;
    }
    if (type == 0x8033) {
        if (format == 0x1908) {
            *pixel_bytes = 2;
            return 1;
        }
        return 0;
    }

    switch (type) {
    case 0x1400:
    case 0x1401:
        *pixel_bytes = components;
        return 1;
    case 0x1402:
    case 0x1403:
        *pixel_bytes = components * 2;
        return 1;
    case 0x1404:
    case 0x1405:
    case 0x1406:
        *pixel_bytes = components * 4;
        return 1;
    default:
        return 0;
    }
}

static int mw_raw(const uint8_t *p, size_t bytes, int sub)
{
    const size_t header = sub ? 60 : 56;
    const size_t unpack = sub ? 32 : 28;
    uint32_t width, height, format, type, alignment, row_length;
    uint32_t skip_rows, skip_pixels, swap, lsb, source, pixel;
    uint64_t source_width, row, need;

    if (bytes < header)
        return 0;

    width = mw_be32(p + (sub ? 16 : 12));
    height = mw_be32(p + (sub ? 20 : 16));
    format = mw_be32(p + (sub ? 24 : 20));
    type = mw_be32(p + (sub ? 28 : 24));
    alignment = mw_be32(p + unpack);
    row_length = mw_be32(p + unpack + 4);
    skip_rows = mw_be32(p + unpack + 8);
    skip_pixels = mw_be32(p + unpack + 12);
    swap = mw_be32(p + unpack + 16);
    lsb = mw_be32(p + unpack + 20);
    source = mw_be32(p + header - 4);

    if (!width || !height || width > MW_MAX_DIMENSION || height > MW_MAX_DIMENSION ||
        !mw_raw_layout(format, type, &pixel) ||
        !(alignment == 1 || alignment == 2 || alignment == 4 || alignment == 8) ||
        swap > 1 || lsb > 1)
        return 0;

    source_width = row_length ? row_length : width;
    if (source_width < (uint64_t)skip_pixels + width)
        return 0;

    row = ((source_width * pixel + alignment - 1) / alignment) * alignment;
    need = (uint64_t)skip_rows * row + (uint64_t)skip_pixels * pixel +
           ((uint64_t)height - 1) * row + (uint64_t)width * pixel;

    return ((!sub && source == 0) || need == source) &&
           (header + (( (uint64_t)source + 3) & ~(uint64_t)3) == bytes);
}

static int mw_color_table(const uint8_t *p, size_t bytes)
{
    uint32_t target, width, format, type, source, pixel;

    if (bytes < 28)
        return 0;

    target = mw_be32(p);
    width = mw_be32(p + 12);
    format = mw_be32(p + 16);
    type = mw_be32(p + 20);
    source = mw_be32(p + 24);

    return (target == 0x80d0 || target == 0x81fb) && width <= 256 &&
           mw_raw_layout(format, type, &pixel) &&
           (format == 0x1907 || format == 0x1908) && type == 0x1401 &&
           source == (uint64_t)width * pixel &&
           (28 + (( (uint64_t)source + 3) & ~(uint64_t)3) == bytes);
}

static int mw_array_scalar(uint32_t type, uint32_t *bytes)
{
    switch (type) {
    case 0x1400:
    case 0x1401:
    case MW_RAW_UBYTE_ARGB:
    case MW_RAW_UBYTE_BGRA:
        *bytes = 1;
        return 1;
    case 0x1402:
    case 0x1403:
        *bytes = 2;
        return 1;
    case 0x1404:
    case 0x1405:
    case 0x1406:
        *bytes = 4;
        return 1;
    case 0x140a:
        *bytes = 8;
        return 1;
    default:
        return 0;
    }
}

static int mw_raw_array(const uint8_t *p, size_t bytes)
{
    uint32_t mode, count, group, flags, streams, index_type, index_bytes;
    size_t offset;
    uint32_t seen, i, index_scalar;

    if (bytes < 32)
        return 0;

    mode = mw_be32(p);
    count = mw_be32(p + 4);
    group = mw_be32(p + 8);
    flags = mw_be32(p + 12);
    streams = mw_be32(p + 16);
    index_type = mw_be32(p + 20);
    index_bytes = mw_be32(p + 24);

    if (mode > 6 || !count || count > MW_MAX_VERTICES || (mode == 1 && (count % 2)) ||
        (mode == 4 && (count % 3)) || group > 2 || flags || !streams || streams > 5 ||
        mw_be32(p + 28))
        return 0;

    offset = 32;
    seen = 0;
    for (i = 0; i < streams; ++i) {
        uint32_t slot, components, type, element, data, scalar, allowed;

        if (bytes - offset < 20)
            return 0;

        slot = mw_be32(p + offset);
        components = mw_be32(p + offset + 4);
        type = mw_be32(p + offset + 8);
        element = mw_be32(p + offset + 12);
        data = mw_be32(p + offset + 16);
        scalar = 0;

        allowed = (slot == 0) ? 0x1c : (slot == 1) ? 0x18 :
                  (slot == 4 || slot == 5) ? 0x1e : (slot == 6) ? 0x08 : 0;

        if (!allowed || (seen & (1U << slot)) || !(allowed & (1U << components)) ||
            !mw_array_scalar(type, &scalar) || element != components * scalar ||
            (slot == 6 && !(type == 0x1400 || type == 0x1402 || type == 0x1404 ||
                             type == 0x1406 || type == 0x140a)))
            return 0;

        if ((type == MW_RAW_UBYTE_ARGB || type == MW_RAW_UBYTE_BGRA) &&
            (slot != 1 || components != 4))
            return 0;

        seen |= (1U << slot);
        offset += 20;

        if (data == 0) {
            if ((type == MW_RAW_UBYTE_ARGB || type == MW_RAW_UBYTE_BGRA) || bytes - offset < 16)
                return 0;
            offset += 16;
        } else {
            if (data != (uint64_t)count * element || data > bytes - offset)
                return 0;
            offset += ((size_t)data + 3) & ~(size_t)3;
            if (offset > bytes)
                return 0;
        }
    }

    index_scalar = 0;
    if (!(seen & 1U) || (index_type == 0 && index_bytes) ||
        (index_type && (!(index_type == 0x1401 || index_type == 0x1403 || index_type == 0x1405) ||
                        !mw_array_scalar(index_type, &index_scalar) ||
                        index_bytes != (uint64_t)count * index_scalar)))
        return 0;

    return offset + (((size_t)index_bytes + 3) & ~(size_t)3) == bytes;
}

static int mw_rgba(const uint8_t *p, size_t bytes, size_t header, size_t dim_offset)
{
    uint32_t w, h;
    if (bytes < header)
        return 0;
    w = mw_be32(p + dim_offset);
    h = mw_be32(p + dim_offset + 4);
    return w && h && w <= MW_MAX_DIMENSION && h <= MW_MAX_DIMENSION &&
           header + (uint64_t)w * h * 4 == bytes;
}

static int mw_valid_state(const uint8_t *p, size_t bytes)
{
    static const unsigned char counts[] = {
        0, 1, 1, 1, 2, 1, 4, 1, 1, 2, 1, 1, 1,
        1, 4, 4, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1,
        3, 4, 4, 1, 1, 1, 1, 4, 1, 1,
        16, 16, 0, 16, 16, 20, 20, 0, /* key 38 and key 43 retired */
        1, 26, 6, 18, 3, 1, 5
    };
    uint32_t key, n;
    const uint8_t *v;

    if (bytes < 8)
        return 0;

    key = mw_be32(p);
    n = mw_be32(p + 4);

    if (!key || key >= sizeof(counts) || n != counts[key] || bytes != 8 + n * 4)
        return 0;

    v = p + 8;
    switch (key) {
    case MW_DEPTH_TEST:
    case MW_DEPTH_MASK:
    case MW_BLEND:
    case MW_ALPHA_TEST:
    case MW_CULL:
    case MW_SCISSOR:
    case MW_OFFSET_FILL:
    case MW_OFFSET_LINE:
    case MW_OFFSET_POINT:
    case MW_FOG:
    case MW_DITHER:
    case MW_POINT_SMOOTH:
    case MW_PERSPECTIVE:
        return mw_be32(v) <= 1;
    case MW_DEPTH_FUNC:
        return mw_compare(mw_be32(v));
    case MW_DEPTH_RANGE:
        return mw_finite(v, 2) && mw_bef(v) >= 0.0f && mw_bef(v) <= 1.0f &&
               mw_bef(v + 4) >= 0.0f && mw_bef(v + 4) <= 1.0f;
    case MW_BLEND_FACTORS:
        return mw_factor(mw_be32(v)) && mw_factor(mw_be32(v + 4)) &&
               mw_factor(mw_be32(v + 8)) && mw_factor(mw_be32(v + 12)) &&
               mw_be32(v + 4) != 0x308 && mw_be32(v + 12) != 0x308;
    case MW_BLEND_EQUATION:
        return mw_be32(v) == 0x8006 || mw_be32(v) == 0x8007 || mw_be32(v) == 0x8008 ||
               mw_be32(v) == 0x800a || mw_be32(v) == 0x800b;
    case MW_ALPHA_FUNC:
        return mw_compare(mw_be32(v)) && mw_finite(v + 4, 1) &&
               mw_bef(v + 4) >= 0.0f && mw_bef(v + 4) <= 1.0f;
    case MW_CULL_FACE:
        return mw_be32(v) == 0x404 || mw_be32(v) == 0x405 || mw_be32(v) == 0x408;
    case MW_FRONT_FACE:
        return mw_be32(v) == 0x900 || mw_be32(v) == 0x901;
    case MW_SCISSOR_BOX:
    case MW_VIEWPORT:
        return mw_be32(v + 8) <= 0x7fffffff && mw_be32(v + 12) <= 0x7fffffff;
    case MW_COLOR_MASK:
        return mw_be32(v) <= 1 && mw_be32(v + 4) <= 1 &&
               mw_be32(v + 8) <= 1 && mw_be32(v + 12) <= 1;
    case MW_POLYGON_OFFSET:
        return mw_finite(v, 2);
    case MW_LINE_WIDTH:
    case MW_POINT_SIZE:
        return mw_finite(v, 1) && mw_bef(v) > 0.0f;
    case MW_SHADE_MODEL:
        return mw_be32(v) == 0x1d00 || mw_be32(v) == 0x1d01;
    case MW_FOG_MODE:
        return mw_be32(v) == 0x800 || mw_be32(v) == 0x801 || mw_be32(v) == 0x2601;
    case MW_FOG_PARAMS:
        return mw_finite(v, 3) && mw_bef(v) >= 0.0f;
    case MW_FOG_COLOR:
        return mw_finite(v, 4);
    case MW_TEXTURE_STAGE:
        return mw_be32(v) < 2 && mw_be32(v + 8) <= 1 && mw_environment(mw_be32(v + 12));
    case MW_DRAW_BUFFER:
    case MW_READ_BUFFER:
        return mw_buffer(mw_be32(v));
    case MW_Z_OFFSET:
        return mw_finite(v, 1);
    case MW_MODELVIEW:
    case MW_PROJECTION:
    case MW_TEXTURE_MATRIX0:
    case MW_TEXTURE_MATRIX1:
        return mw_finite(v, 16);
    case MW_TEXGEN0:
    case MW_TEXGEN1:
        return mw_be32(v) <= 1 && mw_be32(v + 4) <= 1 &&
               (mw_be32(v + 8) == 0x2400 || mw_be32(v + 8) == 0x2401 || mw_be32(v + 8) == 0x2402) &&
               (mw_be32(v + 12) == 0x2400 || mw_be32(v + 12) == 0x2401 || mw_be32(v + 12) == 0x2402) &&
               mw_finite(v + 16, 16);
    case MW_LIGHTING:
        return mw_be32(v) <= 1;
    case MW_LIGHT: {
        float exponent, cutoff;
        if (!(mw_be32(v) < 8 && mw_be32(v + 4) <= 1 && mw_finite(v + 8, 24)))
            return 0;
        exponent = mw_bef(v + 21 * 4);
        cutoff = mw_bef(v + 22 * 4);
        return exponent >= 0.0f && exponent <= 128.0f &&
               ((cutoff >= 0.0f && cutoff <= 90.0f) || cutoff == 180.0f) &&
               mw_bef(v + 23 * 4) >= 0.0f && mw_bef(v + 24 * 4) >= 0.0f &&
               mw_bef(v + 25 * 4) >= 0.0f;
    }
    case MW_LIGHT_MODEL:
        return mw_finite(v, 4) && mw_be32(v + 16) <= 1 && mw_be32(v + 20) <= 1;
    case MW_MATERIAL:
        return mw_be32(v) <= 1 && mw_finite(v + 4, 17) &&
               mw_bef(v + 17 * 4) >= 0.0f && mw_bef(v + 17 * 4) <= 128.0f;
    case MW_COLOR_MATERIAL:
        return mw_be32(v) <= 1 &&
               (mw_be32(v + 4) == 0x404 || mw_be32(v + 4) == 0x405 || mw_be32(v + 4) == 0x408) &&
               (mw_be32(v + 8) == 0x1200 || mw_be32(v + 8) == 0x1201 || mw_be32(v + 8) == 0x1202 ||
                mw_be32(v + 8) == 0x1600 || mw_be32(v + 8) == 0x1602);
    case MW_NORMALIZE:
        return mw_be32(v) <= 1;
    case MW_TEXTURE_ENV_COLOR:
        return mw_be32(v) < 2 && mw_finite(v + 4, 4);
    default:
        return 0;
    }
}

static int mw_valid_command(uint32_t opcode, const uint8_t *p, size_t bytes)
{
    switch (opcode) {
    case MW_CLEAR:
        return bytes == 24 && !(mw_be32(p) & ~3U) && mw_finite(p + 4, 5);

    case MW_DRAW_OBJECT:
    case MW_DRAW_PACKED: {
        uint32_t mode, count;
        uint64_t expected;

        if (bytes < 8)
            return 0;
        mode = mw_be32(p);
        count = mw_be32(p + 4);

        if (opcode == MW_DRAW_PACKED) {
            uint32_t constants, words;
            if (bytes < 12 || !mw_packed_layout(mw_be32(p + 8), &constants, &words))
                return 0;
            expected = 12 + 4 * ((uint64_t)constants + (uint64_t)count * words);
        } else {
            expected = 8 + (uint64_t)count * MW_OBJECT_BYTES;
        }

        return mode <= 6 && count <= MW_MAX_VERTICES && count &&
               (mode != 1 || count % 2 == 0) && (mode != 4 || count % 3 == 0) &&
               bytes == expected;
    }

    case MW_DRAW_LOWERED: {
        uint32_t mode, count, flags;
        uint64_t mask_words;

        if (bytes < 16)
            return 0;
        mode = mw_be32(p);
        count = mw_be32(p + 4);
        flags = mw_be32(p + 12);
        mask_words = ((uint64_t)count + 31U) / 32U;

        return mode <= 9 && count && count <= MW_MAX_VERTICES &&
               mw_be32(p + 8) <= 2 && !(flags & ~0xf9U) &&
               ((flags >> MW_LOWERED_FRONT_SHIFT) & 3U) <= 2 &&
               ((flags >> MW_LOWERED_BACK_SHIFT) & 3U) <= 2 &&
               bytes == 16 + (uint64_t)count * MW_OBJECT_BYTES +
                        ((flags & MW_LOWERED_EDGE_MASK) ? 4 * mask_words : 0);
    }

    case MW_DRAW_ARRAY_RAW:
        return mw_raw_array(p, bytes);

    case MW_UPLOAD:
        return mw_rgba(p, bytes, 16, 8);

    case MW_STATE:
        return mw_valid_state(p, bytes);

    case MW_TEXTURE_IMAGE_RAW:
        return mw_be32(p + 4) <= 12 && mw_internal_format(mw_be32(p + 8)) && mw_raw(p, bytes, 0);

    case MW_TEXTURE_SUB_RAW:
        return mw_be32(p + 4) <= 12 && mw_raw(p, bytes, 1);

    case MW_COLOR_TABLE:
        return mw_color_table(p, bytes);

    case MW_TEXTURE_DELETE:
        return bytes == 4;

    case MW_TEXTURE_PARAM: {
        uint32_t pname, value;
        if (bytes != 12)
            return 0;
        pname = mw_be32(p + 4);
        value = mw_be32(p + 8);
        if (pname == 0x2800)
            return value == 0x2600 || value == 0x2601;
        if (pname == 0x2801)
            return value == 0x2600 || value == 0x2601 || value == 0x2700 ||
                   value == 0x2701 || value == 0x2702 || value == 0x2703;
        if (pname == 0x2802 || pname == 0x2803)
            return value == 0x2900 || value == 0x2901 || value == 0x812d || value == 0x812f;
        if (pname == 0x8191)
            return value <= 1;
        return pname == 0x8066 && mw_finite(p + 8, 1);
    }

    case MW_TEXTURE_COPY:
        return bytes == 40 && mw_be32(p + 4) <= 12 &&
               mw_internal_format(mw_be32(p + 8)) &&
               mw_be32(p + 28) <= MW_MAX_DIMENSION &&
               mw_be32(p + 32) <= MW_MAX_DIMENSION &&
               mw_be32(p + 36) <= 1;

    case MW_SYNC:
        return bytes == 4 && mw_be32(p) <= 1;

    case MW_TARGET:
        return bytes == 24 &&
               mw_be32(p) > 0 && mw_be32(p) <= MW_MAX_DIMENSION &&
               mw_be32(p + 4) > 0 && mw_be32(p + 4) <= MW_MAX_DIMENSION &&
               mw_be32(p + 8) >= 1 && mw_be32(p + 8) <= 3 &&
               (mw_be32(p + 12) == 16 || mw_be32(p + 12) == 24 || mw_be32(p + 12) == 32) &&
               mw_be32(p + 16) > 0 && mw_be32(p + 16) <= MW_MAX_DIMENSION &&
               mw_be32(p + 20) > 0 && mw_be32(p + 20) <= MW_MAX_DIMENSION;

    case MW_PRESENT:
        return bytes == 0;

    default:
        return 0;
    }
}

static uint32_t mw_validate_packet_impl(const uint8_t *packet, uint32_t bytes,
                                        uint32_t *failed_opcode)
{
    uint32_t count, i;
    size_t offset;

    if (failed_opcode)
        *failed_opcode = 0U;
    if (!packet || bytes < 16 || bytes > MW_MAX_PACKET_BYTES || (bytes & 3) != 0 ||
        mw_be32(packet) != MW_MAGIC || mw_be32(packet + 8) != bytes)
        return MW_STATUS_INVALID;
    if (mw_be32(packet + 4) != MW_VERSION)
        return MW_STATUS_INVALID;
    count = mw_be32(packet + 12);
    if (!count || count > MW_MAX_COMMANDS)
        return MW_STATUS_INVALID;

    offset = 16;
    for (i = 0; i < count; ++i) {
        uint32_t opcode, cmd_bytes;

        if (bytes - offset < 8)
            return MW_STATUS_INVALID;
        opcode = mw_be32(packet + offset);
        cmd_bytes = mw_be32(packet + offset + 4);
        offset += 8;
        if (cmd_bytes > bytes - offset || (cmd_bytes & 3) != 0 ||
            !mw_valid_command(opcode, packet + offset, cmd_bytes)) {
            if (failed_opcode)
                *failed_opcode = opcode;
            return MW_STATUS_INVALID;
        }
        offset += cmd_bytes;
    }
    return (offset == bytes) ? MW_STATUS_OK : MW_STATUS_INVALID;
}

uint32_t mw_validate_packet(const uint8_t *packet, uint32_t bytes)
{
    return mw_validate_packet_impl(packet, bytes, NULL);
}

uint32_t mw_validate_packet_report(const uint8_t *packet, uint32_t bytes,
                                   uint32_t *failed_opcode)
{
    return mw_validate_packet_impl(packet, bytes, failed_opcode);
}

uint32_t mw_parse_packet_report(const uint8_t *packet, uint32_t bytes,
                                mw_command_visitor visitor, void *user,
                                uint32_t *failed_opcode)
{
    uint32_t val_status, count, i;
    size_t offset;

    if (failed_opcode)
        *failed_opcode = 0U;
    val_status = mw_validate_packet_report(packet, bytes, failed_opcode);
    if (val_status != MW_STATUS_OK)
        return val_status;

    if (!visitor)
        return MW_STATUS_OK;

    count = mw_be32(packet + 12);
    offset = 16;
    for (i = 0; i < count; ++i) {
        mw_command cmd;

        cmd.opcode = mw_be32(packet + offset);
        cmd.bytes = mw_be32(packet + offset + 4);
        cmd.payload = packet + offset + 8;
        offset += 8 + cmd.bytes;

        if (!visitor(user, &cmd)) {
            if (failed_opcode)
                *failed_opcode = cmd.opcode;
            return MW_STATUS_FAILED;
        }
    }

    return MW_STATUS_OK;
}

uint32_t mw_parse_packet(const uint8_t *packet, uint32_t bytes,
                         mw_command_visitor visitor, void *user)
{
    return mw_parse_packet_report(packet, bytes, visitor, user, NULL);
}
