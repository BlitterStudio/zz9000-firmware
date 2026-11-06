/*
 * File proxy. Relative paths stay under --root. A component that is ".." or
 * empty (a leading slash, or two slashes) is the parent, matching ZZRun. A
 * parent step at the root is ZZ_EACCES. A path containing ':' is ZZ_EACCES
 * unless --abs, in which case the root check does not apply and the string
 * is opened as a host path.
 *
 * FDs are 16 integers starting at 3. CREATE truncates, like MODE_NEWFILE.
 * map_file reads at most 16 MiB into a host-visible allocation. A larger
 * file is ZZ_ENOSPC. A zero-length file is ZZ_ENOMEM, matching the card
 * allocator's rejection of a zero-byte grant.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE 700
#endif

#include "host_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef S_IFDIR
#define S_IFDIR _S_IFDIR
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_IFREG
#define S_IFREG _S_IFREG
#endif
#ifndef S_IFMT
#define S_IFMT (_S_IFDIR | _S_IFREG)
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

struct host_fd {
    int used;
    FILE *fp;
};

struct host_list {
    int used;
    char path[4096];
#ifdef _WIN32
    HANDLE find;
    WIN32_FIND_DATAA data;
    int have;
    int done;
#else
    DIR *dir;
#endif
};

static struct host_fd fds[HOST_FD_COUNT];
static struct host_list lists[HOST_FD_COUNT];
static char root_abs[4096];
static int abs_paths;

static int is_sep(char c)
{
    return c == '/' || c == '\\';
}

static void to_slash(char *p)
{
    for (; *p; p++) {
        if (*p == '\\')
            *p = '/';
    }
}

static void strip_trailing_sep(char *p)
{
    size_t n = strlen(p);

    while (n > 1u && is_sep(p[n - 1u])) {
        if (n == 3u && p[1] == ':')
            break;
        p[--n] = 0;
    }
}

static int make_abs(const char *in, char *out, size_t n)
{
#ifdef _WIN32
    DWORD got = GetFullPathNameA(in, (DWORD)n, out, NULL);
    if (got == 0 || got >= n)
        return -1;
#else
    (void)n;
    if (!realpath(in, out))
        return -1;
#endif
    to_slash(out);
    strip_trailing_sep(out);
    return 0;
}

static int slot_of(int id)
{
    if (id < HOST_FD_BASE || id >= HOST_FD_BASE + HOST_FD_COUNT)
        return -1;
    return id - HOST_FD_BASE;
}



static int at_root(const char *cur)
{
    return strcmp(cur, root_abs) == 0;
}

static int step_parent(char *cur)
{
    char *slash;

    if (at_root(cur))
        return ZZ_EACCES;
    slash = strrchr(cur, '/');
    if (!slash)
        return ZZ_EACCES;
    if (slash == cur)
        slash[1] = 0;
    else if (slash - cur == 2 && cur[1] == ':')
        slash[1] = 0;
    else
        *slash = 0;
    if (strlen(cur) < strlen(root_abs) ||
        strncmp(cur, root_abs, strlen(root_abs)) != 0)
        return ZZ_EACCES;
    return ZZ_OK;
}

static int join_comp(char *cur, size_t n, const char *comp)
{
    size_t len = strlen(cur);
    int need = !(len > 0u && cur[len - 1u] == '/');
    size_t add = strlen(comp) + (need ? 1u : 0u);

    if (len + add + 1u > n)
        return ZZ_EINVAL;
    if (need)
        cur[len++] = '/';
    memcpy(cur + len, comp, strlen(comp) + 1u);
    return ZZ_OK;
}

static int path_stat(const char *path, struct stat *st)
{
#ifdef _WIN32
    struct _stat64 wide;
    if (_stat64(path, &wide) != 0)
        return -1;
    memset(st, 0, sizeof *st);
    st->st_mode = (wide.st_mode & _S_IFDIR) ? S_IFDIR : S_IFREG;
    st->st_size = (off_t)wide.st_size;
    st->st_mtime = (time_t)wide.st_mtime;
    return 0;
#else
    return stat(path, st);
#endif
}

static int is_dir_path(const char *path)
{
    struct stat st;

    if (path_stat(path, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode);
}

/*
 * Walk path the way ZZRun does. On success, out is the host path of the
 * parent directory and leaf is the final component. A missing intermediate
 * directory is ZZ_ENOENT. The leaf itself may be absent when creating.
 */
