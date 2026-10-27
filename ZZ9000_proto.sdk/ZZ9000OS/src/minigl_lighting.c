/* Fixed-function MiniGL vertex lighting. */
#include "minigl_lighting.h"
#include "mw_state.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define GL_FRONT 0x0404U
#define GL_BACK 0x0405U
#define GL_FRONT_AND_BACK 0x0408U
#define GL_AMBIENT 0x1200U
#define GL_DIFFUSE 0x1201U
#define GL_SPECULAR 0x1202U
#define GL_EMISSION 0x1600U
#define GL_AMBIENT_AND_DIFFUSE 0x1602U

#define LIGHT_COUNT 8U
#define COLOR_SCALE (1.0f / 255.0f)

typedef struct Material {
    float ambient[4];
    float diffuse[4];
    float specular[4];
    float emission[4];
    float shininess;
} Material;

typedef struct Light {
    uint32_t enabled;
    float position[4];
    float ambient[4];
    float diffuse[4];
    float specular[4];
    float spot_direction[3];
    float spot_exponent;
    float spot_cutoff;
    float attenuation[3];
} Light;

typedef struct LightingState {
    uint32_t flags;
    uint16_t color_material_face;
    uint16_t color_material_mode;
    float global_ambient[4];
    Material material[2];
    Light light[LIGHT_COUNT];
    float modelview[16];
    float normal_matrix[9];
    float normal_rescale;
    uint8_t modelview_valid;
} LightingState;
typedef char LightingStatePacketPrefix[(offsetof(LightingState, modelview) == 960U) ? 1 : -1];

static LightingState g_lighting;

/* Light positions, directions and spot cones depend on state, not vertices. */
typedef struct LightCache {
    float position[3];
    float direction[3];
    float spot_direction[3];
    float spot_cosine;
    uint8_t direction_valid;
    uint8_t spot_valid;
    uint8_t specular_mask;
} LightCache;
static LightCache g_light_cache[LIGHT_COUNT];
static uint8_t g_light_specular_mask;

static float clamp01(float value)
{
    if (value <= 0.0f)
        return 0.0f;
    if (value >= 1.0f)
        return 1.0f;
    return value;
}

static uint8_t color_component(float value)
{
    return (uint8_t)(clamp01(value) * 255.0f + 0.5f);
}

static uint32_t rgba(float red, float green, float blue, float alpha)
{
    return ((uint32_t)color_component(red) << 24) |
           ((uint32_t)color_component(green) << 16) |
           ((uint32_t)color_component(blue) << 8) |
           color_component(alpha);
}


static float dot3(const float left[3], const float right[3])
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

static int normalize3(float value[3])
{
    float length_squared = dot3(value, value);
    float inverse_length;

    if (length_squared <= 0.0f)
        return 0;
    inverse_length = 1.0f / sqrtf(length_squared);
    value[0] *= inverse_length;
    value[1] *= inverse_length;
    value[2] *= inverse_length;
    return 1;
}

static uint8_t rgb_support(const float color[4])
{
    return (uint8_t)((color[0] != 0.0f) | ((color[1] != 0.0f) << 1) |
                     ((color[2] != 0.0f) << 2));
}

static void cache_lights(void)
{
    unsigned index;
    g_light_specular_mask = 0U;
    for (index = 0U; index < LIGHT_COUNT; ++index) {
        const Light *light = &g_lighting.light[index];
        LightCache *cache = &g_light_cache[index];
        memset(cache, 0, sizeof(*cache));
        if (!light->enabled) continue;
        cache->specular_mask = rgb_support(light->specular);
        g_light_specular_mask |= cache->specular_mask;
        if (fabsf(light->position[3]) < 1e-30f) {
            memcpy(cache->direction, light->position, sizeof(cache->direction));
            cache->direction_valid = (uint8_t)normalize3(cache->direction);
        } else {
            cache->position[0] = light->position[0] / light->position[3];
            cache->position[1] = light->position[1] / light->position[3];
            cache->position[2] = light->position[2] / light->position[3];
            if (light->spot_cutoff >= 0.0f && light->spot_cutoff <= 90.0f) {
                memcpy(cache->spot_direction, light->spot_direction,
                       sizeof(cache->spot_direction));
                cache->spot_valid = (uint8_t)normalize3(cache->spot_direction);
                if (cache->spot_valid)
                    cache->spot_cosine = cosf(light->spot_cutoff * 0.01745329251994329577f);
            }
        }
    }
}

