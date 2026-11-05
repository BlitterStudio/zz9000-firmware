/*
 * App-facing API for one trusted ARM program on the ZZ9000.
 * The table is the only firmware entry. Call the zz_* wrappers; they go
 * through the table. Do not cache a firmware address found any other way.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ZZAPP_H
#define ZZAPP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZZAPP_API_MAJOR 0u
#define ZZAPP_API_MINOR 2u
#define ZZAPP_API_VERSION ((ZZAPP_API_MAJOR << 16) | ZZAPP_API_MINOR)

#define ZZ_OK         0
#define ZZ_EIO       -1
#define ZZ_EINVAL    -2
#define ZZ_ENOMEM    -3
#define ZZ_EBUSY     -4
#define ZZ_ENOENT    -5
#define ZZ_EACCES    -6
#define ZZ_ENOSYS    -7
#define ZZ_ETIMEDOUT -8
#define ZZ_EFAULT    -9
#define ZZ_ENOSPC    -10
#define ZZ_EEXIST    -11
#define ZZ_EBADF     -12

#define ZZ_FMT_RGB565   1u
#define ZZ_FMT_BGRA8888 2u
#define ZZ_FMT_CLUT8    3u

/* Physical byte layout of a display surface.  format remains the requested
 * logical format for compatibility; inspect zzapp_surface.layout before
 * writing pixels. */
#define ZZ_LAYOUT_RGB565_BE  1u
#define ZZ_LAYOUT_RGB565_LE  2u
#define ZZ_LAYOUT_BGR565_BE  3u
#define ZZ_LAYOUT_BGR565_LE  4u
#define ZZ_LAYOUT_BGRA8888   5u
#define ZZ_LAYOUT_RGBA8888   6u
#define ZZ_LAYOUT_CLUT8      7u

#define ZZ_EV_KEY_DOWN 1u
#define ZZ_EV_KEY_UP   2u
#define ZZ_EV_MOUSE    3u
#define ZZ_EV_JOY      4u
#define ZZ_EV_STOP     5u
#define ZZ_EV_RESIZE   6u
#define ZZ_EV_BREAK    7u

#define ZZ_BREAK_CTRL_D 4u
#define ZZ_BREAK_CTRL_E 5u
#define ZZ_BREAK_CTRL_F 6u

#define ZZ_ALLOC_UNCACHED 1u
#define ZZ_FILE_READ   1u
#define ZZ_FILE_WRITE  2u
#define ZZ_FILE_CREATE 4u
#define ZZ_SEEK_SET 0
#define ZZ_SEEK_CUR 1
#define ZZ_SEEK_END 2

/* display_open flags. Mailbox is optional and falls back to strict vsync. */
#define ZZ_DISPLAY_VSYNC 1u
#define ZZ_DISPLAY_MAILBOX (1u << 1)

/*
 * ELF note, little-endian, name "ZZAPP" (section 3). n_namesz 6, n_descsz 24,
 * n_type ZZAPP_NOTE_TYPE. The loader rejects a major other than 0.
 */
#define ZZAPP_NOTE_MAGIC 0x50505A41u
#define ZZAPP_NOTE_TYPE  1u
#define ZZAPP_NOTE_FLAG_DISPLAY 1u
#define ZZAPP_NOTE_FLAG_AUDIO   2u
#define ZZAPP_NOTE_FLAG_GL      4u

struct zzapp_note {
    uint32_t magic;       /* 0x50505A41 */
    uint32_t abi_major;   /* 0 */
    uint32_t abi_minor;   /* 0 */
    uint32_t stack_bytes; /* 0 = 1 MiB */
    uint32_t flags;       /* bit0 display, bit1 audio, bit2 gl */
    uint32_t reserved;
};

typedef struct zzapp_surface {
    uint32_t id;
    uint32_t width, height, pitch, format;
    void    *front;       /* CPU mapping; NULL if the app does not draw it */
    void    *back;
    uint32_t card_front;
    uint32_t card_back;
    uint32_t layout;      /* ZZ_LAYOUT_* byte order reported by the launcher */
} zzapp_surface;

