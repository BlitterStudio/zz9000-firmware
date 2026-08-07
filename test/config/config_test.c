/*
 * Host unit tests for the ZZ9000.CFG parser/loader (zz_config.c).
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Build/run: make -C test/config test
 */

#include <stdio.h>
#include <string.h>
#include <ff.h>
#include "zz_config.h"
#include "zz_video_modes.h"

/* ---- FatFs mock backend ------------------------------------------- */

static const char *mock_file = NULL;
static FRESULT mock_mount_fr = FR_OK;
static int mounts = 0;

void mock_set_file(const char *contents) { mock_file = contents; }
void mock_set_mount_result(FRESULT fr) { mock_mount_fr = fr; }
int mock_mount_balance(void) { return mounts; }

FRESULT f_mount(FATFS *fs, const char *path, unsigned char opt) {
    (void)path; (void)opt;
    if (fs == NULL) { mounts--; return FR_OK; }   /* unregister */
    if (mock_mount_fr != FR_OK) return mock_mount_fr;
    mounts++;
    return FR_OK;
}

FRESULT f_open(FIL *fp, const char *path, unsigned char mode) {
    (void)mode;
    if (strcmp(path, "0:/" ZZ_CONFIG_FILENAME) != 0) return FR_NO_FILE;
    if (!mock_file) return FR_NO_FILE;
    fp->pos = 0;
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
    size_t len = strlen(mock_file);
    size_t left = len - fp->pos;
    UINT n = (left < btr) ? (UINT)left : btr;
    memcpy(buff, mock_file + fp->pos, n);
    fp->pos += n;
    *br = n;
    return FR_OK;
}

FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }

/* ---- tiny test harness --------------------------------------------- */

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* ---- tests ---------------------------------------------------------- */

static int parse_str(const char *text) {
    return zz_config_parse(text, (unsigned)strlen(text));
}

static void test_full_valid_file(void) {
    zz_config_reset();
    int n = parse_str(
        "# full config\n"
        "videocap_mode = pal\n"
        "nonstandard_vsync = pal\n"
        "scanline_mode = 2\n"
        "scanline_parity = 1\n"
        "int2 = on\n"
        "mac = 68:82:F2:12:34:56\n"
        "hdf = games.hdf\n"
        "offscreen_bitmaps = off\n"
        "yuv_rect = off\n"
        "video_overlay = off\n");
    const struct zz_config *c = zz_config_get();
    CHECK(n == 9);
    CHECK(c->videocap_mode_present && c->videocap_mode == ZZVMODE_720x576);
    CHECK(c->ns_vsync_present && c->ns_vsync == 1);
    CHECK(c->scanline_mode_present && c->scanline_mode == 2);
    CHECK(c->scanline_parity_present && c->scanline_parity == 1);
    CHECK(c->int2_present && c->int2 == 1);
    CHECK(c->mac_present);
    CHECK(c->mac[0] == 0x68 && c->mac[1] == 0x82 && c->mac[2] == 0xF2);
    CHECK(c->mac[3] == 0x12 && c->mac[4] == 0x34 && c->mac[5] == 0x56);
    CHECK(c->hdf_present && strcmp(c->hdf_path, "0:/games.hdf") == 0);
    CHECK(c->offscreen_bitmaps_present && c->offscreen_bitmaps == 0);
    CHECK(c->video_overlay_present && c->video_overlay == 0);
}

static void test_defaults_absent(void) {
    zz_config_reset();
    const struct zz_config *c = zz_config_get();
    CHECK(!c->loaded);
    CHECK(!c->videocap_mode_present);
    CHECK(!c->ns_vsync_present);
    CHECK(!c->scanline_mode_present);
    CHECK(!c->scanline_parity_present);
    CHECK(!c->int2_present);
    CHECK(!c->mac_present);
    CHECK(!c->hdf_present);
    CHECK(!c->offscreen_bitmaps_present);
    CHECK(!c->video_overlay_present);
}