static int resolve_rel(const char *path, char *out, size_t out_n, char *leaf,
                       size_t leaf_n, int creating)
{
    const char *p = path;
    char cur[4096];
    char comp[256];
    int have_hold = 0;
    char hold[256];

    if (strlen(root_abs) + 1u > out_n)
        return ZZ_EINVAL;
    memcpy(cur, root_abs, strlen(root_abs) + 1u);
    hold[0] = 0;
    leaf[0] = 0;
    for (;;) {
        size_t i = 0;
        int parent = 0;
        int end = 0;

        if (*p == 0) {
            end = 1;
        } else if (is_sep(*p)) {
            parent = 1;
            p++;
        } else {
            while (*p && !is_sep(*p)) {
                if (i + 1u >= sizeof comp)
                    return ZZ_EINVAL;
                comp[i++] = *p++;
            }
            comp[i] = 0;
            if (is_sep(*p))
                p++;
        }
        if (end)
            break;
        if (parent || strcmp(comp, "..") == 0) {
            if (have_hold) {
                int rc = join_comp(cur, sizeof cur, hold);
                if (rc != ZZ_OK)
                    return rc;
                if (!is_dir_path(cur))
                    return ZZ_ENOENT;
                have_hold = 0;
            }
            {
                int rc = step_parent(cur);
                if (rc != ZZ_OK)
                    return rc;
            }
            continue;
        }
        if (strcmp(comp, ".") == 0)
            continue;
        if (have_hold) {
            int rc = join_comp(cur, sizeof cur, hold);
            if (rc != ZZ_OK)
                return rc;
            if (!is_dir_path(cur))
                return ZZ_ENOENT;
        }
        if (strlen(comp) >= sizeof hold)
            return ZZ_EINVAL;
        memcpy(hold, comp, strlen(comp) + 1u);
        have_hold = 1;
    }
    if (!have_hold)
        return ZZ_EIO;
    if (strlen(hold) + 1u > leaf_n)
        return ZZ_EINVAL;
    memcpy(leaf, hold, strlen(hold) + 1u);
    if (strlen(cur) + 1u > out_n)
        return ZZ_EINVAL;
    memcpy(out, cur, strlen(cur) + 1u);
    (void)creating;
    return ZZ_OK;
}

static int compose(char *dst, size_t n, const char *dir, const char *leaf)
{
    size_t ld = strlen(dir);
    size_t ll = strlen(leaf);
    int need = !(ld > 0u && dir[ld - 1u] == '/');

    if (ld + ll + (need ? 1u : 0u) + 1u > n)
        return ZZ_EINVAL;
    memcpy(dst, dir, ld);
    if (need)
        dst[ld++] = '/';
    memcpy(dst + ld, leaf, ll + 1u);
    return ZZ_OK;
}

static int resolve_open(const char *path, char *full, size_t n, int creating)
{
    char dir[4096];
    char leaf[256];
    int rc;

    if (!path || path[0] == 0 || strlen(path) > HOST_PATH_MAX)
        return ZZ_EINVAL;
    if (strchr(path, ':')) {
        if (!abs_paths)
            return ZZ_EACCES;
        if (strlen(path) + 1u > n)
            return ZZ_EINVAL;
        memcpy(full, path, strlen(path) + 1u);
        return ZZ_OK;
    }
    rc = resolve_rel(path, dir, sizeof dir, leaf, sizeof leaf, creating);
    if (rc != ZZ_OK)
        return rc;
    return compose(full, n, dir, leaf);
}

