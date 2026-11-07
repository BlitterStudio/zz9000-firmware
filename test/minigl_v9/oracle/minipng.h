/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal standalone PNG encoder for RGBA8 images.
 * Zero external dependencies: uses uncompressed DEFLATE blocks.
 */

#ifndef MINIPNG_H
#define MINIPNG_H

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace minipng {

inline uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len)
{
    static uint32_t table[256];
    static bool table_init = false;
    if (!table_init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        table_init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline uint32_t adler32(const uint8_t* data, size_t len)
{
    const uint32_t MOD_ADLER = 65521;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % MOD_ADLER;
        b = (b + a) % MOD_ADLER;
    }
    return (b << 16) | a;
}

inline void put_be32(std::vector<uint8_t>& buf, uint32_t v)
{
    buf.push_back((uint8_t)(v >> 24));
    buf.push_back((uint8_t)(v >> 16));
    buf.push_back((uint8_t)(v >> 8));
    buf.push_back((uint8_t)v);
}

inline void put_le16(std::vector<uint8_t>& buf, uint16_t v)
{
    buf.push_back((uint8_t)v);
    buf.push_back((uint8_t)(v >> 8));
}

inline void write_chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t len)
{
    put_be32(out, (uint32_t)len);
    size_t type_pos = out.size();
    out.push_back((uint8_t)type[0]);
    out.push_back((uint8_t)type[1]);
    out.push_back((uint8_t)type[2]);
    out.push_back((uint8_t)type[3]);
    if (len > 0 && data != nullptr)
        out.insert(out.end(), data, data + len);
    uint32_t crc = crc32_update(0, &out[type_pos], len + 4);
    put_be32(out, crc);
}

inline bool write_rgba_png(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba)
{
    std::vector<uint8_t> png;
    // 8-byte signature
    const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    png.insert(png.end(), sig, sig + 8);

    // IHDR
    std::vector<uint8_t> ihdr(13);
    ihdr[0] = (uint8_t)(width >> 24);
    ihdr[1] = (uint8_t)(width >> 16);
    ihdr[2] = (uint8_t)(width >> 8);
    ihdr[3] = (uint8_t)width;
    ihdr[4] = (uint8_t)(height >> 24);
    ihdr[5] = (uint8_t)(height >> 16);
    ihdr[6] = (uint8_t)(height >> 8);
    ihdr[7] = (uint8_t)height;
    ihdr[8] = 8; // 8 bits per channel
    ihdr[9] = 6; // RGBA
    ihdr[10] = 0; // compression (deflate)
    ihdr[11] = 0; // filter (standard)
    ihdr[12] = 0; // no interlace
    write_chunk(png, "IHDR", ihdr.data(), ihdr.size());

    // Raw scanlines with filter byte 0
    size_t raw_len = (size_t)height * (1 + (size_t)width * 4);
    std::vector<uint8_t> raw;
    raw.reserve(raw_len);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); // filter: None
        const uint8_t* row = rgba + (size_t)y * width * 4;
        raw.insert(raw.end(), row, row + width * 4);
    }

    // IDAT with uncompressed zlib/deflate
    std::vector<uint8_t> zstream;
    zstream.push_back(0x78); // zlib header
    zstream.push_back(0x01); // default / no compression
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t chunk_len = std::min<size_t>(65535, raw.size() - pos);
        bool is_final = (pos + chunk_len == raw.size());
        zstream.push_back(is_final ? 0x01 : 0x00);
        put_le16(zstream, (uint16_t)chunk_len);
        put_le16(zstream, (uint16_t)~chunk_len);
        zstream.insert(zstream.end(), &raw[pos], &raw[pos + chunk_len]);
        pos += chunk_len;
    }
    uint32_t adler = adler32(raw.data(), raw.size());
    put_be32(zstream, adler);
    write_chunk(png, "IDAT", zstream.data(), zstream.size());

    // IEND
    write_chunk(png, "IEND", nullptr, 0);

    std::ofstream f(path, std::ios::binary);
    if (!f.is_open())
        return false;
    f.write(reinterpret_cast<const char*>(png.data()), png.size());
    return true;
}

} // namespace minipng

#endif // MINIPNG_H