static void identity_normal_matrix(void)
{
    memset(g_lighting.normal_matrix, 0, sizeof(g_lighting.normal_matrix));
    g_lighting.normal_matrix[0] = 1.0f;
    g_lighting.normal_matrix[4] = 1.0f;
    g_lighting.normal_matrix[8] = 1.0f;
    g_lighting.normal_rescale = 1.0f;
}

static void cache_normal_matrix(const float matrix[16])
{
    float a = matrix[0], b = matrix[4], c = matrix[8];
    float d = matrix[1], e = matrix[5], f = matrix[9];
    float g = matrix[2], h = matrix[6], i = matrix[10];
    float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    float inverse_determinant;
    float scale_squared;

    if (fabsf(determinant) < 1.0e-20f) {
        identity_normal_matrix();
        return;
    }

    inverse_determinant = 1.0f / determinant;
    g_lighting.normal_matrix[0] = (e * i - f * h) * inverse_determinant;
    g_lighting.normal_matrix[1] = (f * g - d * i) * inverse_determinant;
    g_lighting.normal_matrix[2] = (d * h - e * g) * inverse_determinant;
    g_lighting.normal_matrix[3] = (c * h - b * i) * inverse_determinant;
    g_lighting.normal_matrix[4] = (a * i - c * g) * inverse_determinant;
    g_lighting.normal_matrix[5] = (b * g - a * h) * inverse_determinant;
    g_lighting.normal_matrix[6] = (b * f - c * e) * inverse_determinant;
    g_lighting.normal_matrix[7] = (c * d - a * f) * inverse_determinant;
    g_lighting.normal_matrix[8] = (a * e - b * d) * inverse_determinant;

    scale_squared = a * a + d * d + g * g;
    g_lighting.normal_rescale = scale_squared > 0.0f ? sqrtf(scale_squared) : 1.0f;
}

void minigl_lighting_reset(void)
{
    static const float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    memset(&g_lighting, 0, sizeof(g_lighting));
    memcpy(g_lighting.modelview, identity, sizeof(identity));
    identity_normal_matrix();
    g_lighting.modelview_valid = 1U;
    /* OpenGL light defaults: constant attenuation 1.0 (no falloff) and an
     * omnidirectional 180-degree spot with a +Z direction. Zero attenuation
     * would make a point light's denominator 0, skipping the light. */
    for (unsigned i = 0U; i < LIGHT_COUNT; ++i) {
        g_lighting.light[i].attenuation[0] = 1.0f;
        g_lighting.light[i].spot_cutoff = 180.0f;
        g_lighting.light[i].spot_direction[2] = 1.0f;
    }
    cache_lights();
}

int minigl_lighting_enabled(void)
{
    return (g_lighting.flags & ZZ9K_MGL_LIGHTING_ENABLED) != 0U;
}

int minigl_lighting_configure(const ZZ9KMiniGLPacketLighting *packet)
{
    if (!packet)
        return 0;

    memcpy(&g_lighting.flags, &packet->flags, sizeof(*packet));
    cache_lights();
    return 1;
}

void minigl_lighting_modelview(const float modelview[16])
{
    if (!modelview)
        return;
    if (g_lighting.modelview_valid &&
        memcmp(g_lighting.modelview, modelview, sizeof(g_lighting.modelview)) == 0)
        return;

    memcpy(g_lighting.modelview, modelview, sizeof(g_lighting.modelview));
    cache_normal_matrix(g_lighting.modelview);
    g_lighting.modelview_valid = 1U;
}

static void transformed_normal(const float object_normal[3], float normal[3])
{
    const float *matrix = g_lighting.normal_matrix;

    normal[0] = matrix[0] * object_normal[0] + matrix[1] * object_normal[1] +
                matrix[2] * object_normal[2];
    normal[1] = matrix[3] * object_normal[0] + matrix[4] * object_normal[1] +
                matrix[5] * object_normal[2];
    normal[2] = matrix[6] * object_normal[0] + matrix[7] * object_normal[1] +
                matrix[8] * object_normal[2];
    if (g_lighting.flags & ZZ9K_MGL_LIGHTING_NORMALIZE)
        (void)normalize3(normal);
    else if (g_lighting.flags & ZZ9K_MGL_LIGHTING_RESCALE_NORMAL) {
        normal[0] *= g_lighting.normal_rescale;
        normal[1] *= g_lighting.normal_rescale;
        normal[2] *= g_lighting.normal_rescale;
    }
}

