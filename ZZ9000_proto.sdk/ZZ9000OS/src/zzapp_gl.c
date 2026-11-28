/*
 * App GL table and the present copy into the bound P96 bitmap.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The front end is compiled in from MINIGL_LIB_DIR with -DMGL_PLATFORM_ZZARM.
 * This file does not include amiberry_backend.c, amiberry_glu_quad.c, or vendor/.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "zzapp_gl_contract.h"

#include "aar/app.h"
#include "aar/ext_zz9000.h"
#include "aar/gl.h"

#include "memorymap.h"
#include "minigl_present.h"
#include "minigl_profile.h"
#include "minigl_platform.h"
#include "minigl_raster.h"
#include "minigl_renderer.h"
#include "mw_ctx.h"
#include "mw_types.h"
#include "amiberry_private.h"
#include "zzm_minigl_host.h"



#include "minigl_platform.h"




extern int amiberry_inproc_open(uint32_t width, uint32_t height);
extern void amiberry_inproc_close(void);
extern mw_ctx *amiberry_transport_inproc_ctx(void);
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
extern void GLVertexPointer(GLcontext, GLint, GLenum, GLsizei, const GLvoid *);
extern void GLTexCoordPointer(GLcontext, GLint, GLenum, GLsizei, const GLvoid *);
extern void GLColorPointer(GLcontext, GLint, GLenum, GLsizei, const GLvoid *);
extern void GLEnableClientState(GLcontext, GLenum);
extern void GLDisableClientState(GLcontext, GLenum);
extern void GLClientActiveTextureARB(GLcontext, GLenum);
extern void GLDrawArrays(GLcontext, GLenum, GLint, GLsizei);
extern void GLDrawElements(GLcontext, GLenum, GLsizei, GLenum, const GLvoid *);
extern void GLLockArrays(GLcontext, GLuint, GLsizei);
extern void GLUnlockArrays(GLcontext);
extern void GLClearDepth(GLcontext, GLclampd);
extern void GLColorMask(GLcontext, GLboolean, GLboolean, GLboolean, GLboolean);
extern void GLPolygonOffset(GLcontext, GLfloat, GLfloat);
extern void GLMultMatrixf(GLcontext, const GLfloat *);
extern void GLLoadMatrixd(GLcontext, const GLdouble *);
extern void GLMultMatrixd(GLcontext, const GLdouble *);
extern void GLGetDoublev(GLcontext, GLenum, GLdouble *);
extern void GLGetBooleanv(GLcontext, GLenum, GLboolean *);
extern void GLActiveTextureARB(GLcontext, GLenum);
extern void GLMultiTexCoord2fARB(GLcontext, GLenum, GLfloat, GLfloat);
extern void GLMultiTexCoord2fvARB(GLcontext, GLenum, GLfloat *);
extern void GLNormal3f(GLcontext, GLfloat, GLfloat, GLfloat);
extern void GLNormalPointer(GLcontext, GLenum, GLsizei, const GLvoid *);
extern void GLLightf(GLcontext, GLenum, GLenum, GLfloat);
extern void GLLightfv(GLcontext, GLenum, GLenum, const GLfloat *);
extern void GLLightModelf(GLcontext, GLenum, GLfloat);
extern void GLLightModelfv(GLcontext, GLenum, const GLfloat *);
extern void GLMaterialf(GLcontext, GLenum, GLenum, GLfloat);
extern void GLMaterialfv(GLcontext, GLenum, GLenum, const GLfloat *);
extern void GLColorMaterial(GLcontext, GLenum, GLenum);
extern void GLFogf(GLcontext, GLenum, GLfloat);
extern void GLFogfv(GLcontext, GLenum, GLfloat *);
extern void GLLineWidth(GLcontext, GLfloat);
extern void GLPointSize(GLcontext, GLfloat);
extern GLboolean GLIsTexture(GLcontext, GLuint);
extern void GLCopyTexImage2D(GLcontext, GLenum, GLint, GLenum, GLint, GLint,
                             GLsizei, GLsizei, GLint);
extern void GLCopyTexSubImage2D(GLcontext, GLenum, GLint, GLint, GLint, GLint,
                                GLint, GLsizei, GLsizei);
extern void GLTexEnvfv(GLcontext, GLenum, GLenum, const GLfloat *);
#if defined(MINIGL_MODULE)
#include "zzm_runtime.h"

static const zzm_app_gl_host_service_t *g_module_app_host;

int zzapp_gl_set_module_host(const zzm_app_gl_host_service_t *host)
{
    if (!host || !host->api_enter || !host->api_leave ||
        !host->bound_display || !host->flip_bound || !host->flip_idle ||
        !host->generation || !host->display_layout || !host->display_latest ||
        !host->stop_requested || !host->yield_wait || !host->wait_flip)
        return AAR_E_INVAL;
    g_module_app_host = host;
    return AAR_OK;
}

static int g_module_in_api;
static int g_module_in_worker;

void zzm_module_set_worker_context(int active)
{
    g_module_in_worker = active != 0;
}

int zzm_module_can_park(void)
{
    return g_module_in_api > 0 && !g_module_in_worker;
}

/* An app thread is inside a GL call, possibly parked on the ring. */
int zzm_module_api_active(void)
{
    return g_module_in_api > 0;
}

void zzm_module_api_enter(void)
{
    /* FRONT is the time inside an app's GL call that no deeper stage claims:
     * the front end's state handling and command packing. */
    if (g_module_in_api++ == 0)
        minigl_profile_push(MGLP_FRONT);
    g_module_app_host->api_enter();
}

void zzm_module_api_leave(void)
{
    if (g_module_in_api > 0)
        g_module_in_api--;
    /* The module no longer owns backend state after its outermost API call.
     * Park through the host while its API lock is still held, so the
     * scheduler executes the deferred wire pass before this call returns. */
    if (minigl_raster_feed_pending())
        (void)minigl_raster_try_feed(RIX_DRV_FEED_SOURCE_SVC);
    if (g_module_in_api == 0 && zzm_minigl_take_deferred_dispatch())
        g_module_app_host->yield_wait();
    if (g_module_in_api == 0) {
        minigl_profile_pop();
        minigl_profile_report();
    }
    g_module_app_host->api_leave();
}

int zzm_module_display_bound(uint32_t *w, uint32_t *h, uint32_t *pitch,
                             uint32_t *format, uint32_t *card_front,
                             uint32_t *card_back)
{
    return g_module_app_host->bound_display(w, h, pitch, format, card_front,
                                             card_back);
}
int zzm_module_display_layout(uint32_t *layout)
{
    return g_module_app_host->display_layout(layout);
}


int zzm_module_display_flip_bound(void)
{
    return g_module_app_host->flip_bound(g_module_app_host->generation());
}

int zzm_module_display_flip_idle(void)
{
    return g_module_app_host->flip_idle(g_module_app_host->generation());
}

int zzm_module_display_wait_flip_idle(void)
{
    return g_module_app_host->wait_flip(g_module_app_host->generation());
}

int zzm_module_svc_display_flip(uint32_t surface_id)
{
    (void)surface_id;
    return zzm_module_display_flip_bound();
}

int zzm_module_display_latest(void)
{
    return g_module_app_host->display_latest();
}

int zzm_module_stop_requested(void)
{
    return g_module_app_host->stop_requested();
}

void zzm_module_yield_wait(void)
{
    g_module_app_host->yield_wait();
}

uint32_t zzm_module_current_generation(void)
{
    return g_module_app_host->generation();
}
#else
__attribute__((weak)) int zzm_module_can_park(void) { return 0; }
__attribute__((weak)) void zzm_module_set_worker_context(int active) { (void)active; }
__attribute__((weak)) void zzapp_api_enter(void) {}
__attribute__((weak)) void zzapp_api_leave(void) {}

__attribute__((weak))
int zzapp_display_bound(uint32_t *w, uint32_t *h, uint32_t *pitch,
                         uint32_t *format, uint32_t *card_front, uint32_t *card_back)
{
    (void)w; (void)h; (void)pitch; (void)format; (void)card_front; (void)card_back;
    return AAR_E_NOSYS;
}
__attribute__((weak)) int zzapp_display_layout(uint32_t *layout)
{
    (void)layout;
    return AAR_E_NOSYS;
}


