/*
 * Host-runner contracts that the templates do not cover: path rejection,
 * colon paths, message-ring backpressure, ENOSYS services, display layout,
 * async flip, and mailbox replacement. Exit status is the failing check.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"

#include <stdio.h>
#include <string.h>

static int fail(int code, const char *msg)
{
    fprintf(stderr, "contract: %s\n", msg);
    return code;
}

static int read_all(int fd, char *buf, uint32_t n)
{
    uint32_t done = 0;

    while (done < n) {
        uint32_t got = 0;
        int rc = zz_file_read(fd, buf + done, n - done, &got);
        if (rc != ZZ_OK || got == 0u)
            return rc != ZZ_OK ? rc : ZZ_EIO;
        done += got;
    }
    return ZZ_OK;
}

static int test_reject(void)
{
    int fd;
    char buf[8];
    uint32_t n = 0;
    const void *mapped = 0;
    uint32_t bytes = 0;
    zzapp_dirent ent;
    int list;
    int saw_ok = 0;
    int saw_dir = 0;
    int saw_dot = 0;

    if (zz_file_open("../outside", ZZ_FILE_READ) != ZZ_EACCES)
        return fail(2, "parent escape was not rejected");
    if (zz_file_open("foo/../../outside", ZZ_FILE_READ) != ZZ_EACCES)
        return fail(3, "nested parent escape was not rejected");
    if (zz_file_open("vol:data", ZZ_FILE_READ) != ZZ_EACCES)
        return fail(4, "colon path was not rejected");
    if (zz_file_open("missing-no-such", ZZ_FILE_READ) != ZZ_ENOENT)
        return fail(5, "missing file was not ENOENT");
    fd = zz_file_open("ok.txt", ZZ_FILE_READ);
    if (fd < 0)
        return fail(6, "ok.txt did not open");
    if (read_all(fd, buf, 3u) != ZZ_OK || memcmp(buf, "ok\n", 3) != 0)
        return fail(7, "ok.txt bytes");
    zz_file_close(fd);
    /* '//' is a parent step, so foo//bar.txt is root/bar.txt. */
    fd = zz_file_open("foo//bar.txt", ZZ_FILE_READ);
    if (fd < 0)
        return fail(8, "double-slash parent step failed");
    if (read_all(fd, buf, 4u) != ZZ_OK || memcmp(buf, "bar\n", 4) != 0)
        return fail(9, "double-slash resolved to the wrong file");
    zz_file_close(fd);
    if (zz_map_file("empty.txt", &mapped, &bytes) != ZZ_ENOMEM)
        return fail(10, "empty map_file was not ENOMEM");
    if (zz_map_file("big.bin", &mapped, &bytes) != ZZ_ENOSPC)
        return fail(11, "oversize map_file was not ENOSPC");
    if (zz_map_file("ok.txt", &mapped, &bytes) != ZZ_OK || bytes != 3u ||
        memcmp(mapped, "ok\n", 3) != 0)
        return fail(12, "map_file bytes");
    zz_unmap_file(mapped);
    list = zz_file_list_open(".");
    if (list < 0)
        return fail(13, "list_open");
    for (;;) {
        int rc = zz_file_list_next(list, &ent);
        if (rc == ZZ_ENOENT)
            break;
        if (rc != ZZ_OK)
            return fail(14, "list_next");
        if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0)
            saw_dot = 1;
        if (strcmp(ent.name, "ok.txt") == 0)
            saw_ok = 1;
        if (strcmp(ent.name, "foo") == 0 && ent.mode == 2u)
            saw_dir = 1;
    }
    zz_file_list_close(list);
    if (saw_dot)
        return fail(15, "list returned . or ..");
    if (!saw_ok)
        return fail(16, "list missed ok.txt");
    if (!saw_dir)
        return fail(21, "list did not report foo as a directory");
    fd = zz_file_open("round.bin", ZZ_FILE_WRITE | ZZ_FILE_CREATE);
    if (fd < 0)
        return fail(17, "create");
    if (zz_file_write(fd, "abcd", 4u, &n) != ZZ_OK || n != 4u)
        return fail(18, "write");
    {
        int64_t pos = -1;
        if (zz_file_seek(fd, 1, ZZ_SEEK_SET, &pos) != ZZ_OK || pos != 1)
            return fail(19, "seek");
    }
    if (read_all(fd, buf, 3u) != ZZ_OK || memcmp(buf, "bcd", 3) != 0)
        return fail(20, "read after seek");
    zz_file_close(fd);
    (void)n;
    return 0;
}

static int test_abs(void)
{
    int fd;
    char buf[8];

    fd = zz_file_open("vol:data", ZZ_FILE_READ);
    if (fd < 0)
        return fail(30, "abs colon path did not open");
    if (read_all(fd, buf, 6u) != ZZ_OK || memcmp(buf, "abs-ok", 6) != 0)
        return fail(31, "abs colon bytes");
    zz_file_close(fd);
    return 0;
}

static int test_input(void)
{
    zzapp_event ev;
    uint32_t n = 0;
    int saw_key = 0;
    int saw_break = 0;
    int i;

    for (i = 0; i < 8; i++) {
        if (zz_input_poll(&ev, 1u, &n) != ZZ_OK)
            return fail(40, "poll");
        if (n == 0u)
            break;
        if (ev.type == ZZ_EV_KEY_DOWN && ev.code == 0x22u)
            saw_key = 1;
        if (ev.type == ZZ_EV_BREAK && ev.code == ZZ_BREAK_CTRL_D)
            saw_break = 1;
    }
    if (!saw_key || !saw_break)
        return fail(41, "scripted key or break missing");
    return 0;
}