static int color_material_applies(unsigned side)
{
    uint16_t face = g_lighting.color_material_face;

    return face == GL_FRONT_AND_BACK ||
           (side == 0U && face == GL_FRONT) ||
           (side == 1U && face == GL_BACK);
}

static void apply_color_material(Material *material, unsigned side, const float color[4])
{
    if (!(g_lighting.flags & ZZ9K_MGL_LIGHTING_COLOR_MATERIAL) ||
        !color_material_applies(side))
        return;

    switch (g_lighting.color_material_mode) {
    case GL_AMBIENT:
        memcpy(material->ambient, color, sizeof(material->ambient));
        break;
    case GL_DIFFUSE:
        memcpy(material->diffuse, color, sizeof(material->diffuse));
        break;
    case GL_AMBIENT_AND_DIFFUSE:
        memcpy(material->ambient, color, sizeof(material->ambient));
        memcpy(material->diffuse, color, sizeof(material->diffuse));
        break;
    case GL_SPECULAR:
        memcpy(material->specular, color, sizeof(material->specular));
        break;
    case GL_EMISSION:
        memcpy(material->emission, color, sizeof(material->emission));
        break;
    default:
        break;
    }
}

static float spotlight_factor(const Light *light, const LightCache *cache,
                              const float direction_to_light[3])
{
    float cosine;
    if (light->spot_cutoff >= 180.0f) return 1.0f;
    if (!cache->spot_valid) return 0.0f;
    cosine = -(direction_to_light[0] * cache->spot_direction[0] +
               direction_to_light[1] * cache->spot_direction[1] +
               direction_to_light[2] * cache->spot_direction[2]);
    if (cosine < cache->spot_cosine) return 0.0f;
    return light->spot_exponent > 0.0f ? powf(clamp01(cosine), light->spot_exponent) : 1.0f;
}

static void shade_face(const float eye_position[3], const float normal[3], const Material *base,
                       unsigned side, const float color[4], float result[4])
{
    Material material = *base;
    float view[3] = { 0.0f, 0.0f, 1.0f };
    uint8_t specular_mask;
    unsigned index;

    apply_color_material(&material, side, color);
    result[0] = material.emission[0] + material.ambient[0] * g_lighting.global_ambient[0];
    result[1] = material.emission[1] + material.ambient[1] * g_lighting.global_ambient[1];
    result[2] = material.emission[2] + material.ambient[2] * g_lighting.global_ambient[2];
    result[3] = material.diffuse[3];

    specular_mask = rgb_support(material.specular);
    if ((specular_mask & g_light_specular_mask) &&
        (g_lighting.flags & ZZ9K_MGL_LIGHTING_LOCAL_VIEWER)) {
        view[0] = -eye_position[0];
        view[1] = -eye_position[1];
        view[2] = -eye_position[2];
        if (!normalize3(view)) {
            view[0] = 0.0f;
            view[1] = 0.0f;
            view[2] = 1.0f;
        }
    }

    for (index = 0; index < LIGHT_COUNT; ++index) {
        const Light *light = &g_lighting.light[index];
        const LightCache *cache = &g_light_cache[index];
        float direction[3];
        float attenuation = 1.0f;
        float diffuse;

        if (!light->enabled)
            continue;
        if (fabsf(light->position[3]) >= 1e-30f) {
            float distance;
            float denominator;

            direction[0] = cache->position[0] - eye_position[0];
            direction[1] = cache->position[1] - eye_position[1];
            direction[2] = cache->position[2] - eye_position[2];
            distance = sqrtf(dot3(direction, direction));
            if (distance <= 0.0f)
                continue;
            direction[0] /= distance;
            direction[1] /= distance;
            direction[2] /= distance;
            denominator = light->attenuation[0] + light->attenuation[1] * distance +
                          light->attenuation[2] * distance * distance;
            attenuation = denominator > 0.0f ? 1.0f / denominator : 0.0f;
            attenuation *= spotlight_factor(light, cache, direction);
        } else {
            if (!cache->direction_valid) continue;
            memcpy(direction, cache->direction, sizeof(direction));
        }
        if (attenuation <= 0.0f)
            continue;

        result[0] += attenuation * light->ambient[0] * material.ambient[0];
        result[1] += attenuation * light->ambient[1] * material.ambient[1];
        result[2] += attenuation * light->ambient[2] * material.ambient[2];

        diffuse = dot3(normal, direction);
        if (diffuse > 0.0f) {
            float specular = 0.0f;

            result[0] += attenuation * diffuse * light->diffuse[0] * material.diffuse[0];
            result[1] += attenuation * diffuse * light->diffuse[1] * material.diffuse[1];
            result[2] += attenuation * diffuse * light->diffuse[2] * material.diffuse[2];
            /* Disjoint RGB specular channels cannot contribute. Color material
             * has already been applied before computing the material mask. */
            if (specular_mask & cache->specular_mask) {
                float half_vector[3] = {
                    direction[0] + view[0], direction[1] + view[1], direction[2] + view[2]
                };
                if (normalize3(half_vector)) {
                    specular = dot3(normal, half_vector);
                    if (specular > 0.0f)
                        specular = powf(specular, material.shininess);
                    else
                        specular = 0.0f;
                }
                result[0] += attenuation * specular * light->specular[0] * material.specular[0];
                result[1] += attenuation * specular * light->specular[1] * material.specular[1];
                result[2] += attenuation * specular * light->specular[2] * material.specular[2];
            }
        }
    }
}