__attribute__((weak)) int zzapp_svc_display_flip(uint32_t surface_id)
{
    (void)surface_id;
    return AAR_E_NOSYS;
}

__attribute__((weak)) int zzapp_display_flip_idle(void)
{
    return 0;
}

__attribute__((weak)) int zzapp_display_wait_flip_idle(void)
{
    return AAR_E_NOSYS;
}

__attribute__((weak)) int zzapp_svc_display_flip_async(uint32_t surface_id)
{
    (void)surface_id;
    return AAR_E_NOSYS;
}

__attribute__((weak)) int zzapp_display_latest(void)
{
    return 0;
}

__attribute__((weak)) int zzapp_services_stop_requested(void)
{
    return 0;
}

__attribute__((weak)) void zzapp_host_yield_wait(void) {}

__attribute__((weak)) uint32_t zzapp_current_generation(void)
{
    return 0U;
}
#endif

#if defined(MINIGL_MODULE)
/*
 * The module has its own wrappers around the resident app host table.  Keep
 * them module-local by name: the resident runtime exports the same generic
 * names, and a host link of both images must not turn the service callbacks
 * into recursive calls back into this module.
 */
#define zzapp_api_enter zzm_module_api_enter
#define zzapp_api_leave zzm_module_api_leave
#define zzapp_display_bound zzm_module_display_bound
#define zzapp_display_layout zzm_module_display_layout
#define zzapp_display_flip_idle zzm_module_display_flip_idle
#define zzapp_display_wait_flip_idle zzm_module_display_wait_flip_idle
#define zzapp_svc_display_flip zzm_module_svc_display_flip
#define zzapp_display_latest zzm_module_display_latest
#define zzapp_services_stop_requested zzm_module_stop_requested
#define zzapp_host_yield_wait zzm_module_yield_wait
#define zzapp_current_generation zzm_module_current_generation
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
static aar_gl g_table;
/* Copy and mailbox direct can retain two frames. Queued direct uses two
 * display buffers and waits for each flip acknowledgement before drawing. */
static uint8_t g_present_queue[2];
static uint8_t g_present_count;
/* IDs stay with their sealed colour targets until the host takes ownership. */
static uint64_t g_present_ids[3];
static uint64_t g_present_enqueued_us[3];
static uint8_t g_tracked_present_count;
#if defined(MINIGL_MODULE)
static uint32_t g_bound_generation;
#endif
static uint8_t g_zero_copy;
static uint8_t g_mailbox_direct;
static uint32_t g_mailbox_generation;
static uint32_t g_bound_layout;
static uint8_t g_sync_present;
static uint32_t g_present_frame;

#define ZZAPP_GL_HANDOFF_TIMEOUT_US 30000000ULL
static uint64_t g_mailbox_submit_id;

#ifndef ZZAPP_GL_PROFILE_DEFAULT
#define ZZAPP_GL_PROFILE_DEFAULT 1
#endif

static uint32_t zzapp_gl_select_mailbox_target(mw_ctx *mw, uint32_t outgoing);

static int zzapp_gl_mailbox_supported(void)
{
#if defined(MINIGL_MODULE)
    return ZZM_SERVICE_VERSION_COMPATIBLE(g_module_app_host->version,
                                          ZZM_SERVICE_APP_GL_HOST_V4_2) &&
           g_module_app_host->size >= sizeof(*g_module_app_host) &&
           g_module_app_host->mailbox_buffers && g_module_app_host->mailbox_owned &&
           g_module_app_host->mailbox_present && g_module_app_host->mailbox_lease;
#else
    return 0;
#endif
}

static int zzapp_gl_mailbox_lease(int acquire)
{
#if defined(MINIGL_MODULE)
    return g_module_app_host->mailbox_lease(g_mailbox_generation, acquire);
#else
    (void)acquire;
    return AAR_E_NOSYS;
#endif
}

static int zzapp_gl_mailbox_owned(uint32_t *back, uint32_t *spare)
{
#if defined(MINIGL_MODULE)
    return g_module_app_host->mailbox_owned(g_mailbox_generation, back, spare);
#else
    (void)back;
    (void)spare;
    return AAR_E_NOSYS;
#endif
}

static int zzapp_gl_mailbox_present(uint32_t address, int gpu_written, uint64_t frame_id)
{
#if defined(MINIGL_MODULE)
    return g_module_app_host->mailbox_present(
        g_mailbox_generation, address, gpu_written ? ZZM_DISPLAY_PRESENT_NO_DIRTY : 0U,
        frame_id);
#else
    (void)address;
    (void)gpu_written;
    (void)frame_id;
    return AAR_E_NOSYS;
#endif
}

/* Keep the owned back in colour slot 1 (mw_state's initial back index).
 * Only the vblank may run between these reads; it changes front and spare,
 * never back. The addresses themselves do not change until display close. */
static int zzapp_gl_mailbox_targets(uint32_t buffers[3], uint32_t bytes)
{
#if defined(MINIGL_MODULE)
    uint32_t actual_bytes, back, spare;
    uint32_t i;

    /* Maintenance of a direct target must never touch the adjacent page. */
    if ((bytes & (MINIGL_RIX_COLOR_CACHE_LINE_BYTES - 1U)) != 0U)
        return 0;
    if (g_module_app_host->mailbox_buffers(g_mailbox_generation, buffers,
                                           &actual_bytes) != AAR_OK ||
        actual_bytes != bytes ||
        zzapp_gl_mailbox_owned(&back, &spare) != AAR_OK || !back)
        return 0;
    for (i = 0U; i < 3U; ++i) {
        uint32_t j;

        if (!buffers[i] || (buffers[i] & (MINIGL_RIX_MAILBOX_COLOR_ALIGNMENT - 1U)) != 0U) {
            minigl_platform_logf("[app] direct target %lu is not 256-byte aligned: %08lx",
                                 (unsigned long)i, (unsigned long)buffers[i]);
            return 0;
        }
        for (j = 0U; j < i; ++j) {
            uint32_t diff = buffers[i] > buffers[j] ?
                buffers[i] - buffers[j] : buffers[j] - buffers[i];
            if (diff < bytes)
                return 0;
        }
    }
    for (i = 0U; i < 3U; ++i) {
        if (buffers[i] == back) {
            buffers[i] = buffers[1];
            buffers[1] = back;
            return 1;
        }
    }
#else
    (void)buffers;
    (void)bytes;
#endif
    return 0;
}

static int zzapp_gl_direct_layout(unsigned mailbox)
{
    return g_bound_layout == AAR_LAYOUT_RGB565_LE ||
           (mailbox && g_bound_layout == AAR_LAYOUT_RGB565_BE);
}

#define CTX 1u
#define ENTER() zzapp_api_enter()
#define LEAVE() zzapp_api_leave()
#define ZZAPP_GL_MAX_CLIP_PLANES 0x0d32U

/* Solve modelview^T * eye_plane = equation in the front end's precision. */
static int clip_plane_to_eye(float out[4], const double *matrix,
                            const double equation[4])
{
    double a[4][5], eye[4], scale = 0.0;
    unsigned row, col, k;

    for (row = 0; row < 4U; ++row) {
        for (col = 0; col < 4U; ++col)
            a[row][col] = matrix[row * 4U + col];
        a[row][4] = equation[row];
    }
    for (col = 0; col < 4U; ++col) {
        unsigned pivot = col;

        for (row = col + 1U; row < 4U; ++row)
            if (fabs(a[row][col]) > fabs(a[pivot][col]))
                pivot = row;
        /* OpenGL leaves the plane undefined for a singular modelview. */
        if (a[pivot][col] == 0.0)
            return 0;
        if (pivot != col) {
            for (k = col; k < 5U; ++k) {
                double tmp = a[pivot][k];
                a[pivot][k] = a[col][k];
                a[col][k] = tmp;
            }
        }
        for (row = col + 1U; row < 4U; ++row) {
            double factor = a[row][col] / a[col][col];

            for (k = col + 1U; k < 5U; ++k)
                a[row][k] -= factor * a[col][k];
        }
    }
    for (row = 4U; row-- != 0U;) {
        double rhs = a[row][4];

        for (col = row + 1U; col < 4U; ++col)
            rhs -= a[row][col] * eye[col];
        eye[row] = rhs / a[row][row];
        if (!isfinite(eye[row]))
            return 0;
        if (fabs(eye[row]) > scale)
            scale = fabs(eye[row]);
    }
    /* Positive scaling preserves the half-space; zero never clips. */
    for (row = 0; row < 4U; ++row)
        out[row] = scale > 0.0 ? (float)(eye[row] / scale) : 0.0f;
    return 1;
}