static void test_case_whitespace_comments(void) {
    zz_config_reset();
    const char *text =
        "\r\n"
        "  ; leading comment\r\n"
        "\tVIDEOCAP_MODE\t=  800X600  # trailing comment\r\n"
        "NonStandard_VSync=NTSC\r\n"
        "scanline_mode=3;comment\r\n";
    int n = parse_str(text);
    const struct zz_config *c = zz_config_get();
    CHECK(n == 3);
    CHECK(c->videocap_mode == ZZVMODE_800x600);
    CHECK(c->ns_vsync == 2);
    CHECK(c->scanline_mode == 3);
}

static void test_videocap_aliases(void) {
    zz_config_reset();
    parse_str("videocap_mode = 720x576\n");
    CHECK(zz_config_get()->videocap_mode == ZZVMODE_720x576);

    zz_config_reset();
    parse_str("nonstandard_vsync = on\n");
    CHECK(zz_config_get()->ns_vsync == 1);

    zz_config_reset();
    parse_str("nonstandard_vsync = off\n");
    CHECK(zz_config_get()->ns_vsync_present);
    CHECK(zz_config_get()->ns_vsync == 0);
}

static void test_bad_values_skipped(void) {
    zz_config_reset();
    const char *text =
        "videocap_mode = 1920x1080\n"   /* unsupported for videocap */
        "scanline_mode = 4\n"           /* out of range */
        "scanline_parity = 2\n"         /* out of range */
        "scanline_mode = -1\n"          /* negative */
        "int2 = maybe\n"
        "offscreen_bitmaps = maybe\n"
        "yuv_rect = maybe\n"
        "video_overlay = maybe\n"
        "mac = 68:82:F2:12:34\n"        /* five octets */
        "mac = gg:82:F2:12:34:56\n"     /* not hex */
        "hdf = ../etc/passwd\n"         /* path escape */
        "hdf = sub/dir.hdf\n"           /* separator */
        "hdf = .hidden\n"               /* leading dot */
        "unknown_key = 1\n"
        "not a key value line\n"
        "= novalue\n"
        "novalue =\n";
    int n = parse_str(text);
    const struct zz_config *c = zz_config_get();
    CHECK(n == 0);
    CHECK(!c->videocap_mode_present);
    CHECK(!c->scanline_mode_present);
    CHECK(!c->scanline_parity_present);
    CHECK(!c->int2_present);
    CHECK(!c->mac_present);
    CHECK(!c->hdf_present);
    CHECK(!c->offscreen_bitmaps_present);
    CHECK(!c->video_overlay_present);
}

static void test_last_value_wins(void) {
    zz_config_reset();
    const char *text =
        "scanline_mode = 1\n"
        "scanline_mode = 2\n";
    parse_str(text);
    CHECK(zz_config_get()->scanline_mode == 2);
}

static void test_no_trailing_newline(void) {
    zz_config_reset();
    const char *text = "int2 = on";
    int n = parse_str(text);
    CHECK(n == 1);
    CHECK(zz_config_get()->int2 == 1);
}

static void test_hdf_name_length(void) {
    /* 63 chars: accepted; 64: rejected */
    char line[128];
    char name[80];

    memset(name, 'a', 63); name[63] = 0;
    snprintf(line, sizeof(line), "hdf = %s\n", name);
    zz_config_reset();
    CHECK(parse_str(line) == 1);

    memset(name, 'a', 64); name[64] = 0;
    snprintf(line, sizeof(line), "hdf = %s\n", name);
    zz_config_reset();
    CHECK(parse_str(line) == 0);
}

static void test_overlong_line(void) {
    /* a line longer than the 128-byte line buffer must not crash or
     * corrupt later lines */
    char text[512];
    memset(text, 'x', 300);
    text[300] = 0;
    strcat(text, "\nint2 = on\n");
    zz_config_reset();
    int n = parse_str(text);
    CHECK(n == 1);
    CHECK(zz_config_get()->int2 == 1);
}

static void test_mac_dash_separator(void) {
    zz_config_reset();
    parse_str("mac = 00-11-22-33-44-55\n");
    const struct zz_config *c = zz_config_get();
    CHECK(c->mac_present);
    CHECK(c->mac[0] == 0x00 && c->mac[5] == 0x55);
}