void minigl_lighting_shade(const float eye_position[4], const float object_normal[3],
                           uint32_t incoming, uint32_t *front, uint32_t *back)
{
    float color[4] = {
        (float)((incoming >> 24) & 0xffU) * COLOR_SCALE,
        (float)((incoming >> 16) & 0xffU) * COLOR_SCALE,
        (float)((incoming >> 8) & 0xffU) * COLOR_SCALE,
        (float)(incoming & 0xffU) * COLOR_SCALE
    };
    float normal[3];
    float cartesian_position[3];
    const float *position = eye_position;
    float front_result[4];
    float back_result[4];

    if (!(g_lighting.flags & ZZ9K_MGL_LIGHTING_ENABLED) || !eye_position || !object_normal) {
        if (front)
            *front = incoming;
        if (back)
            *back = incoming;
        return;
    }

    if (eye_position[3] != 0.0f && eye_position[3] != 1.0f) {
        cartesian_position[0] = eye_position[0] / eye_position[3];
        cartesian_position[1] = eye_position[1] / eye_position[3];
        cartesian_position[2] = eye_position[2] / eye_position[3];
        position = cartesian_position;
    }
    transformed_normal(object_normal, normal);
    shade_face(position, normal, &g_lighting.material[0], 0U, color, front_result);
    if (g_lighting.flags & ZZ9K_MGL_LIGHTING_TWO_SIDE) {
        normal[0] = -normal[0];
        normal[1] = -normal[1];
        normal[2] = -normal[2];
        shade_face(position, normal, &g_lighting.material[1], 1U, color, back_result);
    } else {
        memcpy(back_result, front_result, sizeof(back_result));
    }

    if (front)
        *front = rgba(front_result[0], front_result[1], front_result[2], front_result[3]);
    if (back)
        *back = rgba(back_result[0], back_result[1], back_result[2], back_result[3]);
}