static struct AmiberryContext *clip_context(void)
{
    struct AmiberryContext *c = amiberry_context(CTX);

    if (c && c->in_begin) {
        amiberry_error(c, GL_INVALID_OPERATION);
        return NULL;
    }
    return c;
}

static void w_ClipPlane(uint32_t plane, const double *equation)
{
    struct AmiberryContext *c;
    uint32_t index = plane - GL_CLIP_PLANE0;
    float eye[4];

    ENTER();
    c = clip_context();
    if (c) {
        if (index >= MW_USER_CLIP_PLANES)
            amiberry_error(c, GL_INVALID_ENUM);
        else if (clip_plane_to_eye(eye, c->modelview[c->model_top], equation)) {
            mw_state *s = &amiberry_transport_inproc_ctx()->state;

            /* Queued draws see the old equation until they submit. */
            if (!(s->user_clip_mask & (1U << index)) || amiberry_flush(c))
                memcpy(s->user_clip_plane[index], eye, sizeof(eye));
        }
    }
    LEAVE();
}

static void set_clip_enabled(uint32_t index, int enable)
{
    struct AmiberryContext *c = clip_context();
    mw_state *s;
    uint8_t bit;

    if (!c)
        return;
    s = &amiberry_transport_inproc_ctx()->state;
    bit = (uint8_t)(1U << index);
    if (((s->user_clip_mask & bit) != 0U) != enable && amiberry_flush(c))
        s->user_clip_mask ^= bit;
}

enum { CLIP_QUERY_OTHER = -1, CLIP_QUERY_INVALID = -2 };

static int32_t clip_query(uint32_t pname)
{
    uint32_t index = pname - GL_CLIP_PLANE0;

    if (pname != ZZAPP_GL_MAX_CLIP_PLANES && index >= MW_USER_CLIP_PLANES)
        return CLIP_QUERY_OTHER;
    if (!clip_context())
        return CLIP_QUERY_INVALID;
    if (pname == ZZAPP_GL_MAX_CLIP_PLANES)
        return (int32_t)MW_USER_CLIP_PLANES;
    return (amiberry_transport_inproc_ctx()->state.user_clip_mask &
            (1U << index)) != 0U;
}

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
static void w_Enable(uint32_t cap)
{
    uint32_t index = cap - GL_CLIP_PLANE0;

    ENTER();
    if (index < MW_USER_CLIP_PLANES)
        set_clip_enabled(index, 1);
    else
        MGLSetState(CTX, cap, 1);
    LEAVE();
}
static void w_Disable(uint32_t cap)
{
    uint32_t index = cap - GL_CLIP_PLANE0;

    ENTER();
    if (index < MW_USER_CLIP_PLANES)
        set_clip_enabled(index, 0);
    else
        MGLSetState(CTX, cap, 0);
    LEAVE();
}
static uint8_t w_IsEnabled(uint32_t cap)
{
    uint8_t result;

    ENTER();
    result = cap - GL_CLIP_PLANE0 < MW_USER_CLIP_PLANES ?
             clip_query(cap) > 0 : GLIsEnabled(CTX, cap);
    LEAVE();
    return result;
}
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
/* The binding rotates three colour targets and presents the back one, while
 * the front target is the frame the last swap queued.  The front names
 * therefore select the swap target too: the front end's own in-process rule
 * is that its one target answers to both. */
#define ZZAPP_GL_FRONT_LEFT 0x0400U
#define ZZAPP_GL_FRONT 0x0404U
#define ZZAPP_GL_BACK 0x0405U
#define ZZAPP_GL_FOG_COLOR 0x0b66U
#define ZZAPP_GL_STENCIL_BITS 0x0d57U
static uint32_t swap_target_name(uint32_t mode)
{
    return mode == ZZAPP_GL_FRONT || mode == ZZAPP_GL_FRONT_LEFT ?
           ZZAPP_GL_BACK : mode;
}
static void w_DrawBuffer(uint32_t mode) { ENTER(); GLDrawBuffer(CTX, swap_target_name(mode)); LEAVE(); }
static void w_ReadBuffer(uint32_t mode) { ENTER(); GLReadBuffer(CTX, swap_target_name(mode)); LEAVE(); }
static void w_ReadPixels(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type, void *pixels) {
    ENTER(); GLReadPixels(CTX, x, y, width, height, format, type, pixels); LEAVE();
}
static const uint8_t *w_GetString(uint32_t name)
{
    static const uint8_t vendor[] = "ZZ9000";
    static const uint8_t version[] = "1.1";
    /* The AAR table does not expose the MiniGL guest extensions. */
    static const uint8_t extensions[] = "";
    static const uint8_t rix[] = "ZZ9000 RasterIX";
    static const uint8_t cpu[] = "ZZ9000 CPU";
    mw_ctx *mw;
    const uint8_t *r = 0;
    ENTER();
    switch (name) {
    case 0x1F00u: r = vendor; break;
    case 0x1F02u: r = version; break;
    case 0x1F03u: r = extensions; break;
    case 0x1F01u:
        mw = amiberry_transport_inproc_ctx();
        r = (mw && mw->rix_enabled) ? rix : cpu;
        break;
    default: break;
    }
    LEAVE();
    return r;
}
/* There is no stencil attachment. Handle this query without creating a
 * front-end GL_INVALID_ENUM; other names still go through the front end. */