static int io_err(void)
{
    if (errno == ENOENT)
        return ZZ_ENOENT;
    if (errno == EACCES || errno == EPERM)
        return ZZ_EACCES;
    if (errno == ENOSPC)
        return ZZ_ENOSPC;
    if (errno == EEXIST)
        return ZZ_EEXIST;
    if (errno == ENOMEM)
        return ZZ_ENOMEM;
    if (errno == EBUSY)
        return ZZ_EBUSY;
    return ZZ_EIO;
}

static int host_seek(FILE *fp, int64_t off, int whence)
{
#ifdef _WIN32
    return _fseeki64(fp, (__int64)off, whence);
#else
    return fseeko(fp, (off_t)off, whence);
#endif
}

static int64_t host_tell(FILE *fp)
{
#ifdef _WIN32
    return (int64_t)_ftelli64(fp);
#else
    return (int64_t)ftello(fp);
#endif
}

static int api_file_open(const char *path, uint32_t flags)
{
    char full[4096];
    const char *mode;
    FILE *fp;
    int creating;
    int slot;
    int rc;

    if ((flags & ~(ZZ_FILE_READ | ZZ_FILE_WRITE | ZZ_FILE_CREATE)) != 0u)
        return ZZ_EINVAL;
    if ((flags & (ZZ_FILE_READ | ZZ_FILE_WRITE | ZZ_FILE_CREATE)) == 0u)
        return ZZ_EINVAL;
    creating = (flags & ZZ_FILE_CREATE) != 0u;
    rc = resolve_open(path, full, sizeof full, creating);
    if (rc != ZZ_OK)
        return rc;
    if (creating)
        mode = "wb+";
    else if (flags & ZZ_FILE_WRITE)
        mode = "r+b";
    else
        mode = "rb";
    /* Reserve the slot before the creating open: "wb+" truncates, and a
     * truncation followed by ZZ_ENOMEM would destroy the caller's file. */
    for (slot = 0; slot < HOST_FD_COUNT; slot++) {
        if (!fds[slot].used)
            break;
    }
    if (slot == HOST_FD_COUNT)
        return ZZ_ENOMEM;
    fp = fopen(full, mode);
    if (!fp)
        return io_err();
    fds[slot].used = 1;
    fds[slot].fp = fp;
    return HOST_FD_BASE + slot;
}

static int api_file_close(int fd)
{
    int ix = slot_of(fd);

    if (ix < 0 || !fds[ix].used)
        return ZZ_EBADF;
    fclose(fds[ix].fp);
    fds[ix].fp = 0;
    fds[ix].used = 0;
    return ZZ_OK;
}

static int api_file_read(int fd, void *buf, uint32_t len, uint32_t *out_n)
{
    int ix = slot_of(fd);
    size_t n;

    if (ix < 0 || !fds[ix].used)
        return ZZ_EBADF;
    if (!buf || !out_n)
        return ZZ_EINVAL;
    n = fread(buf, 1, (size_t)len, fds[ix].fp);
    *out_n = (uint32_t)n;
    if (n < len && ferror(fds[ix].fp))
        return ZZ_EIO;
    return ZZ_OK;
}

static int api_file_write(int fd, const void *buf, uint32_t len, uint32_t *out_n)
{
    int ix = slot_of(fd);
    size_t n;

    if (ix < 0 || !fds[ix].used)
        return ZZ_EBADF;
    if (!buf || !out_n)
        return ZZ_EINVAL;
    n = fwrite(buf, 1, (size_t)len, fds[ix].fp);
    *out_n = (uint32_t)n;
    if (n < len) {
        if (fflush(fds[ix].fp) != 0 && errno == ENOSPC)
            return ZZ_ENOSPC;
        return ZZ_EIO;
    }
    return ZZ_OK;
}

static int api_file_seek(int fd, int64_t off, int whence, int64_t *out_pos)
{
    int ix = slot_of(fd);
    int w;

    if (ix < 0 || !fds[ix].used)
        return ZZ_EBADF;
    if (!out_pos)
        return ZZ_EINVAL;
    if (whence == ZZ_SEEK_SET)
        w = SEEK_SET;
    else if (whence == ZZ_SEEK_CUR)
        w = SEEK_CUR;
    else if (whence == ZZ_SEEK_END)
        w = SEEK_END;
    else
        return ZZ_EINVAL;
    if (host_seek(fds[ix].fp, off, w) != 0)
        return ZZ_EINVAL;
    *out_pos = host_tell(fds[ix].fp);
    return ZZ_OK;
}

