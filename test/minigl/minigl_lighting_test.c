/* Host-executable fixed-function MiniGL vertex-lighting tests. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../ZZ9000_proto.sdk/ZZ9000OS/src/minigl_lighting.h"

#define GL_FRONT 0x0404U
#define GL_AMBIENT_AND_DIFFUSE 0x1602U

static const float identity[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f
};
static const float origin[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

static uint8_t red(uint32_t color)
{
    return (uint8_t)(color >> 24);
}

static void packet_defaults(ZZ9KMiniGLPacketLighting *packet)
{
    unsigned index;

    memset(packet, 0, sizeof(*packet));
    packet->flags = ZZ9K_MGL_LIGHTING_ENABLED | ZZ9K_MGL_LIGHTING_NORMALIZE;
    packet->material[0].diffuse[3] = 1.0f;
    packet->material[1].diffuse[3] = 1.0f;
    for (index = 0; index < 8U; ++index)
        packet->light[index].attenuation[0] = 1.0f;
}

static void diffuse_light(ZZ9KMiniGLPacketLighting *packet, unsigned index,
                          float x, float y, float z, float intensity)
{
    packet->light[index].enabled = 1U;
    packet->light[index].position[0] = x;
    packet->light[index].position[1] = y;
    packet->light[index].position[2] = z;
    packet->light[index].diffuse[0] = intensity;
    packet->light[index].diffuse[1] = intensity;
    packet->light[index].diffuse[2] = intensity;
}

static void test_disabled_preserves_raster_color(void)
{
    uint32_t front = 0U, back = 0U;

    minigl_lighting_reset();
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, 1.0f },
                          0x12345678U, &front, &back);
    assert(front == 0x12345678U);
    assert(back == 0x12345678U);
}

static void test_directional_facing_and_away(void)
{
    ZZ9KMiniGLPacketLighting packet;
    uint32_t front;

    packet_defaults(&packet);
    packet.material[0].diffuse[0] = 1.0f;
    packet.material[0].diffuse[1] = 1.0f;
    packet.material[0].diffuse[2] = 1.0f;
    diffuse_light(&packet, 0U, 0.0f, 0.0f, 1.0f, 1.0f);
    minigl_lighting_reset();
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_modelview(identity);
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, 1.0f },
                          0U, &front, NULL);
    assert(front == 0xffffffffU);
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, -1.0f },
                          0U, &front, NULL);
    assert(front == 0x000000ffU);
}

static void test_inverse_transpose_and_rescale(void)
{
    ZZ9KMiniGLPacketLighting packet;
    const float nonuniform[16] = {
        2.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    const float uniform[16] = {
        2.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 2.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    uint32_t front;

    packet_defaults(&packet);
    packet.material[0].diffuse[0] = 1.0f;
    diffuse_light(&packet, 0U, 1.0f, 0.0f, 0.0f, 1.0f);
    minigl_lighting_reset();
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_modelview(nonuniform);
    minigl_lighting_shade(origin, (const float[]){ 1.0f, 1.0f, 0.0f },
                          0U, &front, NULL);
    assert(red(front) >= 113U && red(front) <= 115U);

    packet.flags = ZZ9K_MGL_LIGHTING_ENABLED | ZZ9K_MGL_LIGHTING_RESCALE_NORMAL;
    diffuse_light(&packet, 0U, 0.0f, 0.0f, 1.0f, 1.0f);
    packet.material[0].diffuse[1] = 1.0f;
    packet.material[0].diffuse[2] = 1.0f;
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_modelview(uniform);
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, 1.0f },
                          0U, &front, NULL);
    assert(front == 0xffffffffU);
}

static void test_all_eight_lights_accumulate(void)
{
    ZZ9KMiniGLPacketLighting packet;
    uint32_t front;
    unsigned index;

    packet_defaults(&packet);
    packet.material[0].diffuse[0] = 1.0f;
    packet.material[0].diffuse[1] = 1.0f;
    packet.material[0].diffuse[2] = 1.0f;
    for (index = 0; index < 8U; ++index)
        diffuse_light(&packet, index, 0.0f, 0.0f, 1.0f, 0.125f);
    minigl_lighting_reset();
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, 1.0f },
                          0U, &front, NULL);
    assert(front == 0xffffffffU);
}

static void test_spotlight_attenuation_and_shininess(void)
{
    ZZ9KMiniGLPacketLighting packet;
    uint32_t front;

    packet_defaults(&packet);
    packet.material[0].diffuse[0] = 1.0f;
    packet.material[0].diffuse[1] = 1.0f;
    packet.material[0].diffuse[2] = 1.0f;
    diffuse_light(&packet, 0U, 0.0f, 0.0f, 4.0f, 1.0f);
    packet.light[0].position[3] = 1.0f;
    packet.light[0].attenuation[0] = 2.0f;
    packet.light[0].spot_direction[2] = -1.0f;
    packet.light[0].spot_cutoff = 30.0f;
    minigl_lighting_reset();
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, 1.0f },
                          0U, &front, NULL);
    assert(red(front) >= 127U && red(front) <= 128U);

    packet.light[0].spot_direction[2] = 1.0f;
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, 1.0f },
                          0U, &front, NULL);
    assert(front == 0x000000ffU);

    packet_defaults(&packet);
    packet.material[0].specular[0] = 1.0f;
    packet.material[0].shininess = 4.0f;
    diffuse_light(&packet, 0U, 0.0f, 0.6f, 0.8f, 0.0f);
    packet.light[0].specular[0] = 1.0f;
    minigl_lighting_reset();
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, 1.0f },
                          0U, &front, NULL);
    assert(red(front) >= 207U && red(front) <= 209U);
}

static void test_two_sided_and_color_material(void)
{
    ZZ9KMiniGLPacketLighting packet;
    uint32_t front, back;

    packet_defaults(&packet);
    packet.flags |= ZZ9K_MGL_LIGHTING_TWO_SIDE;
    packet.material[0].diffuse[0] = 1.0f;
    packet.material[1].diffuse[1] = 1.0f;
    diffuse_light(&packet, 0U, 0.0f, 0.0f, -1.0f, 1.0f);
    minigl_lighting_reset();
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, 1.0f },
                          0U, &front, &back);
    assert(front == 0x000000ffU);
    assert(back == 0x00ff00ffU);

    packet_defaults(&packet);
    packet.flags |= ZZ9K_MGL_LIGHTING_COLOR_MATERIAL;
    packet.color_material_face = GL_FRONT;
    packet.color_material_mode = GL_AMBIENT_AND_DIFFUSE;
    diffuse_light(&packet, 0U, 0.0f, 0.0f, 1.0f, 1.0f);
    minigl_lighting_reset();
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, 1.0f },
                          0x2040607fU, &front, NULL);
    assert(front == 0x2040607fU);
}

static void test_global_ambient_emission_and_alpha(void)
{
    ZZ9KMiniGLPacketLighting packet;
    uint32_t front;

    packet_defaults(&packet);
    packet.global_ambient[0] = 0.5f;
    packet.material[0].ambient[0] = 0.5f;
    packet.material[0].emission[1] = 0.25f;
    packet.material[0].diffuse[3] = 0.25f;
    minigl_lighting_reset();
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, 1.0f },
                          0U, &front, NULL);
    assert(front == 0x40400040U);
}

static void test_local_viewer_specular(void)
{
    ZZ9KMiniGLPacketLighting packet;
    uint32_t front;

    packet_defaults(&packet);
    packet.flags |= ZZ9K_MGL_LIGHTING_LOCAL_VIEWER;
    packet.material[0].specular[0] = 1.0f;
    packet.material[0].shininess = 16.0f;
    packet.light[0].enabled = 1U;
    packet.light[0].position[2] = 1.0f;
    packet.light[0].specular[0] = 1.0f;
    minigl_lighting_reset();
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_shade((const float[]){ 1.0f, 0.0f, -1.0f, 1.0f },
                          (const float[]){ 0.0f, 0.0f, 1.0f }, 0U, &front, NULL);
    assert(red(front) >= 71U && red(front) <= 73U);
}
static void test_homogeneous_point_positions(void)
{
    ZZ9KMiniGLPacketLighting packet;
    uint32_t front;

    packet_defaults(&packet);
    packet.material[0].diffuse[0] = 1.0f;
    diffuse_light(&packet, 0U, 0.0f, 0.0f, 2.0f, 1.0f);
    packet.light[0].position[3] = 2.0f;
    packet.light[0].spot_cutoff = 180.0f;
    packet.light[0].attenuation[1] = 1.0f;
    minigl_lighting_reset();
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, 1.0f },
                          0U, &front, NULL);
    assert(front == 0x800000ffU); /* Cartesian light z=1; attenuation=1/2. */

    packet.light[0].position[2] = 1.0f;
    packet.light[0].position[3] = 1.0f;
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_shade((const float[]){ 0.0f, 0.0f, 0.5f, 2.0f },
                          (const float[]){ 0.0f, 0.0f, 1.0f }, 0U, &front, NULL);
    assert(front == 0x920000ffU); /* Cartesian vertex z=1/4; attenuation=4/7. */
}

