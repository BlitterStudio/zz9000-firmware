/*
 * Classic GL entry points. Each one calls the bound zzapp_gl slot and
 * does nothing (or returns 0) when the slot is past gl->size.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp_internal.h"

void glBegin(uint32_t mode)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Begin) && gl->Begin)
        gl->Begin(mode);
}

void glEnd(void)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, End) && gl->End)
        gl->End();
}

void glVertex2f(float x, float y)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Vertex2f) && gl->Vertex2f)
        gl->Vertex2f(x, y);
}

void glVertex3f(float x, float y, float z)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Vertex3f) && gl->Vertex3f)
        gl->Vertex3f(x, y, z);
}

void glVertex3fv(const float *v)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Vertex3fv) && gl->Vertex3fv)
        gl->Vertex3fv(v);
}

void glTexCoord2f(float s, float t)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, TexCoord2f) && gl->TexCoord2f)
        gl->TexCoord2f(s, t);
}

void glTexCoord2fv(const float *v)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, TexCoord2fv) && gl->TexCoord2fv)
        gl->TexCoord2fv(v);
}

void glColor3f(float r, float g, float b)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Color3f) && gl->Color3f)
        gl->Color3f(r, g, b);
}

void glColor3fv(const float *v)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Color3fv) && gl->Color3fv)
        gl->Color3fv(v);
}

void glColor3ubv(const uint8_t *v)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Color3ubv) && gl->Color3ubv)
        gl->Color3ubv(v);
}

void glColor4f(float r, float g, float b, float a)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Color4f) && gl->Color4f)
        gl->Color4f(r, g, b, a);
}

void glColor4fv(const float *v)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Color4fv) && gl->Color4fv)
        gl->Color4fv(v);
}

void glColor4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Color4ub) && gl->Color4ub)
        gl->Color4ub(r, g, b, a);
}

void glColor4ubv(const uint8_t *v)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Color4ubv) && gl->Color4ubv)
        gl->Color4ubv(v);
}

void glEnable(uint32_t cap)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Enable) && gl->Enable)
        gl->Enable(cap);
}

void glDisable(uint32_t cap)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Disable) && gl->Disable)
        gl->Disable(cap);
}

uint8_t glIsEnabled(uint32_t cap)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, IsEnabled) && gl->IsEnabled)
        return gl->IsEnabled(cap);
    return 0;
}

void glAlphaFunc(uint32_t func, float ref)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, AlphaFunc) && gl->AlphaFunc)
        gl->AlphaFunc(func, ref);
}

void glBlendFunc(uint32_t sfactor, uint32_t dfactor)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, BlendFunc) && gl->BlendFunc)
        gl->BlendFunc(sfactor, dfactor);
}

void glDepthFunc(uint32_t func)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, DepthFunc) && gl->DepthFunc)
        gl->DepthFunc(func);
}

void glDepthMask(uint8_t flag)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, DepthMask) && gl->DepthMask)
        gl->DepthMask(flag);
}

void glDepthRange(double n, double f)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, DepthRange) && gl->DepthRange)
        gl->DepthRange(n, f);
}

void glCullFace(uint32_t mode)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, CullFace) && gl->CullFace)
        gl->CullFace(mode);
}

void glFrontFace(uint32_t mode)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, FrontFace) && gl->FrontFace)
        gl->FrontFace(mode);
}

void glShadeModel(uint32_t mode)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, ShadeModel) && gl->ShadeModel)
        gl->ShadeModel(mode);
}

void glPolygonMode(uint32_t face, uint32_t mode)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, PolygonMode) && gl->PolygonMode)
        gl->PolygonMode(face, mode);
}

void glClear(uint32_t mask)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Clear) && gl->Clear)
        gl->Clear(mask);
}

void glClearColor(float r, float g, float b, float a)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, ClearColor) && gl->ClearColor)
        gl->ClearColor(r, g, b, a);
}

void glViewport(int32_t x, int32_t y, int32_t width, int32_t height)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Viewport) && gl->Viewport)
        gl->Viewport(x, y, width, height);
}

void glScissor(int32_t x, int32_t y, int32_t width, int32_t height)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Scissor) && gl->Scissor)
        gl->Scissor(x, y, width, height);
}

void glMatrixMode(uint32_t mode)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, MatrixMode) && gl->MatrixMode)
        gl->MatrixMode(mode);
}

void glLoadIdentity(void)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, LoadIdentity) && gl->LoadIdentity)
        gl->LoadIdentity();
}

void glLoadMatrixf(const float *m)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, LoadMatrixf) && gl->LoadMatrixf)
        gl->LoadMatrixf(m);
}

void glPushMatrix(void)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, PushMatrix) && gl->PushMatrix)
        gl->PushMatrix();
}

void glPopMatrix(void)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, PopMatrix) && gl->PopMatrix)
        gl->PopMatrix();
}

void glRotatef(float angle, float x, float y, float z)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Rotatef) && gl->Rotatef)
        gl->Rotatef(angle, x, y, z);
}

void glTranslatef(float x, float y, float z)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Translatef) && gl->Translatef)
        gl->Translatef(x, y, z);
}

void glScalef(float x, float y, float z)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Scalef) && gl->Scalef)
        gl->Scalef(x, y, z);
}

void glOrtho(double left, double right, double bottom, double top,
             double zNear, double zFar)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Ortho) && gl->Ortho)
        gl->Ortho(left, right, bottom, top, zNear, zFar);
}

void glFrustum(double left, double right, double bottom, double top,
               double zNear, double zFar)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Frustum) && gl->Frustum)
        gl->Frustum(left, right, bottom, top, zNear, zFar);
}

void glTexImage2D(uint32_t target, int32_t level, int32_t internalformat,
                  int32_t width, int32_t height, int32_t border,
                  uint32_t format, uint32_t type, const void *pixels)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, TexImage2D) && gl->TexImage2D)
        gl->TexImage2D(target, level, internalformat, width, height, border,
                       format, type, pixels);
}

void glTexSubImage2D(uint32_t target, int32_t level,
                     int32_t xoffset, int32_t yoffset,
                     int32_t width, int32_t height,
                     uint32_t format, uint32_t type, const void *pixels)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, TexSubImage2D) && gl->TexSubImage2D)
        gl->TexSubImage2D(target, level, xoffset, yoffset, width, height,
                          format, type, pixels);
}

void glTexParameteri(uint32_t target, uint32_t pname, int32_t param)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, TexParameteri) && gl->TexParameteri)
        gl->TexParameteri(target, pname, param);
}

void glTexParameterf(uint32_t target, uint32_t pname, float param)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, TexParameterf) && gl->TexParameterf)
        gl->TexParameterf(target, pname, param);
}

void glTexEnvi(uint32_t target, uint32_t pname, int32_t param)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, TexEnvi) && gl->TexEnvi)
        gl->TexEnvi(target, pname, param);
}

void glTexEnvf(uint32_t target, uint32_t pname, float param)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, TexEnvf) && gl->TexEnvf)
        gl->TexEnvf(target, pname, param);
}

void glGenTextures(int32_t n, uint32_t *textures)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, GenTextures) && gl->GenTextures)
        gl->GenTextures(n, textures);
}

void glDeleteTextures(int32_t n, const uint32_t *textures)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, DeleteTextures) && gl->DeleteTextures)
        gl->DeleteTextures(n, textures);
}

void glBindTexture(uint32_t target, uint32_t texture)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, BindTexture) && gl->BindTexture)
        gl->BindTexture(target, texture);
}

void glFinish(void)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Finish) && gl->Finish)
        gl->Finish();
}

void glFlush(void)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Flush) && gl->Flush)
        gl->Flush();
}

void glDrawBuffer(uint32_t mode)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, DrawBuffer) && gl->DrawBuffer)
        gl->DrawBuffer(mode);
}

void glReadBuffer(uint32_t mode)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, ReadBuffer) && gl->ReadBuffer)
        gl->ReadBuffer(mode);
}

void glReadPixels(int32_t x, int32_t y, int32_t width, int32_t height,
                  uint32_t format, uint32_t type, void *pixels)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, ReadPixels) && gl->ReadPixels)
        gl->ReadPixels(x, y, width, height, format, type, pixels);
}

const uint8_t *glGetString(uint32_t name)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, GetString) && gl->GetString)
        return gl->GetString(name);
    return 0;
}

void glGetIntegerv(uint32_t pname, int32_t *params)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, GetIntegerv) && gl->GetIntegerv)
        gl->GetIntegerv(pname, params);
}

void glGetFloatv(uint32_t pname, float *params)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, GetFloatv) && gl->GetFloatv)
        gl->GetFloatv(pname, params);
}

uint32_t glGetError(void)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, GetError) && gl->GetError)
        return gl->GetError();
    return 0;
}

void glHint(uint32_t target, uint32_t mode)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Hint) && gl->Hint)
        gl->Hint(target, mode);
}

void glPixelStorei(uint32_t pname, int32_t param)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, PixelStorei) && gl->PixelStorei)
        gl->PixelStorei(pname, param);
}

void gluPerspective(double fovy, double aspect, double zNear, double zFar)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Perspective) && gl->Perspective)
        gl->Perspective(fovy, aspect, zNear, zFar);
}

int32_t gluBuild2DMipmaps(uint32_t target, int32_t internalFormat,
                          int32_t width, int32_t height,
                          uint32_t format, uint32_t type, const void *data)
{
    const zzapp_gl *gl = zzapp_gl_bound();
    if (ZZ_GL_COVERS(gl, Build2DMipmaps) && gl->Build2DMipmaps)
        return gl->Build2DMipmaps(target, internalFormat, width, height,
                                  format, type, data);
    return 0;
}