static int test_display(void)
{
    zzapp_surface s;
    uint32_t rgb = 0x00ff0000u;
    int idle;

    if (zz_display_open(4u, 2u, ZZ_FMT_RGB565, ZZ_DISPLAY_VSYNC, &s) != ZZ_OK)
        return fail(50, "rgb565 open");
    if (s.layout != ZZ_LAYOUT_RGB565_BE || !s.back || s.back == s.front)
        return fail(51, "rgb565 layout or buffers");
    if (zz_display_flip_async(s.id) != ZZ_OK)
        return fail(52, "async flip");
    idle = zz_display_flip_idle();
    if (idle != 0)
        return fail(53, "strict async was already idle");
    zz_yield();
    if (zz_display_flip_idle() != 1)
        return fail(54, "async flip did not complete on yield");
    if (zz_display_close(s.id) != ZZ_OK)
        return fail(55, "close");

    memset(&s, 0, sizeof s);
    if (zz_display_open(4u, 2u, ZZ_FMT_RGB565,
                        ZZ_DISPLAY_VSYNC | ZZ_DISPLAY_MAILBOX, &s) != ZZ_OK)
        return fail(56, "mailbox open");
    if (zz_display_flip_async(s.id) != ZZ_OK || zz_display_flip_idle() != 1)
        return fail(57, "mailbox idle");
    if (zz_display_mailbox_replaced() != 0u)
        return fail(58, "mailbox replaced before a replacement");
    if (zz_display_flip_async(s.id) != ZZ_OK)
        return fail(59, "second mailbox flip");
    if (zz_display_mailbox_replaced() != 1u)
        return fail(60, "mailbox replaced count");
    zz_display_close(s.id);

    if (zz_display_open(4u, 2u, ZZ_FMT_CLUT8, 0, &s) != ZZ_OK)
        return fail(61, "clut8 open");
    if (s.layout != ZZ_LAYOUT_CLUT8)
        return fail(62, "clut8 layout");
    if (zz_display_set_palette(s.id, 0u, 1u, &rgb) != ZZ_OK)
        return fail(63, "palette");
    if (zz_display_set_palette(s.id, 0u, 1u, 0) != ZZ_EINVAL)
        return fail(64, "null palette");
    zz_display_close(s.id);
    if (zz_display_open(2u, 2u, ZZ_FMT_BGRA8888, 0, &s) != ZZ_OK)
        return fail(65, "bgra open");
    if (s.layout != ZZ_LAYOUT_BGRA8888)
        return fail(66, "bgra layout");
    zz_display_close(s.id);
    return 0;
}

static int test_services(void)
{
    static const uint8_t abc[] = {'a', 'b', 'c'};
    static const uint8_t expect[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    uint8_t digest[32];
    uint8_t msg[32];
    void *p;
    uint32_t off = 0;
    zzapp_mem_info info;
    int i;
    int rc;

    if (zz_sha256(abc, 3u, digest) != ZZ_OK || memcmp(digest, expect, 32) != 0)
        return fail(70, "sha256");
    if (zz_decompress(ZZ_COMPRESS_ZLIB, 0, abc, 3u, digest, 32u, 0) != ZZ_ENOSYS)
        return fail(71, "decompress");
    if (zz_image_open(0, 0, 0) != ZZ_ENOSYS)
        return fail(72, "image");
    if (zz_audio_decode_open(ZZ_AUDIO_MP3, 0) != ZZ_ENOSYS)
        return fail(73, "audio decode");
    if (zz_gl_bind(1u) != 0)
        return fail(74, "gl bind");
    memset(msg, 0x5a, sizeof msg);
    for (i = 0; i < 64; i++) {
        rc = zz_msg_send(msg);
        if (rc != ZZ_OK)
            return fail(75, "msg send before full");
    }
    if (zz_msg_send(msg) != ZZ_EBUSY)
        return fail(76, "msg ring did not report EBUSY");
    if (zz_msg_poll(msg) != 1)
        return fail(77, "msg poll");
    if (zz_msg_send(msg) != ZZ_OK)
        return fail(78, "msg send after poll");
    p = zz_alloc(32u, 8u, ZZ_ALLOC_HOST_VISIBLE);
    if (!p)
        return fail(79, "host visible alloc");
    if (zz_host_address(p, &off) != ZZ_OK || off == 0u)
        return fail(80, "host address");
    if (zz_host_address((char *)p + 1, &off) != ZZ_EINVAL)
        return fail(81, "interior host address");
    if (zz_mem_info(&info) != ZZ_OK ||
        info.host_visible_limit != (16u << 20) || info.host_visible_used < 32u)
        return fail(82, "mem info");
    zz_free(p);
    return 0;
}

int zz_main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";

    if (strcmp(mode, "reject") == 0)
        return test_reject();
    if (strcmp(mode, "abs") == 0)
        return test_abs();
    if (strcmp(mode, "input") == 0)
        return test_input();
    if (strcmp(mode, "display") == 0)
        return test_display();
    if (strcmp(mode, "services") == 0)
        return test_services();
    return fail(1, "usage: contract <reject|abs|input|display|services>");
}