static void test_specular_channel_support_and_color_material(void)
{
    ZZ9KMiniGLPacketLighting packet;
    uint32_t front;

    packet_defaults(&packet);
    packet.flags |= ZZ9K_MGL_LIGHTING_LOCAL_VIEWER;
    packet.material[0].specular[0] = 1.0f;
    packet.material[0].shininess = 32.0f;
    packet.light[0].enabled = 1U;
    packet.light[0].position[2] = 1.0f;
    packet.light[0].specular[1] = 1.0f;
    minigl_lighting_reset();
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, 1.0f },
                          0U, &front, NULL);
    assert(front == 0x000000ffU); /* Red material and green light do not overlap. */

    packet.light[0].specular[1] = 0.0f;
    packet.light[0].specular[0] = 1.0f;
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, 1.0f },
                          0U, &front, NULL);
    assert(front == 0xff0000ffU); /* A light-state change restores red specular. */

    packet.light[0].specular[0] = 0.0f;
    packet.light[0].specular[1] = 1.0f;
    packet.flags |= ZZ9K_MGL_LIGHTING_COLOR_MATERIAL;
    packet.color_material_face = GL_FRONT;
    packet.color_material_mode = 0x1202U; /* GL_SPECULAR */
    assert(minigl_lighting_configure(&packet));
    minigl_lighting_shade(origin, (const float[]){ 0.0f, 0.0f, 1.0f },
                          0x00ff00ffU, &front, NULL);
    assert(front == 0x00ff00ffU); /* Per-vertex color overrides the red material. */
}

int main(void)
{
    assert(!minigl_lighting_configure(NULL));
    minigl_lighting_reset();
    assert(!minigl_lighting_enabled());
    test_disabled_preserves_raster_color();
    test_directional_facing_and_away();
    test_inverse_transpose_and_rescale();
    test_all_eight_lights_accumulate();
    test_spotlight_attenuation_and_shininess();
    test_two_sided_and_color_material();
    test_global_ambient_emission_and_alpha();
    test_local_viewer_specular();
    test_homogeneous_point_positions();
    test_specular_channel_support_and_color_material();
    puts("minigl lighting tests passed");
    return 0;
}
