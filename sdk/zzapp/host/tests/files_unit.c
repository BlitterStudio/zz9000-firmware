/*
 * Unit checks for the host runner file proxy. Includes files.c directly so
 * the descriptor table is observable from the test.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/files.c"

#include <assert.h>

/* files.c's map_file reads the app table pointer. */
const zzapp_api *zzapp_api_ptr;

#define UNIT_ROOT "zzapp_files_unit_root"

static void make_dir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

static void put_file(const char *leaf, const char *data)
{
    char path[4096];
    FILE *fp;
    size_t n = strlen(data);

    snprintf(path, sizeof path, "%s/%s", UNIT_ROOT, leaf);
    fp = fopen(path, "wb");
    assert(fp != NULL);
    if (n)
        assert(fwrite(data, 1, n, fp) == n);
    assert(fclose(fp) == 0);
}

static int file_size(const char *leaf)
{
    char path[4096];
    struct stat st;

    snprintf(path, sizeof path, "%s/%s", UNIT_ROOT, leaf);
    if (path_stat(path, &st) != 0)
        return -1;
    return (int)st.st_size;
}

static void test_create_keeps_file_when_slots_full(void)
{
    char name[32];
    int fd[HOST_FD_COUNT];
    int i;

    host_files_init(UNIT_ROOT, 0);
    put_file("victim.bin", "keep me");
    for (i = 0; i < HOST_FD_COUNT; i++) {
        snprintf(name, sizeof name, "f%02d.bin", i);
        put_file(name, "x");
        fd[i] = api_file_open(name, ZZ_FILE_READ);
        assert(fd[i] == HOST_FD_BASE + i);
    }
    /* Every slot taken: the create must fail without truncating victim. */
    assert(api_file_open("victim.bin",
                         ZZ_FILE_READ | ZZ_FILE_WRITE | ZZ_FILE_CREATE) ==
           ZZ_ENOMEM);
    for (i = 0; i < HOST_FD_COUNT; i++)
        assert(api_file_close(fd[i]) == ZZ_OK);
    assert(file_size("victim.bin") == 7);
    host_files_shutdown();
}


static void test_close_reports_flush_failure(void)
{
#ifndef _WIN32
    FILE *fp;

    host_files_init(UNIT_ROOT, 0);
    fp = fopen("/dev/full", "wb");
    if (!fp)
        return; /* environment without /dev/full; nothing to assert */
    /* Buffered: the failure surfaces when fclose flushes. */
    assert(fwrite("x", 1, 1, fp) == 1);
    fds[0].used = 1;
    fds[0].fp = fp;
    assert(api_file_close(HOST_FD_BASE) == ZZ_ENOSPC);
    assert(!fds[0].used); /* the descriptor is released despite the error */
    assert(api_file_close(HOST_FD_BASE) == ZZ_EBADF);
    host_files_shutdown();
#endif
}

int main(void)
{
    make_dir(UNIT_ROOT);
    test_create_keeps_file_when_slots_full();
    test_close_reports_flush_failure();
    return 0;
}