static void lit_color_eval(const LightingState *s, const float eye[4], const float normal[3],
                           const float vtx_color[4], int back, float result[4])
{
    float m_ambient[4], m_diffuse[4], m_specular[4], m_emission[4];
    float shininess;
    const Material *mat = back ? &s->material[1] : &s->material[0];
    unsigned i;

    memcpy(m_ambient, mat->ambient, sizeof(m_ambient));
    memcpy(m_diffuse, mat->diffuse, sizeof(m_diffuse));
    memcpy(m_specular, mat->specular, sizeof(m_specular));
    memcpy(m_emission, mat->emission, sizeof(m_emission));
    shininess = mat->shininess;

    if (s->flags & ZZ9K_MGL_LIGHTING_COLOR_MATERIAL) {
        int tracked = back ? (s->color_material_face == GL_BACK || s->color_material_face == GL_FRONT_AND_BACK)
                           : (s->color_material_face == GL_FRONT || s->color_material_face == GL_FRONT_AND_BACK);
        if (tracked) {
            uint16_t mode = s->color_material_mode;
            if (mode == GL_AMBIENT) {
                memcpy(m_ambient, vtx_color, 4 * sizeof(float));
            } else if (mode == GL_DIFFUSE) {
                memcpy(m_diffuse, vtx_color, 4 * sizeof(float));
            } else if (mode == GL_SPECULAR) {
                memcpy(m_specular, vtx_color, 4 * sizeof(float));
            } else if (mode == GL_EMISSION) {
                memcpy(m_emission, vtx_color, 4 * sizeof(float));
            } else { /* GL_AMBIENT_AND_DIFFUSE */
                memcpy(m_ambient, vtx_color, 4 * sizeof(float));
                memcpy(m_diffuse, vtx_color, 4 * sizeof(float));
            }
        }
    }

    float color[3];
    color[0] = m_emission[0] + s->global_ambient[0] * m_ambient[0];
    color[1] = m_emission[1] + s->global_ambient[1] * m_ambient[1];
    color[2] = m_emission[2] + s->global_ambient[2] * m_ambient[2];

    float viewer[3];
    if (s->flags & ZZ9K_MGL_LIGHTING_LOCAL_VIEWER) {
        float elen = sqrtf(eye[0]*eye[0] + eye[1]*eye[1] + eye[2]*eye[2]);
        if (elen > 1e-20f) {
            viewer[0] = -eye[0] / elen;
            viewer[1] = -eye[1] / elen;
            viewer[2] = -eye[2] / elen;
        } else {
            viewer[0] = 0.0f; viewer[1] = 0.0f; viewer[2] = 1.0f;
        }
    } else {
        viewer[0] = 0.0f; viewer[1] = 0.0f; viewer[2] = 1.0f;
    }

    for (i = 0; i < LIGHT_COUNT; ++i) {
        const Light *light = &s->light[i];
        if (!light->enabled) continue;

        float to_light[3];
        if (fabsf(light->position[3]) < 1e-30f) {
            to_light[0] = light->position[0];
            to_light[1] = light->position[1];
            to_light[2] = light->position[2];
        } else {
            to_light[0] = light->position[0] - eye[0];
            to_light[1] = light->position[1] - eye[1];
            to_light[2] = light->position[2] - eye[2];
        }

        float dist = sqrtf(to_light[0]*to_light[0] + to_light[1]*to_light[1] + to_light[2]*to_light[2]);
        float l[3];
        if (dist > 1e-20f) {
            float inv_dist = 1.0f / dist;
            l[0] = to_light[0] * inv_dist;
            l[1] = to_light[1] * inv_dist;
            l[2] = to_light[2] * inv_dist;
        } else {
            l[0] = 0.0f; l[1] = 0.0f; l[2] = 1.0f;
        }

        float scale = 1.0f;
        if (fabsf(light->position[3]) >= 1e-30f) {
            float att = light->attenuation[0] + dist * light->attenuation[1] + dist * dist * light->attenuation[2];
            if (att < 1e-20f) att = 1e-20f;
            scale = 1.0f / att;

            if (light->spot_cutoff < 180.0f) {
                float sd[3] = { light->spot_direction[0], light->spot_direction[1], light->spot_direction[2] };
                float sd_len = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
                if (sd_len > 1e-20f) {
                    sd[0] /= sd_len; sd[1] /= sd_len; sd[2] /= sd_len;
                }
                float spot_cos_cutoff = cosf(light->spot_cutoff * 3.14159265358979323846f / 180.0f);
                float cone = -(l[0]*sd[0] + l[1]*sd[1] + l[2]*sd[2]);
                if (cone < spot_cos_cutoff) continue;
                if (light->spot_exponent > 0.0f) {
                    scale *= powf(cone, light->spot_exponent);
                }
            }
        }

        float diffuse = normal[0]*l[0] + normal[1]*l[1] + normal[2]*l[2];
        if (diffuse < 0.0f) diffuse = 0.0f;

        color[0] += scale * (light->ambient[0] * m_ambient[0] + diffuse * light->diffuse[0] * m_diffuse[0]);
        color[1] += scale * (light->ambient[1] * m_ambient[1] + diffuse * light->diffuse[1] * m_diffuse[1]);
        color[2] += scale * (light->ambient[2] * m_ambient[2] + diffuse * light->diffuse[2] * m_diffuse[2]);

        if (diffuse > 0.0f) {
            float h[3] = { l[0] + viewer[0], l[1] + viewer[1], l[2] + viewer[2] };
            float h_len = sqrtf(h[0]*h[0] + h[1]*h[1] + h[2]*h[2]);
            if (h_len > 1e-20f) {
                h[0] /= h_len; h[1] /= h_len; h[2] /= h_len;
            }
            float shine = normal[0]*h[0] + normal[1]*h[1] + normal[2]*h[2];
            if (shine < 0.0f) shine = 0.0f;
            float spec_factor = (shininess <= 0.0f) ? 1.0f : powf(shine, shininess);
            color[0] += scale * spec_factor * light->specular[0] * m_specular[0];
            color[1] += scale * spec_factor * light->specular[1] * m_specular[1];
            color[2] += scale * spec_factor * light->specular[2] * m_specular[2];
        }
    }

    result[0] = clamp01(color[0]);
    result[1] = clamp01(color[1]);
    result[2] = clamp01(color[2]);
    result[3] = clamp01(m_diffuse[3]);
}