static int fill_stat(const char *full, zzapp_stat *out)
{
    struct stat st;

    if (path_stat(full, &st) != 0)
        return io_err();
    out->size = (uint64_t)st.st_size;
    out->mtime = st.st_mtime > 0 ? (uint32_t)st.st_mtime : 0u;
    out->mode = S_ISDIR(st.st_mode) ? 2u : 1u;
    return ZZ_OK;
}

static int api_file_stat(const char *path, zzapp_stat *out)
{
    char full[4096];
    int rc;

    if (!out)
        return ZZ_EINVAL;
    rc = resolve_open(path, full, sizeof full, 0);
    if (rc != ZZ_OK)
        return rc;
    return fill_stat(full, out);
}

static int api_file_list_open(const char *path)
{
    char full[4096];
    int rc;
    int id;
    int ix;

    if (!path || path[0] == 0)
        return ZZ_EINVAL;
    if (strchr(path, ':')) {
        if (!abs_paths)
            return ZZ_EACCES;
        if (strlen(path) + 1u > sizeof full)
            return ZZ_EINVAL;
        memcpy(full, path, strlen(path) + 1u);
    } else if (strcmp(path, ".") == 0 || strcmp(path, "") == 0) {
        memcpy(full, root_abs, strlen(root_abs) + 1u);
    } else {
        char dir[4096];
        char leaf[256];
        rc = resolve_rel(path, dir, sizeof dir, leaf, sizeof leaf, 0);
        if (rc != ZZ_OK)
            return rc;
        rc = compose(full, sizeof full, dir, leaf);
        if (rc != ZZ_OK)
            return rc;
    }
    if (!is_dir_path(full))
        return ZZ_ENOENT;
    for (ix = 0; ix < HOST_FD_COUNT; ix++) {
        if (!lists[ix].used)
            break;
    }
    if (ix == HOST_FD_COUNT)
        return ZZ_ENOMEM;
    memset(&lists[ix], 0, sizeof lists[ix]);
    memcpy(lists[ix].path, full, strlen(full) + 1u);
#ifdef _WIN32
    {
        char pattern[4096];
        if (strlen(full) + 3u > sizeof pattern)
            return ZZ_EINVAL;
        snprintf(pattern, sizeof pattern, "%s/*", full);
        lists[ix].find = FindFirstFileA(pattern, &lists[ix].data);
        if (lists[ix].find == INVALID_HANDLE_VALUE)
            return ZZ_EIO;
        lists[ix].have = 1;
    }
#else
    lists[ix].dir = opendir(full);
    if (!lists[ix].dir)
        return io_err();
#endif
    lists[ix].used = 1;
    id = HOST_FD_BASE + ix;
    return id;
}

