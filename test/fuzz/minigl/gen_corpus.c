#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "mw_types.h"
#include "mw_parse.h"

static void ensure_dir(const char *dir)
{
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", dir);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Failed to open %s for writing\n", path);
        exit(1);
    }
    if (fwrite(data, 1, size, f) != size) {
        fprintf(stderr, "Failed to write %s\n", path);
        fclose(f);
        exit(1);
    }
    fclose(f);
}

static void write_to_both(const char *name, const uint8_t *data, size_t size)
{
    char p1[256], p2[256];
    snprintf(p1, sizeof(p1), "corpus/fuzz_submit/%s", name);
    snprintf(p2, sizeof(p2), "corpus/fuzz_submit_rix/%s", name);
    write_file(p1, data, size);
    write_file(p2, data, size);
}

static void write_req_both(const char *name, const uint8_t *data, size_t size)
{
    char p1[256], p2[256];
    snprintf(p1, sizeof(p1), "corpus/fuzz_request/%s", name);
    snprintf(p2, sizeof(p2), "corpus/fuzz_request_rix/%s", name);
    write_file(p1, data, size);
    write_file(p2, data, size);
}

static void write_ctx_both(const char *name, const uint8_t *data, size_t size)
{
    char p1[256], p2[256];
    snprintf(p1, sizeof(p1), "corpus/fuzz_context/%s", name);
    snprintf(p2, sizeof(p2), "corpus/fuzz_context_rix/%s", name);
    write_file(p1, data, size);
    write_file(p2, data, size);
}

/* Helper to wrap one or more commands into a wire packet with context dimension prefix */
typedef struct {
    uint8_t buf[65536];
    uint32_t cmd_count;
    uint32_t offset;
} PacketBuilder;

static void pb_init(PacketBuilder *b, uint8_t w, uint8_t h)
{
    memset(b->buf, 0, sizeof(b->buf));
    b->buf[0] = w;
    b->buf[1] = h;
    /* Packet header starts at offset 2 */
    mw_put_be32(b->buf + 2, MW_MAGIC);
    mw_put_be32(b->buf + 6, MW_VERSION);
    b->cmd_count = 0;
    b->offset = 18; /* 2 prefix + 16 packet header */
}

static void pb_add_cmd(PacketBuilder *b, uint32_t opcode, const uint8_t *payload, uint32_t payload_bytes)
{
    mw_put_be32(b->buf + b->offset, opcode);
    mw_put_be32(b->buf + b->offset + 4, payload_bytes);
    b->offset += 8;
    if (payload && payload_bytes) {
        memcpy(b->buf + b->offset, payload, payload_bytes);
        b->offset += payload_bytes;
    }
    b->cmd_count++;
}

static size_t pb_finish(PacketBuilder *b)
{
    uint32_t packet_bytes = b->offset - 2;
    mw_put_be32(b->buf + 10, packet_bytes);
    mw_put_be32(b->buf + 14, b->cmd_count);
    uint32_t failed_opcode = 0;
    const uint8_t *pkt = b->buf + 2;
    uint32_t res = mw_validate_packet_report(pkt, packet_bytes, &failed_opcode);
    if (res != MW_STATUS_OK) {
        fprintf(stderr, "pb_finish: bytes=%u magic=0x%08x ver=%u pkt_bytes=%u count=%u status=0x%x opcode=%u\n",
                packet_bytes, mw_be32(pkt), mw_be32(pkt + 4), mw_be32(pkt + 8), mw_be32(pkt + 12), res, failed_opcode);
    }
    assert(res == MW_STATUS_OK);
    return b->offset;
}