void minigl_lighting_configure_wire_v9(const struct mw_state *s)
{
    if (!s) return;
    uint32_t flags = 0;
    if (s->lighting_enabled) flags |= ZZ9K_MGL_LIGHTING_ENABLED;
    if (s->normalize_enabled) flags |= ZZ9K_MGL_LIGHTING_NORMALIZE;
    if (s->color_material_enabled) flags |= ZZ9K_MGL_LIGHTING_COLOR_MATERIAL;
    if (s->light_model_two_side) flags |= ZZ9K_MGL_LIGHTING_TWO_SIDE;
    if (s->light_model_local_viewer) flags |= ZZ9K_MGL_LIGHTING_LOCAL_VIEWER;

    g_lighting.flags = flags;
    g_lighting.color_material_face = (uint16_t)s->color_material_face;
    g_lighting.color_material_mode = (uint16_t)s->color_material_mode;
    memcpy(g_lighting.global_ambient, s->light_model_ambient, sizeof(g_lighting.global_ambient));

    for (unsigned face = 0; face < 2; ++face) {
        const mw_material_param *m = &s->materials[face];
        Material *dm = &g_lighting.material[face];
        memcpy(dm->ambient, m->ambient, sizeof(dm->ambient));
        memcpy(dm->diffuse, m->diffuse, sizeof(dm->diffuse));
        memcpy(dm->specular, m->specular, sizeof(dm->specular));
        memcpy(dm->emission, m->emission, sizeof(dm->emission));
        dm->shininess = m->shininess;
    }

    for (unsigned i = 0; i < LIGHT_COUNT; ++i) {
        const mw_light_param *l = &s->lights[i];
        Light *dl = &g_lighting.light[i];
        dl->enabled = l->enabled;
        memcpy(dl->position, l->position, sizeof(dl->position));
        memcpy(dl->ambient, l->ambient, sizeof(dl->ambient));
        memcpy(dl->diffuse, l->diffuse, sizeof(dl->diffuse));
        memcpy(dl->specular, l->specular, sizeof(dl->specular));
        memcpy(dl->spot_direction, l->spot_direction, sizeof(dl->spot_direction));
        dl->spot_exponent = l->spot_exponent;
        dl->spot_cutoff = l->spot_cutoff;
        dl->attenuation[0] = l->constant;
        dl->attenuation[1] = l->linear;
        dl->attenuation[2] = l->quadratic;
    }

    cache_lights();
    minigl_lighting_modelview(s->modelview);
}

void minigl_lighting_shade_wire_v9(const float eye_position[4], const float normal_eye[3],
                                   const float color[4], uint32_t *front, uint32_t *back)
{
    float normal[3] = { normal_eye[0], normal_eye[1], normal_eye[2] };
    if (g_lighting.flags & ZZ9K_MGL_LIGHTING_NORMALIZE) {
        normalize3(normal);
    }
    float front_res[4], back_res[4];
    lit_color_eval(&g_lighting, eye_position, normal, color, 0, front_res);
    if (g_lighting.flags & ZZ9K_MGL_LIGHTING_TWO_SIDE) {
        float back_normal[3] = { -normal[0], -normal[1], -normal[2] };
        lit_color_eval(&g_lighting, eye_position, back_normal, color, 1, back_res);
    } else {
        memcpy(back_res, front_res, sizeof(back_res));
    }
    if (front) *front = rgba(front_res[0], front_res[1], front_res[2], front_res[3]);
    if (back)  *back  = rgba(back_res[0], back_res[1], back_res[2], back_res[3]);
}