/* Native-endian copy. The launcher ring is big-endian; the firmware converts. */
typedef struct zzapp_event {
    uint16_t type, code;
    int16_t  dx, dy;
    uint16_t buttons, reserved;
    uint32_t time_ms;
} zzapp_event;

typedef struct zzapp_stat {
    uint64_t size;
    uint32_t mtime;
    uint32_t mode;        /* 1 file, 2 dir */
} zzapp_stat;

typedef struct zzapp_dirent {
    uint32_t mode;
    char     name[256];
} zzapp_dirent;

typedef struct zzapp_audio {
    int16_t *samples;              /* stereo interleaved S16LE */
    uint32_t frames;               /* capacity in frames, power of two */
    volatile uint32_t *read_frame; /* firmware advances */
    volatile uint32_t *write_frame;
    uint32_t rate;                 /* granted */
    uint32_t channels;             /* 2 */
} zzapp_audio;

/*
 * Named GL 1.1 pointers, append-only, size first. Integer parameters are
 * int32_t / uint32_t. No Hyperion or Khronos header. A wrapper whose field
 * is past gl->size is a no-op or returns 0.
 *
 * In-process ABI, not a wire image. Do not pack it: the ARM EABI has no
 * padding between size and Begin; a host LP64 build pads for pointer
 * alignment. Both sides of a call must be compiled for the same ABI.
 *
 * Perspective is four doubles (gluPerspective). DepthRange, Ortho, and
 * Frustum are doubles. Build2DMipmaps returns int32_t.
 */