static void test_query_interface(void) {
    zz_config_reset();
    uint16_t present = 1;

    /* nothing set: everything reads 0/absent */
    CHECK(zz_config_query(ZZ_CONFIG_KEY_LOADED, &present) == 0 && !present);
    CHECK(zz_config_query(ZZ_CONFIG_KEY_NS_VSYNC, &present) == 0 && !present);
    CHECK(zz_config_query(0xffff, &present) == 0 && !present);

    const char *text =
        "nonstandard_vsync = ntsc\n"
        "int2 = on\n"
        "offscreen_bitmaps = off\n"
        "yuv_rect = on\n"
        "video_overlay = off\n"
        "mac = 68:82:F2:12:34:56\n";
    parse_str(text);

    CHECK(zz_config_query(ZZ_CONFIG_KEY_NS_VSYNC, &present) == 2 && present);
    CHECK(zz_config_query(ZZ_CONFIG_KEY_INT2, &present) == 1 && present);
    CHECK(zz_config_query(ZZ_CONFIG_KEY_OFFSCREEN_BITMAPS, &present) == 0 && present);
    /* Slot 10 (was yuv_rect) is reserved: the key is no longer parsed, so
     * a file that still sets it must read back absent rather than on. */
    CHECK(zz_config_query(ZZ_CONFIG_KEY_RESERVED_10, &present) == 0 && !present);
    CHECK(zz_config_query(ZZ_CONFIG_KEY_VIDEO_OVERLAY, &present) == 0 && present);
    CHECK(zz_config_query(ZZ_CONFIG_KEY_MAC_HI, &present) == 0x6882 && present);
    CHECK(zz_config_query(ZZ_CONFIG_KEY_MAC_MID, &present) == 0xF212 && present);
    CHECK(zz_config_query(ZZ_CONFIG_KEY_MAC_LO, &present) == 0x3456 && present);
    /* scanline keys still absent */
    CHECK(zz_config_query(ZZ_CONFIG_KEY_SCANLINE_MODE, &present) == 0 && !present);
}

static void test_loader_success(void) {
    mock_set_file("videocap_mode = pal\nint2 = on\n");
    mock_set_mount_result(FR_OK);
    CHECK(zz_config_load() == 0);
    const struct zz_config *c = zz_config_get();
    CHECK(c->loaded);
    CHECK(c->videocap_mode == ZZVMODE_720x576);
    uint16_t present = 0;
    CHECK(zz_config_query(ZZ_CONFIG_KEY_LOADED, &present) == 1 && present);
    CHECK(mock_mount_balance() == 0); /* volume unregistered again */
}

static void test_loader_no_file(void) {
    mock_set_file(NULL);
    mock_set_mount_result(FR_OK);
    CHECK(zz_config_load() == -1);
    CHECK(!zz_config_get()->loaded);
    CHECK(mock_mount_balance() == 0);
}

static void test_read_raw(void) {
    char buf[64];
    uint32_t len = 99;

    mock_set_file("int2 = on\n");
    CHECK(zz_config_read_raw(buf, sizeof(buf), &len) == ZZ_CONFIG_FILE_OK);
    CHECK(len == 10);
    CHECK(memcmp(buf, "int2 = on\n", 10) == 0);

    /* oversized file: silently truncated to max_len, like the boot parse */
    mock_set_file("0123456789abcdef");
    CHECK(zz_config_read_raw(buf, 8, &len) == ZZ_CONFIG_FILE_OK);
    CHECK(len == 8);
    CHECK(memcmp(buf, "01234567", 8) == 0);

    mock_set_file(NULL);
    CHECK(zz_config_read_raw(buf, sizeof(buf), &len) == ZZ_CONFIG_FILE_NO_FILE);
    CHECK(len == 0);
}

static void test_loader_no_card(void) {
    mock_set_file("int2 = on\n");
    mock_set_mount_result(FR_NOT_READY);
    CHECK(zz_config_load() == -1);
    CHECK(!zz_config_get()->loaded);
    CHECK(mock_mount_balance() == 0);
}

int main(void) {
    test_full_valid_file();
    test_defaults_absent();
    test_case_whitespace_comments();
    test_videocap_aliases();
    test_bad_values_skipped();
    test_last_value_wins();
    test_no_trailing_newline();
    test_hdf_name_length();
    test_overlong_line();
    test_mac_dash_separator();
    test_query_interface();
    test_loader_success();
    test_loader_no_file();
    test_loader_no_card();
    test_read_raw();

    if (failures) {
        printf("%d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
