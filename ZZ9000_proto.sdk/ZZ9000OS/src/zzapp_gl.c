/*
 * App GL table and the present copy into the bound P96 bitmap.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The front end is compiled in from MINIGL_LIB_DIR with -DMGL_PLATFORM_ZZARM.
 * This file does not include amiberry_backend.c, amiberry_glu_quad.c, or vendor/.
 */
#include <stdint.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include("zzapp.h")
#    include "zzapp.h"
#    define ZZAPP_GL_HAVE_HEADER 1
#  endif
#endif

#include "memorymap.h"
#include "minigl_present.h"
#include "minigl_raster.h"
#include "minigl_renderer.h"
#include "mw_ctx.h"
#include "mw_types.h"
#include "amiberry_types.h"

#ifndef ZZ_OK
#define ZZ_OK 0
#define ZZ_EINVAL (-2)
#define ZZ_ENOMEM (-3)
#define ZZ_EBUSY (-4)
#define ZZ_ENOSYS (-7)
#endif

#ifndef ZZ_FMT_RGB565
#define ZZ_FMT_RGB565 1u
#define ZZ_FMT_BGRA8888 2u
#endif

#ifndef ZZAPP_GL_HAVE_HEADER
/* Matches sdk/zzapp/include/zzapp.h (app/libzzapp 80c39ab). */
typedef struct zzapp_gl {
    uint32_t size;
    void (*Begin)(uint32_t mode);
    void (*End)(void);
    void (*Vertex2f)(float x, float y);
    void (*Vertex3f)(float x, float y, float z);
    void (*Vertex3fv)(const float *v);
    void (*TexCoord2f)(float s, float t);
    void (*TexCoord2fv)(const float *v);
    void (*Color3f)(float r, float g, float b);
    void (*Color3fv)(const float *v);
    void (*Color3ubv)(const uint8_t *v);
    void (*Color4f)(float r, float g, float b, float a);
    void (*Color4fv)(const float *v);
    void (*Color4ub)(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void (*Color4ubv)(const uint8_t *v);
    void (*Enable)(uint32_t cap);
    void (*Disable)(uint32_t cap);
    uint8_t (*IsEnabled)(uint32_t cap);
    void (*AlphaFunc)(uint32_t func, float ref);
    void (*BlendFunc)(uint32_t sfactor, uint32_t dfactor);
    void (*DepthFunc)(uint32_t func);
    void (*DepthMask)(uint8_t flag);
    void (*DepthRange)(double n, double f);
    void (*CullFace)(uint32_t mode);
    void (*FrontFace)(uint32_t mode);
    void (*ShadeModel)(uint32_t mode);
    void (*PolygonMode)(uint32_t face, uint32_t mode);
    void (*Clear)(uint32_t mask);
    void (*ClearColor)(float r, float g, float b, float a);
    void (*Viewport)(int32_t x, int32_t y, int32_t width, int32_t height);
    void (*Scissor)(int32_t x, int32_t y, int32_t width, int32_t height);
    void (*MatrixMode)(uint32_t mode);
    void (*LoadIdentity)(void);
    void (*LoadMatrixf)(const float *m);
    void (*PushMatrix)(void);
    void (*PopMatrix)(void);
    void (*Rotatef)(float angle, float x, float y, float z);
    void (*Translatef)(float x, float y, float z);
    void (*Scalef)(float x, float y, float z);
    void (*Ortho)(double left, double right, double bottom, double top, double zNear, double zFar);
    void (*Frustum)(double left, double right, double bottom, double top, double zNear, double zFar);
    void (*TexImage2D)(uint32_t target, int32_t level, int32_t internalformat, int32_t width, int32_t height, int32_t border, uint32_t format, uint32_t type, const void *pixels);
    void (*TexSubImage2D)(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height, uint32_t format, uint32_t type, const void *pixels);
    void (*TexParameteri)(uint32_t target, uint32_t pname, int32_t param);
    void (*TexParameterf)(uint32_t target, uint32_t pname, float param);
    void (*TexEnvi)(uint32_t target, uint32_t pname, int32_t param);
    void (*TexEnvf)(uint32_t target, uint32_t pname, float param);
    void (*GenTextures)(int32_t n, uint32_t *textures);
    void (*DeleteTextures)(int32_t n, const uint32_t *textures);
    void (*BindTexture)(uint32_t target, uint32_t texture);
    void (*Finish)(void);
    void (*Flush)(void);
    void (*DrawBuffer)(uint32_t mode);
    void (*ReadBuffer)(uint32_t mode);
    void (*ReadPixels)(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type, void *pixels);
    const uint8_t *(*GetString)(uint32_t name);
    void (*GetIntegerv)(uint32_t pname, int32_t *params);
    void (*GetFloatv)(uint32_t pname, float *params);
    uint32_t (*GetError)(void);
    void (*Hint)(uint32_t target, uint32_t mode);
    void (*PixelStorei)(uint32_t pname, int32_t param);
    void (*Perspective)(double fovy, double aspect, double zNear, double zFar);
    int32_t (*Build2DMipmaps)(uint32_t target, int32_t internalFormat, int32_t width, int32_t height, uint32_t format, uint32_t type, const void *data);
} zzapp_gl;
#endif
#include "minigl_platform.h"




extern int amiberry_inproc_open(uint32_t width, uint32_t height);
extern void amiberry_inproc_close(void);
extern mw_ctx *amiberry_transport_inproc_ctx(void);
extern GLcontext mini_CurrentContext;
struct AmiberryContext;
extern struct AmiberryContext *amiberry_context(GLcontext context);
extern int amiberry_flush(struct AmiberryContext *context);
extern UBYTE *amiberry_command(struct AmiberryContext *context, ULONG opcode, ULONG payload);
extern GLboolean amiberry_has_pending_command(struct AmiberryContext *context, ULONG opcode);

extern void GLBegin(GLcontext, GLenum);
extern void GLEnd(GLcontext);
extern void GLVertex4f(GLcontext, GLfloat, GLfloat, GLfloat, GLfloat);
extern void GLVertex3fv(GLcontext, GLfloat *);
extern void GLTexCoord2f(GLcontext, GLfloat, GLfloat);
extern void GLTexCoord2fv(GLcontext, GLfloat *);
extern void GLColor4f(GLcontext, GLfloat, GLfloat, GLfloat, GLfloat);
extern void GLColor3fv(GLcontext, GLfloat *);
extern void GLColor4ub(GLcontext, GLubyte, GLubyte, GLubyte, GLubyte);
extern void GLColor4ubv(GLcontext, GLubyte *);
extern void GLColor3ubv(GLcontext, GLubyte *);
extern void GLEnable(GLcontext, GLenum);
extern void GLDisable(GLcontext, GLenum);
extern GLboolean GLIsEnabled(GLcontext, GLenum);
extern void GLAlphaFunc(GLcontext, GLenum, GLclampf);
extern void GLBlendFunc(GLcontext, GLenum, GLenum);
extern void GLDepthFunc(GLcontext, GLenum);
extern void GLDepthMask(GLcontext, GLboolean);
extern void GLDepthRange(GLcontext, GLclampd, GLclampd);
extern void GLCullFace(GLcontext, GLenum);
extern void GLFrontFace(GLcontext, GLenum);
extern void MGLSetState(GLcontext, GLenum, GLboolean);
extern void GLShadeModel(GLcontext, GLenum);
extern void GLPolygonMode(GLcontext, GLenum, GLenum);
extern void GLClear(GLcontext, GLbitfield);
extern void GLClearColor(GLcontext, GLclampf, GLclampf, GLclampf, GLclampf);
extern void GLViewport(GLcontext, GLint, GLint, GLsizei, GLsizei);
extern void GLScissor(GLcontext, GLint, GLint, GLsizei, GLsizei);
extern void GLMatrixMode(GLcontext, GLenum);
extern void GLLoadIdentity(GLcontext);
extern void GLLoadMatrixf(GLcontext, const GLfloat *);
extern void GLPushMatrix(GLcontext);
extern void GLPopMatrix(GLcontext);
extern void GLRotatef(GLcontext, GLfloat, GLfloat, GLfloat, GLfloat);
extern void GLTranslatef(GLcontext, GLfloat, GLfloat, GLfloat);
extern void GLScalef(GLcontext, GLfloat, GLfloat, GLfloat);
extern void GLOrtho(GLcontext, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
extern void GLFrustum(GLcontext, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
extern void GLTexImage2D(GLcontext, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *);
extern void GLTexSubImage2D(GLcontext, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const GLvoid *);
extern void GLTexParameteri(GLcontext, GLenum, GLenum, GLint);
extern void GLTexEnvi(GLcontext, GLenum, GLenum, GLint);
extern void GLGenTextures(GLcontext, GLsizei, GLuint *);
extern void GLDeleteTextures(GLcontext, GLsizei, const GLuint *);
extern void GLBindTexture(GLcontext, GLenum, GLuint);
extern void GLFinish(GLcontext);
extern void GLFlush(GLcontext);
extern void GLDrawBuffer(GLcontext, GLenum);
extern void GLReadBuffer(GLcontext, GLenum);
extern void GLReadPixels(GLcontext, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *);
extern void GLGetIntegerv(GLcontext, GLenum, GLint *);
extern void GLGetFloatv(GLcontext, GLenum, GLfloat *);
extern GLenum GLGetError(GLcontext);
extern void GLHint(GLcontext, GLenum, GLenum);
extern void GLPixelStorei(GLcontext, GLenum, GLint);
extern void GLUPerspective(GLfloat, GLfloat, GLfloat, GLfloat);
extern GLint GLUBuild2DMipmaps(GLcontext, GLenum, GLint, GLsizei, GLsizei, GLenum, GLenum, const GLvoid *);
#if defined(MINIGL_MODULE)
#include "zzm_runtime.h"

static const zzm_app_gl_host_service_t *g_module_app_host;

int zzapp_gl_set_module_host(const zzm_app_gl_host_service_t *host)
{
    if (!host || !host->api_enter || !host->api_leave ||
        !host->bound_display || !host->flip_bound || !host->generation)
        return ZZ_EINVAL;
    g_module_app_host = host;
    return ZZ_OK;
}

void zzapp_api_enter(void)
{
    g_module_app_host->api_enter();
}

void zzapp_api_leave(void)
{
    g_module_app_host->api_leave();
}

int zzapp_display_bound(uint32_t *w, uint32_t *h, uint32_t *pitch,
                        uint32_t *format, uint32_t *card_front,
                        uint32_t *card_back)
{
    return g_module_app_host->bound_display(w, h, pitch, format, card_front,
                                             card_back);
}

int zzapp_display_flip_bound(void)
{
    return g_module_app_host->flip_bound(g_module_app_host->generation());
}

int zzapp_svc_display_flip(uint32_t surface_id)
{
    (void)surface_id;
    return zzapp_display_flip_bound();
}
#else
__attribute__((weak)) void zzapp_api_enter(void) {}
__attribute__((weak)) void zzapp_api_leave(void) {}

__attribute__((weak))
int zzapp_display_bound(uint32_t *w, uint32_t *h, uint32_t *pitch,
                         uint32_t *format, uint32_t *card_front, uint32_t *card_back)
{
    (void)w; (void)h; (void)pitch; (void)format; (void)card_front; (void)card_back;
    return ZZ_ENOSYS;
}

__attribute__((weak)) int zzapp_display_flip_bound(void)
{
    return ZZ_ENOSYS;
}

__attribute__((weak)) int zzapp_svc_display_flip(uint32_t surface_id)
{
    (void)surface_id;
    return zzapp_display_flip_bound();
}
#endif

#if defined(MINIGL_HOST_TEST)
static uint8_t *g_test_p96;
static uint32_t g_test_p96_bytes;
void zzapp_gl_test_set_p96(uint8_t *pixels, uint32_t bytes)
{
    g_test_p96 = pixels;
    g_test_p96_bytes = bytes;
}
#endif

static int g_bound;
static uint32_t g_surface_id;
static zzapp_gl g_table;

#define CTX 1u
#define ENTER() zzapp_api_enter()
#define LEAVE() zzapp_api_leave()

static void w_Begin(uint32_t mode) { ENTER(); GLBegin(CTX, mode); LEAVE(); }
static void w_End(void) { ENTER(); GLEnd(CTX); LEAVE(); }
static void w_Vertex2f(float x, float y) { ENTER(); GLVertex4f(CTX, x, y, 0.0f, 1.0f); LEAVE(); }
static void w_Vertex3f(float x, float y, float z) { ENTER(); GLVertex4f(CTX, x, y, z, 1.0f); LEAVE(); }
static void w_Vertex3fv(const float *v) { ENTER(); GLVertex3fv(CTX, (GLfloat *)v); LEAVE(); }
static void w_TexCoord2f(float s, float t) { ENTER(); GLTexCoord2f(CTX, s, t); LEAVE(); }
static void w_TexCoord2fv(const float *v) { ENTER(); GLTexCoord2fv(CTX, (GLfloat *)v); LEAVE(); }
static void w_Color3f(float r, float g, float b) { ENTER(); GLColor4f(CTX, r, g, b, 1.0f); LEAVE(); }
static void w_Color3fv(const float *v) { ENTER(); GLColor3fv(CTX, (GLfloat *)v); LEAVE(); }
static void w_Color3ubv(const uint8_t *v) { ENTER(); GLColor3ubv(CTX, (GLubyte *)v); LEAVE(); }
static void w_Color4f(float r, float g, float b, float a) { ENTER(); GLColor4f(CTX, r, g, b, a); LEAVE(); }
static void w_Color4fv(const float *v) { ENTER(); if (v) GLColor4f(CTX, v[0], v[1], v[2], v[3]); LEAVE(); }
static void w_Color4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { ENTER(); GLColor4ub(CTX, r, g, b, a); LEAVE(); }
static void w_Color4ubv(const uint8_t *v) { ENTER(); GLColor4ubv(CTX, (GLubyte *)v); LEAVE(); }
static void w_Enable(uint32_t cap) { ENTER(); MGLSetState(CTX, cap, 1); LEAVE(); }
static void w_Disable(uint32_t cap) { ENTER(); MGLSetState(CTX, cap, 0); LEAVE(); }
static uint8_t w_IsEnabled(uint32_t cap) { uint8_t r; ENTER(); r = GLIsEnabled(CTX, cap); LEAVE(); return r; }
static void w_AlphaFunc(uint32_t func, float ref) { ENTER(); GLAlphaFunc(CTX, func, ref); LEAVE(); }
static void w_BlendFunc(uint32_t s, uint32_t d) { ENTER(); GLBlendFunc(CTX, s, d); LEAVE(); }
static void w_DepthFunc(uint32_t func) { ENTER(); GLDepthFunc(CTX, func); LEAVE(); }
static void w_DepthMask(uint8_t flag) { ENTER(); GLDepthMask(CTX, flag); LEAVE(); }
static void w_DepthRange(double n, double f) { ENTER(); GLDepthRange(CTX, n, f); LEAVE(); }
static void w_CullFace(uint32_t mode) { ENTER(); GLCullFace(CTX, mode); LEAVE(); }
static void w_FrontFace(uint32_t mode) { ENTER(); GLFrontFace(CTX, mode); LEAVE(); }
static void w_ShadeModel(uint32_t mode) { ENTER(); GLShadeModel(CTX, mode); LEAVE(); }
static void w_PolygonMode(uint32_t face, uint32_t mode) { ENTER(); GLPolygonMode(CTX, face, mode); LEAVE(); }
static void w_Clear(uint32_t mask) { ENTER(); GLClear(CTX, mask); LEAVE(); }
static void w_ClearColor(float r, float g, float b, float a) { ENTER(); GLClearColor(CTX, r, g, b, a); LEAVE(); }
static void w_Viewport(int32_t x, int32_t y, int32_t w, int32_t h) { ENTER(); GLViewport(CTX, x, y, w, h); LEAVE(); }
static void w_Scissor(int32_t x, int32_t y, int32_t w, int32_t h) { ENTER(); GLScissor(CTX, x, y, w, h); LEAVE(); }
static void w_MatrixMode(uint32_t mode) { ENTER(); GLMatrixMode(CTX, mode); LEAVE(); }
static void w_LoadIdentity(void) { ENTER(); GLLoadIdentity(CTX); LEAVE(); }
static void w_LoadMatrixf(const float *m) { ENTER(); GLLoadMatrixf(CTX, m); LEAVE(); }
static void w_PushMatrix(void) { ENTER(); GLPushMatrix(CTX); LEAVE(); }
static void w_PopMatrix(void) { ENTER(); GLPopMatrix(CTX); LEAVE(); }
static void w_Rotatef(float a, float x, float y, float z) { ENTER(); GLRotatef(CTX, a, x, y, z); LEAVE(); }
static void w_Translatef(float x, float y, float z) { ENTER(); GLTranslatef(CTX, x, y, z); LEAVE(); }
static void w_Scalef(float x, float y, float z) { ENTER(); GLScalef(CTX, x, y, z); LEAVE(); }
static void w_Ortho(double l, double r, double b, double t, double n, double f) { ENTER(); GLOrtho(CTX, l, r, b, t, n, f); LEAVE(); }
static void w_Frustum(double l, double r, double b, double t, double n, double f) { ENTER(); GLFrustum(CTX, l, r, b, t, n, f); LEAVE(); }
static void w_TexImage2D(uint32_t target, int32_t level, int32_t internalformat, int32_t width, int32_t height, int32_t border, uint32_t format, uint32_t type, const void *pixels) {
    ENTER(); GLTexImage2D(CTX, target, level, internalformat, width, height, border, format, type, pixels); LEAVE();
}
static void w_TexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height, uint32_t format, uint32_t type, const void *pixels) {
    ENTER(); GLTexSubImage2D(CTX, target, level, xoffset, yoffset, width, height, format, type, pixels); LEAVE();
}
static void w_TexParameteri(uint32_t target, uint32_t pname, int32_t param) { ENTER(); GLTexParameteri(CTX, target, pname, param); LEAVE(); }
/* The front end has no GLTexParameterf / GLTexEnvf. Quake passes enum constants. */
static void w_TexParameterf(uint32_t target, uint32_t pname, float param) { ENTER(); GLTexParameteri(CTX, target, pname, (GLint)param); LEAVE(); }
static void w_TexEnvi(uint32_t target, uint32_t pname, int32_t param) { ENTER(); GLTexEnvi(CTX, target, pname, param); LEAVE(); }
static void w_TexEnvf(uint32_t target, uint32_t pname, float param) { ENTER(); GLTexEnvi(CTX, target, pname, (GLint)param); LEAVE(); }
static void w_GenTextures(int32_t n, uint32_t *textures) { ENTER(); GLGenTextures(CTX, n, textures); LEAVE(); }
static void w_DeleteTextures(int32_t n, const uint32_t *textures) { ENTER(); GLDeleteTextures(CTX, n, textures); LEAVE(); }
static void w_BindTexture(uint32_t target, uint32_t texture) { ENTER(); GLBindTexture(CTX, target, texture); LEAVE(); }
static void w_Finish(void) { ENTER(); GLFinish(CTX); LEAVE(); }
static void w_Flush(void) { ENTER(); GLFlush(CTX); LEAVE(); }
static void w_DrawBuffer(uint32_t mode) { ENTER(); GLDrawBuffer(CTX, mode); LEAVE(); }
static void w_ReadBuffer(uint32_t mode) { ENTER(); GLReadBuffer(CTX, mode); LEAVE(); }
static void w_ReadPixels(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type, void *pixels) {
    ENTER(); GLReadPixels(CTX, x, y, width, height, format, type, pixels); LEAVE();
}
static const uint8_t *w_GetString(uint32_t name)
{
    static const uint8_t vendor[] = "ZZ9000";
    static const uint8_t version[] = "1.1";
    static const uint8_t rix[] = "ZZ9000 RasterIX";
    static const uint8_t cpu[] = "ZZ9000 CPU";
    mw_ctx *mw;
    const uint8_t *r = 0;
    ENTER();
    switch (name) {
    case 0x1F00u: r = vendor; break;
    case 0x1F02u: r = version; break;
    case 0x1F01u:
        mw = amiberry_transport_inproc_ctx();
        r = (mw && mw->rix_enabled) ? rix : cpu;
        break;
    default: break;
    }
    LEAVE();
    return r;
}
static void w_GetIntegerv(uint32_t pname, int32_t *params) { ENTER(); GLGetIntegerv(CTX, pname, params); LEAVE(); }
static void w_GetFloatv(uint32_t pname, float *params) { ENTER(); GLGetFloatv(CTX, pname, params); LEAVE(); }
static uint32_t w_GetError(void) { uint32_t r; ENTER(); r = GLGetError(CTX); LEAVE(); return r; }
static void w_Hint(uint32_t target, uint32_t mode) { ENTER(); GLHint(CTX, target, mode); LEAVE(); }
static void w_PixelStorei(uint32_t pname, int32_t param) { ENTER(); GLPixelStorei(CTX, pname, param); LEAVE(); }
static void w_Perspective(double fovy, double aspect, double zNear, double zFar) {
    ENTER(); GLUPerspective((GLfloat)fovy, (GLfloat)aspect, (GLfloat)zNear, (GLfloat)zFar); LEAVE();
}
static int32_t w_Build2DMipmaps(uint32_t target, int32_t internalFormat, int32_t width, int32_t height, uint32_t format, uint32_t type, const void *data) {
    int32_t r;
    ENTER();
    r = GLUBuild2DMipmaps(CTX, target, internalFormat, width, height, format, type, data);
    LEAVE();
    return r;
}

static void fill_table(void)
{
    memset(&g_table, 0, sizeof(g_table));
    g_table.size = (uint32_t)sizeof(g_table);
    g_table.Begin = w_Begin;
    g_table.End = w_End;
    g_table.Vertex2f = w_Vertex2f;
    g_table.Vertex3f = w_Vertex3f;
    g_table.Vertex3fv = w_Vertex3fv;
    g_table.TexCoord2f = w_TexCoord2f;
    g_table.TexCoord2fv = w_TexCoord2fv;
    g_table.Color3f = w_Color3f;
    g_table.Color3fv = w_Color3fv;
    g_table.Color3ubv = w_Color3ubv;
    g_table.Color4f = w_Color4f;
    g_table.Color4fv = w_Color4fv;
    g_table.Color4ub = w_Color4ub;
    g_table.Color4ubv = w_Color4ubv;
    g_table.Enable = w_Enable;
    g_table.Disable = w_Disable;
    g_table.IsEnabled = w_IsEnabled;
    g_table.AlphaFunc = w_AlphaFunc;
    g_table.BlendFunc = w_BlendFunc;
    g_table.DepthFunc = w_DepthFunc;
    g_table.DepthMask = w_DepthMask;
    g_table.DepthRange = w_DepthRange;
    g_table.CullFace = w_CullFace;
    g_table.FrontFace = w_FrontFace;
    g_table.ShadeModel = w_ShadeModel;
    g_table.PolygonMode = w_PolygonMode;
    g_table.Clear = w_Clear;
    g_table.ClearColor = w_ClearColor;
    g_table.Viewport = w_Viewport;
    g_table.Scissor = w_Scissor;
    g_table.MatrixMode = w_MatrixMode;
    g_table.LoadIdentity = w_LoadIdentity;
    g_table.LoadMatrixf = w_LoadMatrixf;
    g_table.PushMatrix = w_PushMatrix;
    g_table.PopMatrix = w_PopMatrix;
    g_table.Rotatef = w_Rotatef;
    g_table.Translatef = w_Translatef;
    g_table.Scalef = w_Scalef;
    g_table.Ortho = w_Ortho;
    g_table.Frustum = w_Frustum;
    g_table.TexImage2D = w_TexImage2D;
    g_table.TexSubImage2D = w_TexSubImage2D;
    g_table.TexParameteri = w_TexParameteri;
    g_table.TexParameterf = w_TexParameterf;
    g_table.TexEnvi = w_TexEnvi;
    g_table.TexEnvf = w_TexEnvf;
    g_table.GenTextures = w_GenTextures;
    g_table.DeleteTextures = w_DeleteTextures;
    g_table.BindTexture = w_BindTexture;
    g_table.Finish = w_Finish;
    g_table.Flush = w_Flush;
    g_table.DrawBuffer = w_DrawBuffer;
    g_table.ReadBuffer = w_ReadBuffer;
    g_table.ReadPixels = w_ReadPixels;
    g_table.GetString = w_GetString;
    g_table.GetIntegerv = w_GetIntegerv;
    g_table.GetFloatv = w_GetFloatv;
    g_table.GetError = w_GetError;
    g_table.Hint = w_Hint;
    g_table.PixelStorei = w_PixelStorei;
    g_table.Perspective = w_Perspective;
    g_table.Build2DMipmaps = w_Build2DMipmaps;
}

int zzapp_gl_bind(uint32_t surface_id, const struct zzapp_gl **out)
{
    uint32_t w, h, pitch, format, front, back;
    if (out)
        *out = 0;
    if (!surface_id)
        return ZZ_EINVAL;
    if (g_bound)
        return ZZ_EBUSY;
    if (zzapp_display_bound(&w, &h, &pitch, &format, &front, &back) != ZZ_OK)
        return ZZ_EINVAL;
    if (!w || !h || w > MW_MAX_DIMENSION || h > MW_MAX_DIMENSION || !pitch)
        return ZZ_EINVAL;
    if (format != ZZ_FMT_RGB565 && format != ZZ_FMT_BGRA8888)
        return ZZ_EINVAL;
    if (!amiberry_inproc_open(w, h))
        return ZZ_ENOMEM;
    fill_table();
    g_bound = 1;
    g_surface_id = surface_id;
    if (out)
        *out = &g_table;
    return ZZ_OK;
}

void zzapp_gl_unbind(void)
{
    if (!g_bound)
        return;
    amiberry_inproc_close();
    g_surface_id = 0;
    g_bound = 0;
}

void zzapp_gl_reclaim(void)
{
    zzapp_gl_unbind();
}

static uint32_t dest_format(uint32_t format)
{
    /* P96 16-bit is high-byte-first, matching sdk_surface.c's RGB565 store. */
    if (format == ZZ_FMT_RGB565)
        return ZZ9K_MGL_PIXFMT_RGB565_BE;
    return ZZ9K_MGL_PIXFMT_BGRA32;
}

static int copy_bound(const uint32_t *src, int rgb565, int bottom_up,
                      uint32_t w, uint32_t h, uint32_t pitch, uint32_t format,
                      uint32_t card)
{
    ZZ9KMiniGLPacketPresent pres;
    uint8_t *vram;
    uint32_t vram_len;
    uint32_t y;

    if (!src || !card || !w || !h || !pitch)
        return 0;
    memset(&pres, 0, sizeof(pres));
    pres.target_width = w;
    pres.target_height = h;
    pres.window_w = w;
    pres.window_h = h;
    pres.dest_offset = MNT_FB_BASE;
    pres.dest_pitch = pitch;
    pres.dest_format = dest_format(format);
#if defined(MINIGL_HOST_TEST)
    if (g_test_p96) {
        vram = g_test_p96;
        vram_len = g_test_p96_bytes;
    } else
#endif
    {
        vram = (uint8_t *)(uintptr_t)card;
        vram_len = pitch * h;
    }
    if (!vram || vram_len < pitch)
        return 0;

    /* present_copy addresses vram + dest_offset - MNT_FB_BASE. With
     * dest_offset == MNT_FB_BASE the write starts at vram, which is the
     * bound ARM address (or the host mock). */
    if (!bottom_up) {
        if (rgb565)
            return minigl_present_copy_rgb565(&pres, (const uint16_t *)src, vram, vram_len);
        return minigl_present_copy(&pres, src, vram, vram_len);
    }
    /* RasterIX RGB565 is bottom-up. present_copy has no flip flag. */
    pres.window_h = 1;
    for (y = 0; y < h; ++y) {
        int ok;
        pres.window_y = y;
        pres.source_y = h - 1u - y;
        if (rgb565)
            ok = minigl_present_copy_rgb565(&pres, (const uint16_t *)src, vram, vram_len);
        else
            ok = minigl_present_copy(&pres, src, vram, vram_len);
        if (!ok)
            return 0;
    }
    return 1;
}

void zzapp_gl_swap(void)
{
    uint32_t w, h, pitch, format, front, back;
    uint32_t card;
    mw_ctx *mw;
    struct AmiberryContext *guest;
    const uint32_t *src;
    int rgb565;

    zzapp_api_enter();
    if (!g_bound)
        goto done;
    if (zzapp_display_bound(&w, &h, &pitch, &format, &front, &back) != ZZ_OK)
        goto done;
    if (!w || !h || !pitch || h > UINT32_MAX / pitch)
        goto done;
    card = back ? back : front;
    guest = amiberry_context(mini_CurrentContext);
    if (guest && amiberry_has_pending_command(guest, MW_PRESENT)) {
        if (!amiberry_flush(guest))
            goto done;
    } else if (guest && (!amiberry_flush(guest) ||
                         !amiberry_command(guest, MW_PRESENT, 0) ||
                         !amiberry_flush(guest))) {
        goto done;
    }
    if (!minigl_raster_prepare_present() && !minigl_raster_finish())
        goto done;
    rgb565 = minigl_raster_present_is_rgb565();
    mw = amiberry_transport_inproc_ctx();
    if (!mw || !mw->color_buffers[mw->state.target.latest])
        goto done;
    src = mw->color_buffers[mw->state.target.latest];
    if (!copy_bound(src, rgb565, minigl_raster_present_is_bottom_up(),
                    w, h, pitch, format, card))
        goto done;
    minigl_platform_cache_clean(NULL, card, pitch * h);
    minigl_platform_barrier(NULL);
    (void)zzapp_svc_display_flip(g_surface_id);
done:
    zzapp_api_leave();
}