static int skip_dot(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static int api_file_list_next(int list, zzapp_dirent *out)
{
    int ix = slot_of(list);

    if (ix < 0 || !lists[ix].used)
        return ZZ_EBADF;
    if (!out)
        return ZZ_EINVAL;
#ifdef _WIN32
    for (;;) {
        const char *name;
        if (lists[ix].done)
            return ZZ_ENOENT;
        if (!lists[ix].have) {
            if (!FindNextFileA(lists[ix].find, &lists[ix].data)) {
                lists[ix].done = 1;
                return ZZ_ENOENT;
            }
        }
        lists[ix].have = 0;
        name = lists[ix].data.cFileName;
        if (skip_dot(name))
            continue;
        memset(out, 0, sizeof *out);
        strncpy(out->name, name, sizeof out->name - 1u);
        out->mode = (lists[ix].data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        ? 2u
                        : 1u;
        return ZZ_OK;
    }
#else
    for (;;) {
        struct dirent *ent = readdir(lists[ix].dir);
        if (!ent)
            return ZZ_ENOENT;
        if (skip_dot(ent->d_name))
            continue;
        memset(out, 0, sizeof *out);
        strncpy(out->name, ent->d_name, sizeof out->name - 1u);
        out->mode = 1u;
        {
            char child[4096];
            struct stat st;

            if (compose(child, sizeof child, lists[ix].path, ent->d_name) ==
                    ZZ_OK &&
                path_stat(child, &st) == 0 && S_ISDIR(st.st_mode))
                out->mode = 2u;
        }
        return ZZ_OK;
    }
#endif
}

static int api_file_list_close(int list)
{
    int ix = slot_of(list);

    if (ix < 0 || !lists[ix].used)
        return ZZ_EBADF;
#ifdef _WIN32
    if (lists[ix].find != INVALID_HANDLE_VALUE)
        FindClose(lists[ix].find);
#else
    if (lists[ix].dir)
        closedir(lists[ix].dir);
#endif
    memset(&lists[ix], 0, sizeof lists[ix]);
    return ZZ_OK;
}

static int api_map_file(const char *path, const void **data, uint32_t *bytes)
{
    zzapp_stat st;
    uint8_t *dst;
    int fd;
    uint32_t done = 0;
    int rc;

    if (!path || !data || !bytes)
        return ZZ_EINVAL;
    *data = 0;
    *bytes = 0;
    rc = api_file_stat(path, &st);
    if (rc != ZZ_OK)
        return rc;
    if (st.mode != 1u)
        return ZZ_EIO;
    if (st.size > HOST_MAP_LIMIT)
        return ZZ_ENOSPC;
    if (st.size == 0u)
        return ZZ_ENOMEM;
    dst = (uint8_t *)zzapp_api_ptr->alloc((uint32_t)st.size, 8u,
                                          ZZ_ALLOC_HOST_VISIBLE);
    if (!dst)
        return ZZ_ENOMEM;
    fd = api_file_open(path, ZZ_FILE_READ);
    if (fd < 0) {
        zzapp_api_ptr->free(dst);
        return fd;
    }
    while (done < (uint32_t)st.size) {
        uint32_t got = 0;
        uint32_t want = (uint32_t)st.size - done;
        rc = api_file_read(fd, dst + done, want, &got);
        if (rc != ZZ_OK || got == 0u) {
            api_file_close(fd);
            zzapp_api_ptr->free(dst);
            return rc != ZZ_OK ? rc : ZZ_EIO;
        }
        done += got;
    }
    api_file_close(fd);
    *data = dst;
    *bytes = done;
    return ZZ_OK;
}

static void api_unmap_file(const void *data)
{
    if (!data || !zzapp_api_ptr || !zzapp_api_ptr->free)
        return;
    zzapp_api_ptr->free((void *)data);
}

int host_files_init(const char *root, int allow_abs)
{
    struct stat st;

    abs_paths = allow_abs;
    if (!root)
        root = ".";
    if (make_abs(root, root_abs, sizeof root_abs) != 0) {
        fprintf(stderr, "zzapp-host: --root %s is not a directory\n", root);
        return -1;
    }
    if (path_stat(root_abs, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "zzapp-host: --root %s is not a directory\n", root);
        return -1;
    }
    memset(fds, 0, sizeof fds);
    memset(lists, 0, sizeof lists);
    return 0;
}

void host_files_shutdown(void)
{
    int i;

    for (i = 0; i < HOST_FD_COUNT; i++) {
        if (fds[i].used && fds[i].fp)
            fclose(fds[i].fp);
        fds[i].used = 0;
        fds[i].fp = 0;
        if (lists[i].used)
            api_file_list_close(HOST_FD_BASE + i);
    }
}

void host_files_bind(zzapp_api *api)
{
    api->file_open = api_file_open;
    api->file_close = api_file_close;
    api->file_read = api_file_read;
    api->file_write = api_file_write;
    api->file_seek = api_file_seek;
    api->file_stat = api_file_stat;
    api->file_list_open = api_file_list_open;
    api->file_list_next = api_file_list_next;
    api->file_list_close = api_file_list_close;
    api->map_file = api_map_file;
    api->unmap_file = api_unmap_file;
}
