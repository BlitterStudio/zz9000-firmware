/* Fixed-function MiniGL vertex lighting. */
#ifndef MINIGL_LIGHTING_H
#define MINIGL_LIGHTING_H

#include <stdint.h>
#include "minigl_renderer.h"

void minigl_lighting_reset(void);
int minigl_lighting_enabled(void);
int minigl_lighting_configure(const ZZ9KMiniGLPacketLighting *packet);
void minigl_lighting_modelview(const float modelview[16]);
void minigl_lighting_shade(const float eyePosition[4], const float objectNormal[3],
                           uint32_t incoming, uint32_t *front, uint32_t *back);
struct mw_state;
void minigl_lighting_configure_wire_v9(const struct mw_state *s);
void minigl_lighting_shade_wire_v9(const float eye_position[4], const float normal_eye[3],
                                   const float color[4], uint32_t *front, uint32_t *back);

#endif