typedef struct zzapp_gl {
    uint32_t size;

    void     (*Begin)(uint32_t mode);
    void     (*End)(void);
    void     (*Vertex2f)(float x, float y);
    void     (*Vertex3f)(float x, float y, float z);
    void     (*Vertex3fv)(const float *v);
    void     (*TexCoord2f)(float s, float t);
    void     (*TexCoord2fv)(const float *v);
    void     (*Color3f)(float r, float g, float b);
    void     (*Color3fv)(const float *v);
    void     (*Color3ubv)(const uint8_t *v);
    void     (*Color4f)(float r, float g, float b, float a);
    void     (*Color4fv)(const float *v);
    void     (*Color4ub)(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void     (*Color4ubv)(const uint8_t *v);
    void     (*Enable)(uint32_t cap);
    void     (*Disable)(uint32_t cap);
    uint8_t  (*IsEnabled)(uint32_t cap);
    void     (*AlphaFunc)(uint32_t func, float ref);
    void     (*BlendFunc)(uint32_t sfactor, uint32_t dfactor);
    void     (*DepthFunc)(uint32_t func);
    void     (*DepthMask)(uint8_t flag);
    void     (*DepthRange)(double n, double f);
    void     (*CullFace)(uint32_t mode);
    void     (*FrontFace)(uint32_t mode);
    void     (*ShadeModel)(uint32_t mode);
    void     (*PolygonMode)(uint32_t face, uint32_t mode);
    void     (*Clear)(uint32_t mask);
    void     (*ClearColor)(float r, float g, float b, float a);
    void     (*Viewport)(int32_t x, int32_t y, int32_t width, int32_t height);
    void     (*Scissor)(int32_t x, int32_t y, int32_t width, int32_t height);
    void     (*MatrixMode)(uint32_t mode);
    void     (*LoadIdentity)(void);
    void     (*LoadMatrixf)(const float *m);
    void     (*PushMatrix)(void);
    void     (*PopMatrix)(void);
    void     (*Rotatef)(float angle, float x, float y, float z);
    void     (*Translatef)(float x, float y, float z);
    void     (*Scalef)(float x, float y, float z);
    void     (*Ortho)(double left, double right, double bottom, double top,
                      double zNear, double zFar);
    void     (*Frustum)(double left, double right, double bottom, double top,
                        double zNear, double zFar);
    void     (*TexImage2D)(uint32_t target, int32_t level, int32_t internalformat,
                           int32_t width, int32_t height, int32_t border,
                           uint32_t format, uint32_t type, const void *pixels);
    void     (*TexSubImage2D)(uint32_t target, int32_t level,
                              int32_t xoffset, int32_t yoffset,
                              int32_t width, int32_t height,
                              uint32_t format, uint32_t type, const void *pixels);
    void     (*TexParameteri)(uint32_t target, uint32_t pname, int32_t param);
    void     (*TexParameterf)(uint32_t target, uint32_t pname, float param);
    void     (*TexEnvi)(uint32_t target, uint32_t pname, int32_t param);
    void     (*TexEnvf)(uint32_t target, uint32_t pname, float param);
    void     (*GenTextures)(int32_t n, uint32_t *textures);
    void     (*DeleteTextures)(int32_t n, const uint32_t *textures);
    void     (*BindTexture)(uint32_t target, uint32_t texture);
    void     (*Finish)(void);
    void     (*Flush)(void);
    void     (*DrawBuffer)(uint32_t mode);
    void     (*ReadBuffer)(uint32_t mode);
    void     (*ReadPixels)(int32_t x, int32_t y, int32_t width, int32_t height,
                           uint32_t format, uint32_t type, void *pixels);
    const uint8_t *(*GetString)(uint32_t name);
    void     (*GetIntegerv)(uint32_t pname, int32_t *params);
    void     (*GetFloatv)(uint32_t pname, float *params);
    uint32_t (*GetError)(void);
    void     (*Hint)(uint32_t target, uint32_t mode);
    void     (*PixelStorei)(uint32_t pname, int32_t param);
    void     (*Perspective)(double fovy, double aspect, double zNear, double zFar);
    int32_t  (*Build2DMipmaps)(uint32_t target, int32_t internalFormat,
                               int32_t width, int32_t height,
                               uint32_t format, uint32_t type, const void *data);
    /*
     * App GL ABI v1 additions.  These remain append-only: applications built
     * against the original table test ZZ_GL_COVERS before using them.
     */
    void     (*VertexPointer)(int32_t size, uint32_t type, int32_t stride,
                              const void *pointer);
    void     (*TexCoordPointer)(int32_t size, uint32_t type, int32_t stride,
                                const void *pointer);
    void     (*ColorPointer)(int32_t size, uint32_t type, int32_t stride,
                             const void *pointer);
    void     (*EnableClientState)(uint32_t cap);
    void     (*DisableClientState)(uint32_t cap);
    void     (*ClientActiveTextureARB)(uint32_t texture);
    void     (*DrawArrays)(uint32_t mode, int32_t first, int32_t count);
    void     (*DrawElements)(uint32_t mode, int32_t count, uint32_t type,
                             const void *indices);
    void     (*LockArrays)(uint32_t first, int32_t count);
    void     (*UnlockArrays)(void);
    const char *build_id; /* Module source revision; NULL on older firmware. */
} zzapp_gl;

/*
 * The table size is the compatibility version.  The v0 fields end at
 * Build2DMipmaps; new callers must use ZZ_GL_COVERS for every v1 slot.
 */
#define ZZAPP_GL_VERSION_ARRAYS 1u
/* zz_gl_bind_flags() requests optional rendering behavior. */
#define ZZ_GL_BIND_DIRECT_PRESENT 1u
/* Debug switch: drain and copy every frame before zz_gl_swap returns. */
#define ZZ_GL_BIND_SYNC_PRESENT 2u


typedef struct zzapp_api {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved0;

    void     (*exit)(int code);
    void     (*yield)(void);
    int      (*should_stop)(void);
    uint64_t (*time_us)(void);
    void     (*sleep_us)(uint32_t us);

    void    *(*alloc)(uint32_t bytes, uint32_t align, uint32_t flags);
    void     (*free)(void *p);
    void     (*heap_range)(void **base, uint32_t *bytes);

    void     (*log)(const char *msg, uint32_t len);

    int      (*file_open)(const char *path, uint32_t flags);
    int      (*file_close)(int fd);
    int      (*file_read)(int fd, void *buf, uint32_t len, uint32_t *out_n);
    int      (*file_write)(int fd, const void *buf, uint32_t len, uint32_t *out_n);
    int      (*file_seek)(int fd, int64_t off, int whence, int64_t *out_pos);
    int      (*file_stat)(const char *path, zzapp_stat *out);
    int      (*file_list_open)(const char *path);
    int      (*file_list_next)(int list, zzapp_dirent *out);
    int      (*file_list_close)(int list);

    int      (*display_open)(uint32_t w, uint32_t h, uint32_t format,
                             uint32_t flags, zzapp_surface *out);
    int      (*display_flip)(uint32_t surface_id);
    int      (*display_vsync)(void);
    int      (*display_close)(uint32_t surface_id);

    int      (*input_poll)(zzapp_event *out, uint32_t max, uint32_t *out_n);

    int      (*audio_open)(uint32_t rate, uint32_t channels, zzapp_audio *out);
    int      (*audio_close)(void);

    const zzapp_gl *(*gl_bind)(uint32_t surface_id);
    void     (*gl_swap)(void);
    void     (*gl_unbind)(void);

    /* Optional non-blocking P96 present; unsupported firmware returns ENOSYS. */
    int      (*display_flip_async)(uint32_t surface_id);
    int      (*display_flip_idle)(void);
    /* Refresh front/back after a completed flip; appended for ABI compatibility. */
    int      (*display_refresh)(zzapp_surface *surface);
    /* Appended: bind with ZZ_GL_BIND_* flags; old firmware leaves it NULL. */
    const zzapp_gl *(*gl_bind_flags)(uint32_t surface_id, uint32_t flags);
    /* Appended: update CLUT8 entries as 0x00RRGGBB words. */
    int      (*display_set_palette)(uint32_t surface_id, uint32_t first,
                                    uint32_t count, const uint32_t *rgb);
    /* Appended: cumulative frames replaced before card vblank in mailbox mode. */
    uint32_t (*display_mailbox_replaced)(void);
    /* Resident firmware source revision; NULL on older firmware. */
    const char *build_id;

    /* API 0.2 host: Shell/Workbench console and 32-byte client messages. */
    int      (*console_write)(const void *buf, uint32_t len, uint32_t *out_n);
    int      (*console_read)(void *buf, uint32_t len, uint32_t *out_n);
    int      (*msg_send)(const void *msg);
    int      (*msg_poll)(void *msg); /* 1 message, 0 empty, negative error */
} zzapp_api;

/* True when size covers field. A NULL function still returns ZZ_ENOSYS. */
#define ZZ_API_COVERS(api, field) \
    ((api) != 0 && (uint32_t)(api)->size >= \
        (uint32_t)(offsetof(zzapp_api, field) + sizeof((api)->field)))

#define ZZ_GL_COVERS(gl, field) \
    ((gl) != 0 && (uint32_t)(gl)->size >= \
        (uint32_t)(offsetof(zzapp_gl, field) + sizeof((gl)->field)))

/*
 * crt0 stores r0 here before calling zz_main. The table is const; the
 * pointer is not. The name is not zzapp_api: that is the typedef, and C
 * will not allow both. Apps may read this. Do not replace it.
 */
extern const zzapp_api *zzapp_api_ptr;

/* The app defines this. crt0 calls it and then zz_exit of the status. */
int zz_main(int argc, char **argv);

void     zz_exit(int code);
void     zz_yield(void);
int      zz_should_stop(void);
uint64_t zz_time_us(void);
void     zz_sleep_us(uint32_t us);

void    *zz_alloc(uint32_t bytes, uint32_t align, uint32_t flags);
void     zz_free(void *p);
void     zz_heap_range(void **base, uint32_t *bytes);

void     zz_log(const char *msg, uint32_t len);

int      zz_file_open(const char *path, uint32_t flags);
int      zz_file_close(int fd);
int      zz_file_read(int fd, void *buf, uint32_t len, uint32_t *out_n);
int      zz_file_write(int fd, const void *buf, uint32_t len, uint32_t *out_n);
int      zz_file_seek(int fd, int64_t off, int whence, int64_t *out_pos);
int      zz_file_stat(const char *path, zzapp_stat *out);
int      zz_file_list_open(const char *path);
int      zz_file_list_next(int list, zzapp_dirent *out);
int      zz_file_list_close(int list);

int      zz_display_open(uint32_t w, uint32_t h, uint32_t format,
                         uint32_t flags, zzapp_surface *out);
int      zz_display_flip(uint32_t surface_id);
/* Queue a flip of the back buffer and return without waiting: ZZ_OK, or a
 * negative ZZ_E* code.  zz_display_flip_idle: 1 when the last queued flip
 * has been shown (the back buffer may be written again), 0 while it is
 * still pending, negative ZZ_E* code on error (ZZ_ENOSYS on older firmware). */
int      zz_display_flip_async(uint32_t surface_id);
int      zz_display_flip_idle(void);
/* Refreshes front/back, pitch, and layout from the launcher after a completed
 * flip.  Returns ZZ_ENOSYS on firmware predating this optional API slot. */
int      zz_display_refresh(zzapp_surface *surface);
/* Cumulative frames replaced before scanout in mailbox mode; zero when the
 * optional mailbox API is unavailable or not active. */
uint32_t zz_display_mailbox_replaced(void);
/* Updates CLUT8 entries as 0x00RRGGBB words.  Returns ZZ_ENOSYS when the
 * launcher or firmware predates palette presentation support. */
int      zz_display_set_palette(uint32_t surface_id, uint32_t first,
                                uint32_t count, const uint32_t *rgb);
int      zz_display_vsync(void);
int      zz_display_close(uint32_t surface_id);

int      zz_input_poll(zzapp_event *out, uint32_t max, uint32_t *out_n);

int      zz_audio_open(uint32_t rate, uint32_t channels, zzapp_audio *out);
int      zz_audio_close(void);

/* Console reads wait politely for input. At EOF they return ZZ_OK with zero
 * bytes. Console writes never silently drop bytes. */
int      zz_console_write(const void *buf, uint32_t len, uint32_t *out_n);
int      zz_console_read(void *buf, uint32_t len, uint32_t *out_n);

/* Every message is exactly 32 opaque bytes. zz_msg_poll returns 1 when it
 * copied one message, 0 when the ring is empty, or a negative ZZ_E* code. */
int      zz_msg_send(const void *msg);
int      zz_msg_poll(void *msg);

const zzapp_gl *zz_gl_bind(uint32_t surface_id);
const zzapp_gl *zz_gl_bind_flags(uint32_t surface_id, uint32_t flags);
void     zz_gl_swap(void);
void     zz_gl_unbind(void);

/*
 * libzzapp defines these. Do not define them in the app. Classic GL types
 * that match: GLenum/GLbitfield/GLuint = unsigned int, GLint/GLsizei = int,
 * GLboolean/GLubyte = unsigned char, GLfloat/GLclampf = float,
 * GLdouble/GLclampd = double. No stdcall. gluPerspective is four doubles,
 * not the 68k GLfloat signature.
 */
void     glBegin(uint32_t mode);
void     glEnd(void);
void     glVertex2f(float x, float y);
void     glVertex3f(float x, float y, float z);
void     glVertex3fv(const float *v);
void     glTexCoord2f(float s, float t);
void     glTexCoord2fv(const float *v);
void     glColor3f(float r, float g, float b);
void     glColor3fv(const float *v);
void     glColor3ubv(const uint8_t *v);
void     glColor4f(float r, float g, float b, float a);
void     glColor4fv(const float *v);
void     glColor4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void     glColor4ubv(const uint8_t *v);
void     glEnable(uint32_t cap);
void     glDisable(uint32_t cap);
uint8_t  glIsEnabled(uint32_t cap);
void     glAlphaFunc(uint32_t func, float ref);
void     glBlendFunc(uint32_t sfactor, uint32_t dfactor);
void     glDepthFunc(uint32_t func);
void     glDepthMask(uint8_t flag);
void     glDepthRange(double n, double f);
void     glCullFace(uint32_t mode);
void     glFrontFace(uint32_t mode);
void     glShadeModel(uint32_t mode);
void     glPolygonMode(uint32_t face, uint32_t mode);
void     glClear(uint32_t mask);
void     glClearColor(float r, float g, float b, float a);
void     glViewport(int32_t x, int32_t y, int32_t width, int32_t height);
void     glScissor(int32_t x, int32_t y, int32_t width, int32_t height);
void     glMatrixMode(uint32_t mode);
void     glLoadIdentity(void);
void     glLoadMatrixf(const float *m);
void     glPushMatrix(void);
void     glPopMatrix(void);
void     glRotatef(float angle, float x, float y, float z);
void     glTranslatef(float x, float y, float z);
void     glScalef(float x, float y, float z);
void     glOrtho(double left, double right, double bottom, double top,
                 double zNear, double zFar);
void     glFrustum(double left, double right, double bottom, double top,
                   double zNear, double zFar);
void     glTexImage2D(uint32_t target, int32_t level, int32_t internalformat,
                      int32_t width, int32_t height, int32_t border,
                      uint32_t format, uint32_t type, const void *pixels);
void     glTexSubImage2D(uint32_t target, int32_t level,
                         int32_t xoffset, int32_t yoffset,
                         int32_t width, int32_t height,
                         uint32_t format, uint32_t type, const void *pixels);
void     glTexParameteri(uint32_t target, uint32_t pname, int32_t param);
void     glTexParameterf(uint32_t target, uint32_t pname, float param);
void     glTexEnvi(uint32_t target, uint32_t pname, int32_t param);
void     glTexEnvf(uint32_t target, uint32_t pname, float param);
void     glGenTextures(int32_t n, uint32_t *textures);
void     glDeleteTextures(int32_t n, const uint32_t *textures);
void     glBindTexture(uint32_t target, uint32_t texture);
void     glFinish(void);
void     glFlush(void);
void     glDrawBuffer(uint32_t mode);
void     glReadBuffer(uint32_t mode);
void     glReadPixels(int32_t x, int32_t y, int32_t width, int32_t height,
                      uint32_t format, uint32_t type, void *pixels);
const uint8_t *glGetString(uint32_t name);
void     glGetIntegerv(uint32_t pname, int32_t *params);
void     glGetFloatv(uint32_t pname, float *params);
uint32_t glGetError(void);
void     glHint(uint32_t target, uint32_t mode);
void     glPixelStorei(uint32_t pname, int32_t param);
void     gluPerspective(double fovy, double aspect, double zNear, double zFar);
int32_t  gluBuild2DMipmaps(uint32_t target, int32_t internalFormat,
                           int32_t width, int32_t height,
                           uint32_t format, uint32_t type, const void *data);
void     glVertexPointer(int32_t size, uint32_t type, int32_t stride,
                         const void *pointer);
void     glTexCoordPointer(int32_t size, uint32_t type, int32_t stride,
                           const void *pointer);
void     glColorPointer(int32_t size, uint32_t type, int32_t stride,
                        const void *pointer);
void     glEnableClientState(uint32_t cap);
void     glDisableClientState(uint32_t cap);
void     glClientActiveTextureARB(uint32_t texture);
void     glDrawArrays(uint32_t mode, int32_t first, int32_t count);
void     glDrawElements(uint32_t mode, int32_t count, uint32_t type,
                        const void *indices);
void     glLockArrays(uint32_t first, int32_t count);
void     glUnlockArrays(void);

#ifdef __cplusplus
}
#endif

#endif