static void w_GetIntegerv(uint32_t pname, int32_t *params)
{
    int32_t value;

    ENTER();
    if (pname == ZZAPP_GL_STENCIL_BITS) {
        if (params)
            *params = 0;
    } else {
        value = clip_query(pname);
        if (value == CLIP_QUERY_OTHER)
            GLGetIntegerv(CTX, pname, params);
        else if (value >= 0 && params)
            *params = value;
    }
    LEAVE();
}
static void w_GetFloatv(uint32_t pname, float *params)
{
    int32_t value;

    ENTER();
    value = clip_query(pname);
    if (value == CLIP_QUERY_OTHER)
        GLGetFloatv(CTX, pname, params);
    else if (value >= 0 && params)
        *params = (float)value;
    LEAVE();
}
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
static void w_VertexPointer(int32_t size, uint32_t type, int32_t stride, const void *pointer) { ENTER(); GLVertexPointer(CTX, size, type, stride, pointer); LEAVE(); }
static void w_TexCoordPointer(int32_t size, uint32_t type, int32_t stride, const void *pointer) { ENTER(); GLTexCoordPointer(CTX, size, type, stride, pointer); LEAVE(); }
static void w_ColorPointer(int32_t size, uint32_t type, int32_t stride, const void *pointer) { ENTER(); GLColorPointer(CTX, size, type, stride, pointer); LEAVE(); }
static void w_EnableClientState(uint32_t cap) { ENTER(); GLEnableClientState(CTX, cap); LEAVE(); }
static void w_DisableClientState(uint32_t cap) { ENTER(); GLDisableClientState(CTX, cap); LEAVE(); }
static void w_ClientActiveTextureARB(uint32_t texture) { ENTER(); GLClientActiveTextureARB(CTX, texture); LEAVE(); }
static void w_DrawArrays(uint32_t mode, int32_t first, int32_t count) { ENTER(); GLDrawArrays(CTX, mode, first, count); LEAVE(); }
static void w_DrawElements(uint32_t mode, int32_t count, uint32_t type, const void *indices) { ENTER(); GLDrawElements(CTX, mode, count, type, indices); LEAVE(); }
static void w_LockArrays(uint32_t first, int32_t count) { ENTER(); GLLockArrays(CTX, first, count); LEAVE(); }
static void w_UnlockArrays(void) { ENTER(); GLUnlockArrays(CTX); LEAVE(); }
static void w_ClearDepth(double d) { ENTER(); GLClearDepth(CTX, d); LEAVE(); }
static void w_ColorMask(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { ENTER(); GLColorMask(CTX, r, g, b, a); LEAVE(); }
static void w_PolygonOffset(float factor, float units) { ENTER(); GLPolygonOffset(CTX, factor, units); LEAVE(); }
static void w_MultMatrixf(const float *m) { ENTER(); GLMultMatrixf(CTX, m); LEAVE(); }
static void w_LoadMatrixd(const double *m) { ENTER(); GLLoadMatrixd(CTX, m); LEAVE(); }
static void w_MultMatrixd(const double *m) { ENTER(); GLMultMatrixd(CTX, m); LEAVE(); }
static void w_GetDoublev(uint32_t pname, double *out)
{
    int32_t value;

    ENTER();
    value = clip_query(pname);
    if (value == CLIP_QUERY_OTHER)
        GLGetDoublev(CTX, pname, out);
    else if (value >= 0 && out)
        *out = (double)value;
    LEAVE();
}
static void w_GetBooleanv(uint32_t pname, uint8_t *out)
{
    int32_t value;

    ENTER();
    value = clip_query(pname);
    if (value == CLIP_QUERY_OTHER)
        GLGetBooleanv(CTX, pname, out);
    else if (value >= 0 && out)
        *out = value != 0;
    LEAVE();
}
static void w_ActiveTextureARB(uint32_t texture) { ENTER(); GLActiveTextureARB(CTX, texture); LEAVE(); }
static void w_MultiTexCoord2fARB(uint32_t target, float s, float t) { ENTER(); GLMultiTexCoord2fARB(CTX, target, s, t); LEAVE(); }
static void w_MultiTexCoord2fvARB(uint32_t target, const float *v) { ENTER(); GLMultiTexCoord2fvARB(CTX, target, (GLfloat *)v); LEAVE(); }
static void w_Normal3f(float x, float y, float z) { ENTER(); GLNormal3f(CTX, x, y, z); LEAVE(); }
static void w_Normal3fv(const float *v) { w_Normal3f(v[0], v[1], v[2]); }
static void w_NormalPointer(uint32_t type, int32_t stride, const void *p) { ENTER(); GLNormalPointer(CTX, type, stride, p); LEAVE(); }
static void w_Lightf(uint32_t light, uint32_t pname, float v) { ENTER(); GLLightf(CTX, light, pname, v); LEAVE(); }
static void w_Lightfv(uint32_t light, uint32_t pname, const float *v) { ENTER(); GLLightfv(CTX, light, pname, v); LEAVE(); }
static void w_LightModelf(uint32_t pname, float v) { ENTER(); GLLightModelf(CTX, pname, v); LEAVE(); }
static void w_LightModelfv(uint32_t pname, const float *v) { ENTER(); GLLightModelfv(CTX, pname, v); LEAVE(); }
static void w_LightModeli(uint32_t pname, int32_t v) { w_LightModelf(pname, (float)v); }
static void w_Materialf(uint32_t face, uint32_t pname, float v) { ENTER(); GLMaterialf(CTX, face, pname, v); LEAVE(); }
static void w_Materialfv(uint32_t face, uint32_t pname, const float *v) { ENTER(); GLMaterialfv(CTX, face, pname, v); LEAVE(); }
static void w_ColorMaterial(uint32_t face, uint32_t mode) { ENTER(); GLColorMaterial(CTX, face, mode); LEAVE(); }
static void w_Fogf(uint32_t pname, float v) { ENTER(); GLFogf(CTX, pname, v); LEAVE(); }
static void w_Fogfv(uint32_t pname, const float *v)
{
    ENTER();
    if (pname == ZZAPP_GL_FOG_COLOR)
        GLFogfv(CTX, pname, (GLfloat *)v);
    else
        GLFogf(CTX, pname, v[0]);
    LEAVE();
}
static void w_Fogi(uint32_t pname, int32_t v) { w_Fogf(pname, (float)v); }
static void w_LineWidth(float width) { ENTER(); GLLineWidth(CTX, width); LEAVE(); }
static void w_PointSize(float size) { ENTER(); GLPointSize(CTX, size); LEAVE(); }
static void w_Color3ub(uint8_t r, uint8_t g, uint8_t b) { w_Color4ub(r, g, b, 255U); }
static void w_Vertex2i(int32_t x, int32_t y) { w_Vertex2f((float)x, (float)y); }
static void w_Vertex2fv(const float *v) { w_Vertex2f(v[0], v[1]); }
static void w_TexCoord2i(int32_t s, int32_t t) { w_TexCoord2f((float)s, (float)t); }
static uint8_t w_IsTexture(uint32_t texture)
{
    uint8_t result;
    ENTER();
    result = GLIsTexture(CTX, texture);
    LEAVE();
    return result;
}
static void w_CopyTexImage2D(uint32_t target, int32_t level, uint32_t internal,
                             int32_t x, int32_t y, int32_t w, int32_t h, int32_t border)
{
    ENTER();
    GLCopyTexImage2D(CTX, target, level, internal, x, y, w, h, border);
    LEAVE();
}
static void w_CopyTexSubImage2D(uint32_t target, int32_t level, int32_t dx,
                                int32_t dy, int32_t x, int32_t y, int32_t w,
                                int32_t h)
{
    ENTER();
    GLCopyTexSubImage2D(CTX, target, level, dx, dy, x, y, w, h);
    LEAVE();
}
static void w_TexEnvfv(uint32_t target, uint32_t pname, const float *v) { ENTER(); GLTexEnvfv(CTX, target, pname, v); LEAVE(); }


static void fill_table(void)
{
    memset(&g_table, 0, sizeof(g_table));
    g_table.size = (uint32_t)sizeof(g_table);
    g_table.version = AAR_GL_VERSION_11;
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
    g_table.VertexPointer = w_VertexPointer;
    g_table.TexCoordPointer = w_TexCoordPointer;
    g_table.ColorPointer = w_ColorPointer;
    g_table.EnableClientState = w_EnableClientState;
    g_table.DisableClientState = w_DisableClientState;
    g_table.ClientActiveTextureARB = w_ClientActiveTextureARB;
    g_table.DrawArrays = w_DrawArrays;
    g_table.DrawElements = w_DrawElements;
    g_table.LockArrays = w_LockArrays;
    g_table.UnlockArrays = w_UnlockArrays;

    /* Leave unsupported operations NULL, including stencil, display
     * lists and attribute stacks. */
    g_table.ClearDepth = w_ClearDepth;
    g_table.ColorMask = w_ColorMask;
    g_table.PolygonOffset = w_PolygonOffset;
    g_table.MultMatrixf = w_MultMatrixf;
    g_table.LoadMatrixd = w_LoadMatrixd;
    g_table.MultMatrixd = w_MultMatrixd;
    g_table.GetDoublev = w_GetDoublev;
    g_table.GetBooleanv = w_GetBooleanv;
    g_table.ActiveTextureARB = w_ActiveTextureARB;
    g_table.MultiTexCoord2fARB = w_MultiTexCoord2fARB;
    g_table.MultiTexCoord2fvARB = w_MultiTexCoord2fvARB;
    g_table.Normal3f = w_Normal3f;
    g_table.Normal3fv = w_Normal3fv;
    g_table.NormalPointer = w_NormalPointer;
    g_table.Lightf = w_Lightf;
    g_table.Lightfv = w_Lightfv;
    g_table.LightModelf = w_LightModelf;
    g_table.LightModelfv = w_LightModelfv;
    g_table.LightModeli = w_LightModeli;
    g_table.Materialf = w_Materialf;
    g_table.Materialfv = w_Materialfv;
    g_table.ColorMaterial = w_ColorMaterial;
    g_table.Fogf = w_Fogf;
    g_table.Fogfv = w_Fogfv;
    g_table.Fogi = w_Fogi;
    g_table.LineWidth = w_LineWidth;
    g_table.PointSize = w_PointSize;
    g_table.Color3ub = w_Color3ub;
    g_table.Vertex2i = w_Vertex2i;
    g_table.Vertex2fv = w_Vertex2fv;
    g_table.TexCoord2i = w_TexCoord2i;
    g_table.IsTexture = w_IsTexture;
    g_table.CopyTexImage2D = w_CopyTexImage2D;
    g_table.CopyTexSubImage2D = w_CopyTexSubImage2D;
    g_table.ClipPlane = w_ClipPlane;
    g_table.TexEnvfv = w_TexEnvfv;
}

static int zzapp_gl_configure_targets(uint32_t width, uint32_t height,
                                      uint32_t buffer_count)
{
    struct AmiberryContext *guest = amiberry_context(mini_CurrentContext);
    UBYTE *payload;

    if (guest == NULL || (payload = amiberry_command(guest, MW_TARGET, 24U)) == NULL)
        return 0;
    mw_put_be32(payload, width);
    mw_put_be32(payload + 4U, height);
    mw_put_be32(payload + 8U, buffer_count);
    mw_put_be32(payload + 12U, 24U);
    mw_put_be32(payload + 16U, width);
    mw_put_be32(payload + 20U, height);
    if (!amiberry_flush(guest))
        return 0;
    /* The front end opens a one-buffer context drawing and reading the front
     * target.  With these rotating targets that is the frame the last swap
     * queued: the draws of each frame landed in the previous frame's buffer,
     * and the buffer a swap sealed kept its initial 0x000000ff fill (or an
     * older frame) until a later swap drew into it. */
    GLDrawBuffer(CTX, ZZAPP_GL_BACK);
    GLReadBuffer(CTX, ZZAPP_GL_BACK);
    return amiberry_flush(guest);
}


int zzapp_gl_bind(uint32_t surface_id, uint32_t flags,
                  const struct aar_gl **out)
{
    uint32_t w, h, pitch, format, front, back;
    uint32_t buffers[3];
    uint32_t bytes;
    int mailbox;
    mw_ctx *mw;
    const char *present_mode;

    if (out)
        *out = 0;
    if (!surface_id ||
        (flags & ~(AAR_ZZ9000_GL_BIND_DIRECT_PRESENT | AAR_ZZ9000_GL_BIND_SYNC_PRESENT |
                   AAR_ZZ9000_GL_BIND_SYNC_RIX_FEED |
                   AAR_ZZ9000_GL_BIND_INLINE_RIX_FEED)) != 0U)
        return AAR_E_INVAL;
    if (g_bound)
        return AAR_E_BUSY;
    if (zzapp_display_bound(&w, &h, &pitch, &format, &front, &back) != AAR_OK)
        return AAR_E_INVAL;
    if (!w || !h || w > MW_MAX_DIMENSION || h > MW_MAX_DIMENSION || !pitch)
        return AAR_E_INVAL;
    if (format != AAR_FMT_RGB565 && format != AAR_FMT_BGRA8888)
        return AAR_E_INVAL;
    g_bound_layout = 0U;
    (void)zzapp_display_layout(&g_bound_layout);
    bytes = w * h * 2U;
    mailbox = zzapp_display_latest();
    g_zero_copy = 0U;
    g_mailbox_direct = 0U;
    if ((flags & AAR_ZZ9000_GL_BIND_DIRECT_PRESENT) == 0U)
        present_mode = "copy (direct not requested)";
    else if (format != AAR_FMT_RGB565)
        present_mode = "copy (not RGB565)";
    else if (!zzapp_gl_direct_layout(mailbox))
        present_mode = "copy (RGB565 layout)";
    else if (pitch != w * 2U)
        present_mode = "copy (padded pitch)";
    else if (mailbox) {
        g_mailbox_generation = zzapp_current_generation();
        if (!zzapp_gl_mailbox_supported())
            present_mode = "copy (mailbox host ABI)";
        else if (!zzapp_gl_mailbox_targets(buffers, bytes))
            present_mode = "copy (mailbox buffers)";
        else {
            g_mailbox_direct = 1U;
            present_mode = "direct";
        }
    } else if (!front || !back)
        present_mode = "copy (missing buffers)";
    else if (((front | back) & 2047U) != 0U)
        present_mode = "copy (unaligned buffers)";
    else {
        g_zero_copy = 1U;
        present_mode = "direct";
    }
    minigl_raster_set_async_feed((flags & AAR_ZZ9000_GL_BIND_SYNC_RIX_FEED) == 0U);
    minigl_platform_set_memfetch_enabled(
        (flags & AAR_ZZ9000_GL_BIND_INLINE_RIX_FEED) == 0U);
    if (!amiberry_inproc_open(w, h)) {
        minigl_raster_set_async_feed(0);
        minigl_platform_set_memfetch_enabled(1);
        return AAR_E_NOMEM;
    }
    mw = amiberry_transport_inproc_ctx();
    if (g_zero_copy && (!mw || !mw_host_bind_direct_rgb565(mw, front, back, 0U,
                                                            bytes))) {
        g_zero_copy = 0U;
        present_mode = "copy (direct buffer bind)";
    }
    if (!zzapp_gl_configure_targets(w, h, g_zero_copy ? 2U : 3U)) {
        amiberry_inproc_close();
        g_zero_copy = 0U;
        g_mailbox_direct = 0U;
        minigl_raster_set_async_feed(0);
        minigl_platform_set_memfetch_enabled(1);
        return AAR_E_NOMEM;
    }
    if (g_mailbox_direct) {
        int leased = mw && mw->rix_enabled && zzapp_gl_mailbox_lease(1) == AAR_OK;

        if (leased && g_bound_layout == AAR_LAYOUT_RGB565_BE)
            mw->direct_color_be = minigl_raster_set_color_byte_swap(1);
        if (leased &&
            (g_bound_layout != AAR_LAYOUT_RGB565_BE || mw->direct_color_be) &&
            mw_host_bind_direct_rgb565(mw, buffers[0], buffers[1], buffers[2],
                                       bytes)) {
            mw->present_select = zzapp_gl_select_mailbox_target;
        } else {
            g_mailbox_direct = 0U;
            if (mw && mw->direct_color_be) {
                (void)minigl_raster_set_color_byte_swap(0);
                mw->direct_color_be = 0;
            }
            if (leased)
                (void)zzapp_gl_mailbox_lease(0);
            present_mode = "copy (direct buffer bind)";
        }
    }
    /* The module's stdout is not wired to the UART; the module log is. */
    minigl_platform_logf("[app] present: %s", present_mode);
    if ((flags & AAR_ZZ9000_GL_BIND_SYNC_RIX_FEED) != 0U)
        minigl_platform_logf("[app] gl feed: sync");
    else
        minigl_platform_logf("[app] gl feed: async %s",
                             (flags & AAR_ZZ9000_GL_BIND_INLINE_RIX_FEED) != 0U ?
                             "inline" : "memfetch");
    fill_table();
    g_bound = 1;
    g_surface_id = surface_id;
#if defined(MINIGL_MODULE)
    g_bound_generation = zzapp_current_generation();
#endif
    g_present_count = 0U;
    g_sync_present = (flags & AAR_ZZ9000_GL_BIND_SYNC_PRESENT) != 0U;
    g_present_frame = 0U;
    /* One [MGLP] UART line per 5 s of GL work: where an ARM app's frame
     * time goes (app vs MiniGL stages, hardware vs CPU draws, fallback
     * reasons).  68k clients switch it on through trace_enabled.  A module
     * built with ZZAPP_GL_PROFILE_DEFAULT=0 leaves it off for ARM apps, for
     * timing runs that must not pay for the stage clocks or the UART. */
    minigl_profile_set_enabled(ZZAPP_GL_PROFILE_DEFAULT);
    if (out)
        *out = &g_table;
    return AAR_OK;
}

static void zzapp_gl_clear_present_id(uint32_t source)
{
    g_present_ids[source] = 0U;
    --g_tracked_present_count;
}

static void zzapp_gl_finish_target(uint32_t source, uint32_t reason)
{
#if defined(MINIGL_MODULE)
    uint64_t frame_id = g_present_ids[source];

    if (frame_id != 0U) {
        zzapp_gl_clear_present_id(source);
        if (reason != AAR_PRESENT_REASON_CANCELLED && zzapp_services_stop_requested())
            reason = AAR_PRESENT_REASON_CANCELLED;
        g_module_app_host->finish_tracked(g_bound_generation, frame_id, reason);
    }
#else
    (void)source;
    (void)reason;
#endif
}

/* Feedback can expire while a target still belongs to the renderer. Leave
 * the target in the queue so a later swap can advance it under the same
 * fence and display-ownership rules as an ordinary frame. */
static int zzapp_gl_expire_tracked(void)
{
    uint64_t now;
    unsigned i;
    int expired = 0;

    if (g_tracked_present_count == 0U)
        return 0;
    now = minigl_platform_time_us();
    for (i = 0U; i < g_present_count; ++i) {
        uint32_t source = g_present_queue[i];

        if (g_present_ids[source] != 0U &&
            now - g_present_enqueued_us[source] >= ZZAPP_GL_HANDOFF_TIMEOUT_US) {
            zzapp_gl_finish_target(source, AAR_PRESENT_REASON_TIMEOUT);
            expired = 1;
        }
    }
    return expired;
}

void zzapp_gl_unbind(void)
{
    unsigned i;

    if (!g_bound)
        return;
    for (i = 0U; i < g_present_count; ++i)
        zzapp_gl_finish_target(g_present_queue[i], AAR_PRESENT_REASON_CANCELLED);
    amiberry_inproc_close();
    if (g_mailbox_direct)
        (void)zzapp_gl_mailbox_lease(0);
    g_surface_id = 0;
    g_present_count = 0U;
    g_zero_copy = 0U;
    g_mailbox_direct = 0U;
    g_sync_present = 0U;
    g_bound_layout = 0U;
    g_bound = 0;
    minigl_raster_set_async_feed(0);
    minigl_platform_set_memfetch_enabled(1);
    minigl_profile_set_enabled(0);
}

void zzapp_gl_reclaim(void)
{
    /* Forced app reclamation discards a parked GL stack without its normal
     * API epilogue, so no stale call may keep worker dispatch deferred. */
    g_module_in_api = 0;
    zzapp_gl_unbind();
}

static uint32_t dest_format(uint32_t format, uint32_t layout)
{
    if (format == AAR_FMT_RGB565) {
        /* P96 defaults to high-byte-first; the bound layout decides. */
        if (layout == AAR_LAYOUT_RGB565_LE)
            return ZZ9K_MGL_PIXFMT_RGB565_LE;
        return ZZ9K_MGL_PIXFMT_RGB565_BE;
    }
    if (layout == AAR_LAYOUT_RGBA8888)
        return ZZ9K_MGL_PIXFMT_RGBA32;
    return ZZ9K_MGL_PIXFMT_BGRA32;
}

/* The MiniGL present helpers have no BGR565 pixel format; the R/B-swapped
 * variants of the bound layout are written here directly. */
static int copy_bgr565(const uint32_t *src_rgba, int rgb565_source,
                       uint32_t w, uint32_t h, uint32_t pitch, uint8_t *vram,
                       uint32_t vram_len, int little_end)
{
    uint32_t x;
    uint32_t y;

    if (!src_rgba || !vram || vram_len < pitch || pitch < w * 2U)
        return 0;
    for (y = 0; y < h; y++) {
        uint8_t *row = vram + y * pitch;
        const uint32_t *in = src_rgba + y * w;

        for (x = 0; x < w; x++) {
            uint16_t value;

            if (rgb565_source) {
                uint16_t rgb = ((const uint16_t *)in)[x];

                value = (uint16_t)(((rgb & 0x001fu) << 11) |
                                   (rgb & 0x07e0u) |
                                   ((rgb & 0xf800u) >> 11));
            } else {
                uint32_t c = in[x];

                value = (uint16_t)((((c >> 11) & 0x1fu) << 11) |
                                   ((c >> 13) & 0x7e0u) |
                                   (((c >> 24) & 0xf8u) >> 3));
            }
            row[x * 2U] = little_end ? (uint8_t)value : (uint8_t)(value >> 8);
            row[x * 2U + 1U] = little_end ? (uint8_t)(value >> 8)
                                          : (uint8_t)value;
        }
    }
    return 1;
}

static int copy_bound(const uint32_t *src, int rgb565,
                      uint32_t w, uint32_t h, uint32_t pitch, uint32_t format,
                      uint32_t card)
{
    ZZ9KMiniGLPacketPresent pres;
    uint8_t *vram;
    uint32_t vram_len;

    if (!src || !card || !w || !h || !pitch)
        return 0;
    memset(&pres, 0, sizeof(pres));
    pres.target_width = w;
    pres.target_height = h;
    pres.window_w = w;
    pres.window_h = h;
    pres.dest_offset = MNT_FB_BASE;
    pres.dest_pitch = pitch;
    pres.dest_format = dest_format(format, g_bound_layout);
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

    if (format == AAR_FMT_RGB565 &&
        (g_bound_layout == AAR_LAYOUT_BGR565_BE ||
         g_bound_layout == AAR_LAYOUT_BGR565_LE)) {
        return copy_bgr565(src, rgb565, w, h, pitch, vram, vram_len,
                           g_bound_layout == AAR_LAYOUT_BGR565_LE);
    }
    /* present_copy addresses vram + dest_offset - MNT_FB_BASE. With
     * dest_offset == MNT_FB_BASE the write starts at vram, which is the
     * bound ARM address (or the host mock). */
    /* No row flip for the RGB565 layout: it is the same memory the RGBA
     * view converts in place, and that view has always presented unflipped
     * and correct on hardware (app-runtime-25: all-RasterIX frames,
     * converted per packet). The earlier flip came from the 68k span
     * convention and double-flipped. */
    if (rgb565)
        return minigl_present_copy_rgb565(&pres, (const uint16_t *)src, vram, vram_len);
    return minigl_present_copy(&pres, src, vram, vram_len);
}

static int zzapp_gl_queue_flip(uint32_t surface_id)
{
#if defined(MINIGL_MODULE)
    return zzapp_svc_display_flip(surface_id);
#else
    return zzapp_svc_display_flip_async(surface_id);
#endif
}

static void zzapp_gl_release_target(mw_ctx *mw, uint32_t source)
{
    mw->present_queued[source] = 0U;
    if (--g_present_count != 0U)
        g_present_queue[0] = g_present_queue[1];
}

static void zzapp_gl_present_layout_mismatch(const mw_ctx *mw, uint32_t source,
                                             uint8_t current)
{
#ifndef MINIGL_HOST_TEST
    static uint64_t window_us;
    static uint32_t count;
    uint64_t now;

    ++count;
    now = minigl_platform_time_us();
    if (now - window_us < 1000000ULL)
        return;
    window_us = now;
    minigl_platform_logf("[present] b=%lu sub=%lu flag=%u->%u reason=copy n=%lu",
                         (unsigned long)source,
                         (unsigned long)mw->present_submit_frame[source],
                         (unsigned)mw->present_submit_rgb565[source],
                         (unsigned)current, (unsigned long)count);
    count = 0U;
#else
    (void)mw;
    (void)source;
    (void)current;
#endif
}

#if defined(MINIGL_HOST_TEST)
static uint32_t g_test_present_invalidates;
void zzapp_gl_test_reset_invalidates(void) { g_test_present_invalidates = 0U; }
uint32_t zzapp_gl_test_invalidates(void) { return g_test_present_invalidates; }
#endif

static void present_cache_invalidate(uintptr_t address, size_t bytes)
{
#if defined(MINIGL_HOST_TEST)
    g_test_present_invalidates++;
#endif
    minigl_platform_cache_invalidate(NULL, address, bytes);
}

#if defined(MINIGL_HOST_TEST)
/* Direct seam for byte-exact layout checks: sets the bound layout like
 * zzapp_gl_bind would and runs one copy. */
int zzapp_gl_test_copy_bound(const uint32_t *src, int rgb565,
                             uint32_t w, uint32_t h, uint32_t pitch,
                             uint32_t format, uint32_t card, uint32_t layout)
{
    int ok;

    g_bound_layout = layout;
    ok = copy_bound(src, rgb565, w, h, pitch, format, card);
    g_bound_layout = 0U;
    return ok;
}
#endif

/* The address RasterIX renders colour target `index` at. */
static uintptr_t zzapp_gl_target_address(const mw_ctx *mw, uint32_t index)
{
    return mw->color_direct[index] ?
        (uintptr_t)mw->direct_color_buffers[index] :
        (uintptr_t)mw->color_buffers[index];
}

/* 1: handed off; -1: failed handoff; 0: no handoff yet. */
static int zzapp_gl_present_pending(mw_ctx *mw, int deferred)
{
    uint32_t w, h, pitch, format, front, back, card;
    uint32_t source;
    uint32_t copy_us, flip_us;
    uint64_t started;
    uint64_t frame_id;
    int flip_status;
    int rgb565;

    if (!deferred)
        (void)zzapp_gl_expire_tracked();
    if (g_present_count == 0U)
        return 0;
    source = g_present_queue[0];
    if (source >= 3U)
        return 0;
    frame_id = g_present_ids[source];
    if (!minigl_raster_target_ready(zzapp_gl_target_address(mw, source)))
        return 0;
    if (g_mailbox_direct) {
        w = mw->width;
        h = mw->height;
        pitch = w * 2U;
        format = AAR_FMT_RGB565;
    } else {
        if (!zzapp_display_flip_idle())
            return 0;
        if (zzapp_display_bound(&w, &h, &pitch, &format, &front, &back) != AAR_OK ||
            !w || !h || !pitch || h > UINT32_MAX / pitch)
            goto failed;
    }
    if (!mw->color_direct[source] && !mw->color_buffers[source])
        goto failed;
    card = g_mailbox_direct ? (uint32_t)(uintptr_t)mw->direct_color_buffers[source] :
                              (back ? back : front);
    rgb565 = mw->color_rgb565[source] != 0U;
    if (mw->present_submit_rgb565[source] != (uint8_t)rgb565)
        zzapp_gl_present_layout_mismatch(mw, source, (uint8_t)rgb565);
    started = minigl_platform_time_us();
    minigl_profile_push(MGLP_SWAP_COPY);
    if (g_mailbox_direct && mw->color_direct[source]) {
        /* RasterIX has finished its HP writes. No CPU has written the
         * frame; scanout reads DDR directly and needs no cache operation. */
    } else if (mw->color_direct[source]) {
        if ((uint32_t)(uintptr_t)mw->direct_color_buffers[source] != card) {
            minigl_profile_pop();
            goto failed;
        }
        /* RasterIX writes DDR directly.  Discard any old CPU view before
         * aar-run's flip service performs its scanout cache hand-off. */
        present_cache_invalidate((uintptr_t)mw->direct_color_buffers[source],
                                 (size_t)w * h * 2U);
        minigl_platform_barrier(NULL);
    } else if (rgb565) {
        /*
         * RasterIX owns the RGB565 half of this colour target and wrote it
         * over DDR. Its fence was observed above, but the ARM still needs
         * to discard cached target lines before the NEON row copy reads
         * them.
         */
        present_cache_invalidate((uintptr_t)mw->color_buffers[source],
                                 (size_t)w * h * 2U);
        if (!copy_bound(mw->color_buffers[source], rgb565, w, h, pitch, format, card)) {
            minigl_profile_pop();
            goto failed;
        }
    } else {
        /*
         * CPU-written RGBA target (software frame, or a RasterIX frame
         * that finished back to RGBA through minigl_raster_finish). This
         * core's cache holds the only up-to-date lines; invalidating
         * before copy_bound reads them back would discard them.
         */
        if (!copy_bound(mw->color_buffers[source], rgb565, w, h, pitch, format, card)) {
            minigl_profile_pop();
            goto failed;
        }
    }
    minigl_profile_pop();
    copy_us = (uint32_t)(minigl_platform_time_us() - started);
    started = minigl_platform_time_us();
    minigl_profile_push(MGLP_SWAP_FLIP);
    if (g_mailbox_direct)
        flip_status = zzapp_gl_mailbox_present(card, mw->color_direct[source], frame_id);
    else {
#if defined(MINIGL_MODULE)
        if (frame_id != 0U || deferred)
            flip_status = g_module_app_host->flip_bound_tracked(g_bound_generation, frame_id);
        else
#endif
            flip_status = zzapp_gl_queue_flip(g_surface_id);
    }
    minigl_profile_pop();
    if (flip_status == AAR_E_BUSY)
        return 0;
    if (flip_status != AAR_OK)
        goto failed;
    if (frame_id != 0U)
        zzapp_gl_clear_present_id(source);
    flip_us = (uint32_t)(minigl_platform_time_us() - started);
    mw_ctx_diag_add_present_wait(mw, source, copy_us, flip_us, 0U);
    mw_ctx_diag_presented(mw, source, (const uint8_t *)(uintptr_t)card,
                          pitch, format, w, h);
    zzapp_gl_release_target(mw, source);
    return 1;

failed:
    if (g_mailbox_direct) {
        zzapp_gl_finish_target(source, AAR_PRESENT_REASON_BACKEND);
        return -1;
    }
    if (frame_id == 0U && !deferred)
        return 0;
    zzapp_gl_finish_target(source, AAR_PRESENT_REASON_BACKEND);
    /* Without a handoff the direct buffers did not change display ownership. */
    if (g_zero_copy)
        return 0;
    zzapp_gl_release_target(mw, source);
    return -1;
}

#if defined(MINIGL_MODULE)
int zzapp_gl_has_present_work(void)
{
    /* Queued P96 direct needs the submitting app thread for its ownership wait. */
    return g_tracked_present_count != 0U && (!g_zero_copy || g_mailbox_direct);
}

static int zzapp_gl_poll_present_owner(void)
{
    mw_ctx *mw;

    if (zzapp_gl_expire_tracked())
        return 1;
    mw = amiberry_transport_inproc_ctx();

    /* Always process the head, even if it is an untracked predecessor. */
    return mw && zzapp_gl_present_pending(mw, 1) != 0;
}

int zzapp_gl_poll_present(void)
{
    return zzapp_gl_has_present_work() &&
           g_module_app_host->poll_present(g_bound_generation, zzapp_gl_poll_present_owner);
}
#endif

static void zzapp_gl_enqueue(mw_ctx *mw, uint32_t outgoing, uint64_t frame_id)
{
    if (frame_id != 0U) {
        g_present_ids[outgoing] = frame_id;
        g_present_enqueued_us[outgoing] = minigl_platform_time_us();
        ++g_tracked_present_count;
    }
    g_present_queue[g_present_count++] = (uint8_t)outgoing;
    mw->present_queued[outgoing] = 1U;
    mw->present_submit_rgb565[outgoing] = mw->color_rgb565[outgoing];
    mw->present_submit_frame[outgoing] = ++g_present_frame;
    if (minigl_raster_feed_pending())
        (void)minigl_raster_try_feed(RIX_DRV_FEED_SOURCE_SWAP);
}

/* After sealing the draw target, submit every ready frame in order. Only
 * the card can retire pending into shown, and only our submits take away an
 * owned buffer. A vblank between the query and the rebind can only add a
 * spare, so an owned, unqueued slot stays writable. */
static uint32_t zzapp_gl_select_mailbox_target(mw_ctx *mw, uint32_t outgoing)
{
    uint64_t waiting_since = 0U;
    int waited = 0;

    if (g_present_count == 2U)
        return UINT32_MAX;
    zzapp_gl_enqueue(mw, outgoing, g_mailbox_submit_id);
    if (g_sync_present && !minigl_raster_drain())
        return UINT32_MAX;
    for (;;) {
        uint32_t owned[2];
        uint32_t i, j;
        int presented;

        do {
            presented = zzapp_gl_present_pending(mw, 0);
        } while (presented > 0);
        if (presented < 0 || zzapp_gl_mailbox_owned(&owned[0], &owned[1]) != AAR_OK)
            return UINT32_MAX;
        for (i = 0U; i < 2U; ++i) {
            if (!owned[i])
                continue;
            for (j = 0U; j < 3U; ++j) {
                if ((uintptr_t)mw->direct_color_buffers[j] == owned[i] &&
                    !mw->present_queued[j]) {
                    if (waited)
                        mw_ctx_diag_add_wait(mw, 0U, 0U, 0U,
                            (uint32_t)(minigl_platform_time_us() - waiting_since));
                    return j;
                }
            }
        }
        /* Pending might latch before the GPU finishes, so a wait on just
         * the GPU would stall unnecessarily. Poll both after each yield. */
        if (zzapp_services_stop_requested())
            return UINT32_MAX;
        if (!waited) {
            waiting_since = minigl_platform_time_us();
            waited = 1;
        } else if (minigl_platform_time_us() - waiting_since >= 3000000U) {
            minigl_platform_log("[app] direct present target wait timed out");
            return UINT32_MAX;
        }
        zzapp_host_yield_wait();
    }
}

/* The host flip wait registers with the core-1 service pass, which wakes
 * this thread as soon as the scanout acknowledgement lands, and reports a
 * flip that takes unusually long. */
static int zzapp_gl_wait_flip_idle(void)
{
    return zzapp_display_wait_flip_idle() == AAR_OK;
}


static int zzapp_gl_make_target_available(mw_ctx *mw)
{
    while (g_present_count == 2U) {
        uintptr_t oldest = zzapp_gl_target_address(mw, g_present_queue[0]);

        if (zzapp_gl_present_pending(mw, 0))
            continue;
        if (!minigl_raster_target_ready(oldest)) {
            int ok;
            uint64_t started = minigl_platform_time_us();

            minigl_profile_push(MGLP_SWAP_DRAIN);
            ok = minigl_raster_wait_target(oldest);
            minigl_profile_pop();
            mw_ctx_diag_add_wait(mw, 0U, 0U,
                                 (uint32_t)(minigl_platform_time_us() - started), 0U);
            if (!ok) {
                /* Keep the target fenced after a failed drain. */
                zzapp_gl_finish_target(g_present_queue[0], AAR_PRESENT_REASON_BACKEND);
                return 0;
            }
            continue;
        }
        {
            uint64_t started = minigl_platform_time_us();

            int idle;

            minigl_profile_push(MGLP_SWAP_FLIP);
            idle = zzapp_gl_wait_flip_idle();

            minigl_profile_pop();
            if (!idle)
                return 0;
            mw_ctx_diag_add_wait(mw, 0U,
                                 (uint32_t)(minigl_platform_time_us() - started), 0U, 0U);
        }
    }
    return 1;
}

static int zzapp_gl_swap_frame(uint64_t frame_id)
{
    mw_ctx *mw;
    struct AmiberryContext *guest;
    uint32_t outgoing;
    uint64_t target_started;
    int status = AAR_E_IO;

    zzapp_api_enter();
    if (!g_bound) {
        status = AAR_E_NOSYS;
        goto done;
    }
    mw = amiberry_transport_inproc_ctx();
    if (!mw || mw->present_lost)
        goto done;
    if (!g_mailbox_direct) {
        /* Copy has fixed rotation; make room before rebinding its target. */
        (void)zzapp_gl_present_pending(mw, 0);
        target_started = minigl_platform_time_us();
        if (!zzapp_gl_make_target_available(mw))
            goto done;
        mw_ctx_diag_add_wait(mw, 0U, 0U, 0U,
                             (uint32_t)(minigl_platform_time_us() - target_started));
    }
    outgoing = mw->state.target.back;
    guest = amiberry_context(mini_CurrentContext);
    if (!guest ||
        (frame_id != 0U && (outgoing >= 3U || zzapp_services_stop_requested())))
        goto done;
    g_mailbox_submit_id = frame_id;
    if (amiberry_has_pending_command(guest, MW_PRESENT)) {
        if (!amiberry_flush(guest))
            goto done;
    } else if (guest && (!amiberry_flush(guest) ||
                         !amiberry_command(guest, MW_PRESENT, 0) ||
                         !amiberry_flush(guest))) {
        goto done;
    }
    if (g_mailbox_direct) {
        /* MW_PRESENT queued and selected a target through the hook. */
        if (!mw->present_lost && mw->state.target.back != outgoing)
            status = AAR_OK;
        minigl_profile_report();
        goto done;
    }
    if (outgoing >= 3U || g_present_count == 2U)
        goto done;
    if (frame_id != 0U && mw->state.target.back == outgoing)
        goto done;
    zzapp_gl_enqueue(mw, outgoing, frame_id);
    status = AAR_OK;
    if (g_zero_copy) {
        int ok;
        uint64_t started;

        /* With two P96 buffers, MW_PRESENT has rebound RasterIX to the
         * currently displayed one.  Do not return to the app until the flip
         * makes it hidden; this deliberately trades the old two-frame queue
         * for ownership safety. */
        started = minigl_platform_time_us();
        minigl_profile_push(MGLP_SWAP_DRAIN);
        ok = minigl_raster_wait_target(zzapp_gl_target_address(mw, outgoing));
        minigl_profile_pop();
        mw_ctx_diag_add_present_wait(mw, outgoing, 0U, 0U,
                                     (uint32_t)(minigl_platform_time_us() - started));
        if (!ok) {
            zzapp_gl_finish_target(outgoing, AAR_PRESENT_REASON_BACKEND);
            goto done;
        }
        if (zzapp_gl_present_pending(mw, 0) <= 0) {
            zzapp_gl_finish_target(outgoing, AAR_PRESENT_REASON_BACKEND);
            goto done;
        }
        started = minigl_platform_time_us();
        minigl_profile_push(MGLP_SWAP_FLIP);
        ok = zzapp_gl_wait_flip_idle();
        minigl_profile_pop();
        if (!ok)
            goto done;
        mw_ctx_diag_add_wait(mw, 0U,
                             (uint32_t)(minigl_platform_time_us() - started), 0U, 0U);
    }
    else if (g_sync_present) {
        int ok;
        uint64_t started = minigl_platform_time_us();

        minigl_profile_push(MGLP_SWAP_DRAIN);
        ok = minigl_raster_drain();
        minigl_profile_pop();
        mw_ctx_diag_add_present_wait(mw, outgoing, 0U, 0U,
                                     (uint32_t)(minigl_platform_time_us() - started));
        if (!ok) {
            zzapp_gl_finish_target(outgoing, AAR_PRESENT_REASON_BACKEND);
            goto done;
        }
        while (g_present_count != 0U) {
            if (zzapp_gl_present_pending(mw, 0) <= 0)
                goto done;
            minigl_profile_push(MGLP_SWAP_FLIP);
            ok = zzapp_gl_wait_flip_idle();
            minigl_profile_pop();
            if (!ok)
                goto done;
        }
    }
    minigl_profile_report();
done:
    g_mailbox_submit_id = 0U;
    zzapp_api_leave();
    return status;
}

void zzapp_gl_swap(void)
{
    (void)zzapp_gl_swap_frame(0U);
}

#if defined(MINIGL_MODULE)
int zzapp_gl_swap_tracked(uint64_t frame_id)
{
    const zzm_app_gl_host_service_t *host = g_module_app_host;

    if (frame_id == 0U)
        return AAR_E_INVAL;
    if (host->size < offsetof(zzm_app_gl_host_service_t, poll_present) +
                         sizeof(host->poll_present) ||
        !host->flip_bound_tracked || !host->finish_tracked || !host->poll_present)
        return AAR_E_NOSYS;
    return zzapp_gl_swap_frame(frame_id);
}
#endif