int main(void)
{
    ensure_dir("corpus/fuzz_submit");
    ensure_dir("corpus/fuzz_submit_rix");
    ensure_dir("corpus/fuzz_request");
    ensure_dir("corpus/fuzz_request_rix");
    ensure_dir("corpus/fuzz_context");
    ensure_dir("corpus/fuzz_context_rix");

    PacketBuilder b;
    uint8_t payload[4096];
    size_t size;

    /* 1. MW_CLEAR */
    pb_init(&b, 16, 16);
    memset(payload, 0, 24);
    mw_put_be32(payload, 3); /* color and depth mask */
    mw_put_bef(payload + 4, 1.0f);
    mw_put_bef(payload + 8, 0.0f);
    mw_put_bef(payload + 12, 0.0f);
    mw_put_bef(payload + 16, 1.0f);
    mw_put_bef(payload + 20, 1.0f);
    pb_add_cmd(&b, MW_CLEAR, payload, 24);
    size = pb_finish(&b);
    write_to_both("seed_clear.bin", b.buf, size);

    /* 2. MW_UPLOAD */
    pb_init(&b, 16, 16);
    memset(payload, 0, 24);
    mw_put_be32(payload, 0);     /* x */
    mw_put_be32(payload + 4, 0); /* y */
    mw_put_be32(payload + 8, 2); /* w */
    mw_put_be32(payload + 12, 1);/* h */
    mw_put_be32(payload + 16, 0xff0000ffU);
    mw_put_be32(payload + 20, 0x00ff00ffU);
    pb_add_cmd(&b, MW_UPLOAD, payload, 24);
    size = pb_finish(&b);
    write_to_both("seed_upload.bin", b.buf, size);

    /* 3. MW_STATE (depth test and blend) */
    pb_init(&b, 16, 16);
    memset(payload, 0, 12);
    mw_put_be32(payload, MW_DEPTH_TEST);
    mw_put_be32(payload + 4, 1);
    mw_put_be32(payload + 8, 1);
    pb_add_cmd(&b, MW_STATE, payload, 12);
    mw_put_be32(payload, MW_BLEND);
    mw_put_be32(payload + 4, 1);
    mw_put_be32(payload + 8, 1);
    pb_add_cmd(&b, MW_STATE, payload, 12);
    size = pb_finish(&b);
    write_to_both("seed_state.bin", b.buf, size);

    /* 4. MW_TEXTURE_DELETE */
    pb_init(&b, 16, 16);
    memset(payload, 0, 4);
    mw_put_be32(payload, 1);
    pb_add_cmd(&b, MW_TEXTURE_DELETE, payload, 4);
    size = pb_finish(&b);
    write_to_both("seed_texture_delete.bin", b.buf, size);

    /* 5. MW_TEXTURE_PARAM */
    pb_init(&b, 16, 16);
    memset(payload, 0, 12);
    mw_put_be32(payload, 0x0de1); /* GL_TEXTURE_2D */
    mw_put_be32(payload + 4, 0x2800); /* GL_TEXTURE_MAG_FILTER */
    mw_put_be32(payload + 8, 0x2600); /* GL_NEAREST */
    pb_add_cmd(&b, MW_TEXTURE_PARAM, payload, 12);
    size = pb_finish(&b);
    write_to_both("seed_texture_param.bin", b.buf, size);

    /* 6. MW_TEXTURE_COPY */
    pb_init(&b, 16, 16);
    memset(payload, 0, 40);
    mw_put_be32(payload, 1);       /* id */
    mw_put_be32(payload + 4, 0);   /* level */
    mw_put_be32(payload + 8, 0x1908); /* internalformat */
    mw_put_be32(payload + 28, 2);  /* w */
    mw_put_be32(payload + 32, 2);  /* h */
    pb_add_cmd(&b, MW_TEXTURE_COPY, payload, 40);
    size = pb_finish(&b);
    write_to_both("seed_texture_copy.bin", b.buf, size);

    /* 7. MW_SYNC */
    pb_init(&b, 16, 16);
    memset(payload, 0, 4);
    mw_put_be32(payload, 0);
    pb_add_cmd(&b, MW_SYNC, payload, 4);
    size = pb_finish(&b);
    write_to_both("seed_sync.bin", b.buf, size);

    /* 8. MW_TARGET */
    pb_init(&b, 16, 16);
    memset(payload, 0, 24);
    mw_put_be32(payload, 16);
    mw_put_be32(payload + 4, 16);
    mw_put_be32(payload + 8, 2);
    mw_put_be32(payload + 12, 24);
    mw_put_be32(payload + 16, 16);
    mw_put_be32(payload + 20, 16);
    pb_add_cmd(&b, MW_TARGET, payload, 24);
    size = pb_finish(&b);
    write_to_both("seed_target.bin", b.buf, size);

    /* 9. MW_PRESENT */
    pb_init(&b, 16, 16);
    pb_add_cmd(&b, MW_PRESENT, NULL, 0);
    size = pb_finish(&b);
    write_to_both("seed_present.bin", b.buf, size);

    /* 10. MW_DRAW_OBJECT */
    pb_init(&b, 16, 16);
    memset(payload, 0, 8 + 3 * MW_OBJECT_BYTES);
    mw_put_be32(payload, 4); /* GL_TRIANGLES */
    mw_put_be32(payload + 4, 3);
    uint8_t *v = payload + 8;
    for (int i = 0; i < 3; ++i) {
        mw_put_bef(v, i == 0 ? -1.0f : i == 1 ? 1.0f : 0.0f);
        mw_put_bef(v + 4, i == 2 ? 1.0f : -1.0f);
        mw_put_bef(v + 8, 0.0f);
        mw_put_bef(v + 12, 1.0f);
        mw_put_bef(v + 16, 1.0f);
        mw_put_bef(v + 20, 1.0f);
        mw_put_bef(v + 24, 1.0f);
        mw_put_bef(v + 28, 1.0f);
        v += MW_OBJECT_BYTES;
    }
    pb_add_cmd(&b, MW_DRAW_OBJECT, payload, 8 + 3 * MW_OBJECT_BYTES);
    size = pb_finish(&b);
    write_to_both("seed_draw_object.bin", b.buf, size);

    /* 11. MW_DRAW_PACKED */
    pb_init(&b, 16, 16);
    uint32_t pack_fmt = (1U << MW_PACK_POSITION) | (1U << MW_PACK_COLOR);
    uint32_t consts = 0, words = 0;
    assert(mw_packed_layout(pack_fmt, &consts, &words));
    uint32_t pld = 12 + 4 * (consts + 3 * words);
    memset(payload, 0, pld);
    mw_put_be32(payload, 4); /* GL_TRIANGLES */
    mw_put_be32(payload + 4, 3);
    mw_put_be32(payload + 8, pack_fmt);
    v = payload + 12 + consts * 4;
    for (int i = 0; i < 3; ++i) {
        mw_put_bef(v, i == 0 ? -1.0f : i == 1 ? 1.0f : 0.0f);
        mw_put_bef(v + 4, i == 2 ? 1.0f : -1.0f);
        mw_put_bef(v + 8, 0.0f);
        mw_put_bef(v + 12, 1.0f);
        mw_put_bef(v + 16, 0.5f);
        mw_put_bef(v + 20, 0.5f);
        v += words * 4;
    }
    pb_add_cmd(&b, MW_DRAW_PACKED, payload, pld);
    size = pb_finish(&b);
    write_to_both("seed_draw_packed.bin", b.buf, size);

    /* 12. MW_DRAW_LOWERED */
    pb_init(&b, 16, 16);
    memset(payload, 0, 16 + 3 * MW_OBJECT_BYTES);
    mw_put_be32(payload, 4); /* GL_TRIANGLES */
    mw_put_be32(payload + 4, 3);
    mw_put_be32(payload + 8, 0);
    mw_put_be32(payload + 12, 0);
    v = payload + 16;
    for (int i = 0; i < 3; ++i) {
        mw_put_bef(v, i == 0 ? -1.0f : i == 1 ? 1.0f : 0.0f);
        mw_put_bef(v + 4, i == 2 ? 1.0f : -1.0f);
        mw_put_bef(v + 8, 0.0f);
        mw_put_bef(v + 12, 1.0f);
        mw_put_bef(v + 16, 1.0f);
        mw_put_bef(v + 20, 1.0f);
        mw_put_bef(v + 24, 1.0f);
        mw_put_bef(v + 28, 1.0f);
        v += MW_OBJECT_BYTES;
    }
    pb_add_cmd(&b, MW_DRAW_LOWERED, payload, 16 + 3 * MW_OBJECT_BYTES);
    size = pb_finish(&b);
    write_to_both("seed_draw_lowered.bin", b.buf, size);

    /* 13. MW_TEXTURE_IMAGE_RAW */
    pb_init(&b, 16, 16);
    memset(payload, 0, 56 + 16);
    mw_put_be32(payload, 1);       /* name */
    mw_put_be32(payload + 4, 0);   /* level */
    mw_put_be32(payload + 8, 0x1908); /* internalformat */
    mw_put_be32(payload + 12, 2);  /* width */
    mw_put_be32(payload + 16, 2);  /* height */
    mw_put_be32(payload + 20, 0x1908); /* format */
    mw_put_be32(payload + 24, 0x1401); /* type GL_UNSIGNED_BYTE */
    mw_put_be32(payload + 28, 4);  /* alignment */
    mw_put_be32(payload + 52, 16); /* source bytes */
    for (int i = 0; i < 16; ++i) payload[56 + i] = 0x80;
    pb_add_cmd(&b, MW_TEXTURE_IMAGE_RAW, payload, 56 + 16);
    size = pb_finish(&b);
    write_to_both("seed_texture_image_raw.bin", b.buf, size);

    /* 14. MW_TEXTURE_SUB_RAW */
    pb_init(&b, 16, 16);
    memset(payload, 0, 60 + 16);
    mw_put_be32(payload, 1);       /* name */
    mw_put_be32(payload + 4, 0);   /* level */
    mw_put_be32(payload + 8, 0);   /* xoffset */
    mw_put_be32(payload + 12, 0);  /* yoffset */
    mw_put_be32(payload + 16, 2);  /* width */
    mw_put_be32(payload + 20, 2);  /* height */
    mw_put_be32(payload + 24, 0x1908); /* format */
    mw_put_be32(payload + 28, 0x1401); /* type */
    mw_put_be32(payload + 32, 4);  /* alignment */
    mw_put_be32(payload + 56, 16); /* source */
    for (int i = 0; i < 16; ++i) payload[60 + i] = 0xff;
    pb_add_cmd(&b, MW_TEXTURE_SUB_RAW, payload, 60 + 16);
    size = pb_finish(&b);
    write_to_both("seed_texture_sub_raw.bin", b.buf, size);

    /* 15. MW_DRAW_ARRAY_RAW */
    pb_init(&b, 16, 16);
    memset(payload, 0, 32 + 20 + 24);
    mw_put_be32(payload, 4);       /* GL_TRIANGLES */
    mw_put_be32(payload + 4, 3);   /* count */
    mw_put_be32(payload + 8, 0);   /* group */
    mw_put_be32(payload + 12, 0);  /* flags */
    mw_put_be32(payload + 16, 1);  /* streams */
    mw_put_be32(payload + 20, 0);  /* index_type */
    mw_put_be32(payload + 24, 0);  /* index_bytes */
    mw_put_be32(payload + 28, 0);
    /* stream 0: slot 0 (pos), 2 components, GL_FLOAT, elem 8, data 24 */
    mw_put_be32(payload + 32, 0);
    mw_put_be32(payload + 36, 2);
    mw_put_be32(payload + 40, 0x1406);
    mw_put_be32(payload + 44, 8);
    mw_put_be32(payload + 48, 24);
    v = payload + 52;
    for (int i = 0; i < 3; ++i) {
        mw_put_bef(v, i == 0 ? -1.0f : i == 1 ? 1.0f : 0.0f);
        mw_put_bef(v + 4, i == 2 ? 1.0f : -1.0f);
        v += 8;
    }
    pb_add_cmd(&b, MW_DRAW_ARRAY_RAW, payload, 32 + 20 + 24);
    size = pb_finish(&b);
    write_to_both("seed_draw_array_raw.bin", b.buf, size);

    /* 16. MW_COLOR_TABLE */
    pb_init(&b, 16, 16);
    memset(payload, 0, 28 + 8);
    mw_put_be32(payload, 0x80d0);  /* GL_SHARED_TEXTURE_PALETTE_EXT */
    mw_put_be32(payload + 4, 0x1908);
    mw_put_be32(payload + 12, 2);  /* width */
    mw_put_be32(payload + 16, 0x1908); /* format */
    mw_put_be32(payload + 20, 0x1401); /* type */
    mw_put_be32(payload + 24, 8);  /* source */
    payload[28] = 0xff; payload[31] = 0xff;
    payload[32] = 0x00; payload[35] = 0xff;
    pb_add_cmd(&b, MW_COLOR_TABLE, payload, 28 + 8);
    size = pb_finish(&b);
    write_to_both("seed_color_table.bin", b.buf, size);

    /* 17. MW_DRAW_PACKED_INDEXED */
    pb_init(&b, 16, 16);
    uint32_t idx_consts = 0, idx_words = 0;
    assert(mw_packed_layout(pack_fmt, &idx_consts, &idx_words));
    uint32_t idx_bytes = ((3 * 2 + 3) & ~3);
    uint32_t pld_idx = 16 + 4 * (idx_consts + 3 * idx_words) + idx_bytes;
    memset(payload, 0, pld_idx);
    mw_put_be32(payload, 4);       /* GL_TRIANGLES */
    mw_put_be32(payload + 4, 3);   /* v_count */
    mw_put_be32(payload + 8, 3);   /* idx_count */
    mw_put_be32(payload + 12, pack_fmt);
    v = payload + 16 + idx_consts * 4;
    for (int i = 0; i < 3; ++i) {
        mw_put_bef(v, i == 0 ? -1.0f : i == 1 ? 1.0f : 0.0f);
        mw_put_bef(v + 4, i == 2 ? 1.0f : -1.0f);
        mw_put_bef(v + 8, 0.0f);
        mw_put_bef(v + 12, 1.0f);
        mw_put_bef(v + 16, 1.0f);
        mw_put_bef(v + 20, 1.0f);
        v += idx_words * 4;
    }
    mw_put_be16(v, 0);
    mw_put_be16(v + 2, 1);
    mw_put_be16(v + 4, 2);
    pb_add_cmd(&b, MW_DRAW_PACKED_INDEXED, payload, pld_idx);
    size = pb_finish(&b);
    write_to_both("seed_draw_packed_indexed.bin", b.buf, size);

    /* 18. MW_PRESENT_SPAN */
    pb_init(&b, 16, 16);
    memset(payload, 0, 48);
    mw_put_be32(payload, MW_BLIT);
    mw_put_be32(payload + 4, 4);   /* rgb565 */
    mw_put_be32(payload + 8, 4);   /* pitch */
    mw_put_be32(payload + 12, 0);  /* frame_x */
    mw_put_be32(payload + 16, 0);  /* frame_y */
    mw_put_be32(payload + 20, 1);  /* count */
    mw_put_be32(payload + 24, 0);  /* rx0 */
    mw_put_be32(payload + 28, 0);  /* ry0 */
    mw_put_be32(payload + 32, 2);  /* rx1 */
    mw_put_be32(payload + 36, 2);  /* ry1 */
    mw_put_be32(payload + 40, 0x1000000); /* card offset */
    mw_put_be32(payload + 44, 16);        /* span bytes */
    pb_add_cmd(&b, MW_PRESENT_SPAN, payload, 48);
    size = pb_finish(&b);
    write_to_both("seed_present_span.bin", b.buf, size);

    /* 19. Multi-command frame sequence: CLEAR + DRAW + PRESENT */
    pb_init(&b, 16, 16);
    memset(payload, 0, 24);
    mw_put_be32(payload, 3);
    mw_put_bef(payload + 4, 0.2f);
    mw_put_bef(payload + 8, 0.3f);
    mw_put_bef(payload + 12, 0.4f);
    mw_put_bef(payload + 16, 1.0f);
    mw_put_bef(payload + 20, 1.0f);
    pb_add_cmd(&b, MW_CLEAR, payload, 24);

    memset(payload, 0, 8 + 3 * MW_OBJECT_BYTES);
    mw_put_be32(payload, 4);
    mw_put_be32(payload + 4, 3);
    v = payload + 8;
    for (int i = 0; i < 3; ++i) {
        mw_put_bef(v, i == 0 ? -0.8f : i == 1 ? 0.8f : 0.0f);
        mw_put_bef(v + 4, i == 2 ? 0.8f : -0.8f);
        mw_put_bef(v + 8, 0.0f);
        mw_put_bef(v + 12, 1.0f);
        mw_put_bef(v + 16, 1.0f);
        mw_put_bef(v + 20, 0.0f);
        mw_put_bef(v + 24, 0.0f);
        mw_put_bef(v + 28, 1.0f);
        v += MW_OBJECT_BYTES;
    }
    pb_add_cmd(&b, MW_DRAW_OBJECT, payload, 8 + 3 * MW_OBJECT_BYTES);
    pb_add_cmd(&b, MW_PRESENT, NULL, 0);
    size = pb_finish(&b);
    write_to_both("seed_frame_sequence.bin", b.buf, size);

    /* Requests corpus */
    /* Helper to build a request seed: [w, h, submit_len, submit_packet..., request_bytes...] */
    uint8_t req_buf[4096];

    /* req 1: MW_PLUGIN_VERSION */
    req_buf[0] = 16;
    req_buf[1] = 16;
    req_buf[2] = 0; /* no submit prefix */
    mw_put_be32(req_buf + 3, MW_PLUGIN_VERSION);
    write_req_both("seed_req_version.bin", req_buf, 7);

    /* req 2: MW_READ_PIXELS with clear+present prefix */
    pb_init(&b, 16, 16);
    memset(payload, 0, 24);
    mw_put_be32(payload, 3);
    mw_put_bef(payload + 4, 1.0f);
    mw_put_bef(payload + 16, 1.0f);
    mw_put_bef(payload + 20, 1.0f);
    pb_add_cmd(&b, MW_CLEAR, payload, 24);
    pb_add_cmd(&b, MW_PRESENT, NULL, 0);
    size_t submit_bytes = pb_finish(&b) - 2; /* exclude w, h */

    req_buf[0] = 16;
    req_buf[1] = 16;
    req_buf[2] = (uint8_t)submit_bytes;
    memcpy(req_buf + 3, b.buf + 2, submit_bytes);
    uint8_t *rq = req_buf + 3 + submit_bytes;
    mw_put_be32(rq, MW_READ_PIXELS);
    mw_put_be32(rq + 4, 0x1908); /* GL_RGBA */
    mw_put_be32(rq + 8, 0);      /* x */
    mw_put_be32(rq + 12, 0);     /* y */
    mw_put_be32(rq + 16, 2);     /* w */
    mw_put_be32(rq + 20, 2);     /* h */
    write_req_both("seed_req_read_pixels.bin", req_buf, 3 + submit_bytes + 24);

    /* req 3: MW_BLIT */
    mw_put_be32(rq, MW_BLIT);
    mw_put_be32(rq + 4, 4);      /* rgb565 */
    mw_put_be32(rq + 8, 4);      /* pitch */
    mw_put_be32(rq + 12, 0);
    mw_put_be32(rq + 16, 0);
    mw_put_be32(rq + 20, 1);     /* count */
    mw_put_be32(rq + 24, 0);
    mw_put_be32(rq + 28, 0);
    mw_put_be32(rq + 32, 2);
    mw_put_be32(rq + 36, 2);
    write_req_both("seed_req_blit.bin", req_buf, 3 + submit_bytes + 40);

    /* req 4: MW_CAPTURE */
    mw_put_be32(rq, MW_CAPTURE);
    write_req_both("seed_req_capture.bin", req_buf, 3 + submit_bytes + 40);

    /* Context switching corpus */
    /* format: [w1, h1, w2, h2, ctrl, len, chunk...] */
    uint8_t ctx_buf[4096];
    ctx_buf[0] = 16; ctx_buf[1] = 16;
    ctx_buf[2] = 16; ctx_buf[3] = 16;
    uint8_t *cp = ctx_buf + 4;

    /* op 1: c1 submit CLEAR */
    pb_init(&b, 16, 16);
    memset(payload, 0, 24);
    mw_put_be32(payload, 3);
    mw_put_bef(payload + 4, 1.0f);
    mw_put_bef(payload + 16, 1.0f);
    mw_put_bef(payload + 20, 1.0f);
    pb_add_cmd(&b, MW_CLEAR, payload, 24);
    pb_add_cmd(&b, MW_PRESENT, NULL, 0);
    size_t pkt1_len = pb_finish(&b) - 2;

    cp[0] = 0; /* c1 submit */
    cp[1] = (uint8_t)pkt1_len;
    memcpy(cp + 2, b.buf + 2, pkt1_len);
    cp += 2 + pkt1_len;

    /* op 2: c2 submit CLEAR */
    cp[0] = 1; /* c2 submit */
    cp[1] = (uint8_t)pkt1_len;
    memcpy(cp + 2, b.buf + 2, pkt1_len);
    cp += 2 + pkt1_len;

    /* op 3: c1 request READ_PIXELS */
    cp[0] = 2; /* c1 request */
    cp[1] = 24;
    mw_put_be32(cp + 2, MW_READ_PIXELS);
    mw_put_be32(cp + 6, 0x1908);
    mw_put_be32(cp + 10, 0);
    mw_put_be32(cp + 14, 0);
    mw_put_be32(cp + 18, 2);
    mw_put_be32(cp + 22, 2);
    cp += 26;

    /* op 4: c2 request READ_PIXELS */
    cp[0] = 3; /* c2 request */
    cp[1] = 24;
    mw_put_be32(cp + 2, MW_READ_PIXELS);
    mw_put_be32(cp + 6, 0x1908);
    mw_put_be32(cp + 10, 0);
    mw_put_be32(cp + 14, 0);
    mw_put_be32(cp + 18, 2);
    mw_put_be32(cp + 22, 2);
    cp += 26;

    write_ctx_both("seed_ctx_interleaved.bin", ctx_buf, (size_t)(cp - ctx_buf));

    printf("Generated seed corpus successfully!\n");
    return 0;
}
