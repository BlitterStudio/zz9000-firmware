/*
 * Unit tests for Wire Protocol v9 Context State tracking
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "mw_types.h"
#include "mw_state.h"

static void test_state_defaults(void)
{
    mw_state s;
    mw_state_init(&s, 640, 480);

    assert(s.target.width == 640);
    assert(s.target.height == 480);
    assert(s.target.buffer_count == 2);
    assert(s.target.depth_bits == 24);

    assert(s.viewport_w == 640);
    assert(s.viewport_h == 480);
    assert(s.scissor_enabled == 0);

    assert(s.depth_test == 0);
    assert(s.depth_mask == 1);
    assert(s.depth_func == 0x201); /* GL_LESS */

    assert(s.blend_enabled == 0);
    assert(s.blend_src_rgb == 1);  /* GL_ONE */
    assert(s.blend_dst_rgb == 0);  /* GL_ZERO */

    assert(s.cull_enabled == 0);
    assert(s.front_face == 0x901); /* GL_CCW */
    assert(s.cull_face == 0x405);  /* GL_BACK */

    assert(s.shade_model == 0x1d01); /* GL_SMOOTH */

    /* Matrices should be identity */
    assert(s.modelview[0] == 1.0f && s.modelview[5] == 1.0f && s.modelview[10] == 1.0f && s.modelview[15] == 1.0f);
    assert(s.projection[0] == 1.0f && s.projection[5] == 1.0f && s.projection[10] == 1.0f && s.projection[15] == 1.0f);
    assert(s.mvp[0] == 1.0f && s.mvp[5] == 1.0f && s.mvp[10] == 1.0f && s.mvp[15] == 1.0f);

    printf("PASS: test_state_defaults\n");
}

static void test_matrix_mvp(void)
{
    mw_state s;
    mw_state_init(&s, 320, 240);

    /* Scale modelview by 2 along X */
    s.modelview[0] = 2.0f;
    /* Scale projection by 3 along X */
    s.projection[0] = 3.0f;

    mw_state_update_mvp(&s);

    /* Combined MVP should have 2 * 3 = 6 along X */
    assert(fabsf(s.mvp[0] - 6.0f) < 1e-5f);
    assert(fabsf(s.mvp[5] - 1.0f) < 1e-5f);

    printf("PASS: test_matrix_mvp\n");
}

static void test_state_updates(void)
{
    mw_state s;
    mw_state_init(&s, 800, 600);

    s.depth_test = 1;
    s.depth_func = 0x203; /* GL_LEQUAL */
    s.depth_mask = 0;
    assert(s.depth_test == 1 && s.depth_func == 0x203 && s.depth_mask == 0);

    s.blend_enabled = 1;
    s.blend_src_rgb = 0x302; /* GL_SRC_ALPHA */
    s.blend_dst_rgb = 0x303; /* GL_ONE_MINUS_SRC_ALPHA */
    assert(s.blend_enabled == 1 && s.blend_src_rgb == 0x302 && s.blend_dst_rgb == 0x303);

    s.alpha_test = 1;
    s.alpha_func = 0x204; /* GL_GREATER */
    s.alpha_ref = 0.5f;
    assert(s.alpha_test == 1 && s.alpha_func == 0x204 && s.alpha_ref == 0.5f);

    s.stages[0].enabled = 1;
    s.stages[0].name = 42;
    s.stages[0].environment = 0x1e01; /* GL_REPLACE */
    assert(s.stages[0].enabled == 1 && s.stages[0].name == 42 && s.stages[0].environment == 0x1e01);

    printf("PASS: test_state_updates\n");
}

static void test_normal_matrix(void)
{
    mw_state s;
    mw_state_init(&s, 320, 240);

    /* Uniform scale: inverse-transpose = 0.5 * I (GL needs normalize to restore) */
    s.modelview[0] = 2.0f; s.modelview[5] = 2.0f; s.modelview[10] = 2.0f;
    mw_state_update_normal_matrix(&s);
    assert(fabsf(s.normal_matrix[0] - 0.5f) < 1e-5f);
    assert(fabsf(s.normal_matrix[4] - 0.5f) < 1e-5f);
    assert(fabsf(s.normal_matrix[8] - 0.5f) < 1e-5f);

    /* Non-uniform scale (2,1,1): inverse transpose = (0.5,1,1) diagonal */
    mw_state_init(&s, 320, 240);
    s.modelview[0] = 2.0f;
    mw_state_update_normal_matrix(&s);
    assert(fabsf(s.normal_matrix[0] - 0.5f) < 1e-5f);
    assert(fabsf(s.normal_matrix[4] - 1.0f) < 1e-5f);
    assert(fabsf(s.normal_matrix[8] - 1.0f) < 1e-5f);

    /* Translation: rotation part unchanged -> identity normal matrix */
    mw_state_init(&s, 320, 240);
    s.modelview[12] = 3.0f; s.modelview[13] = -2.0f; s.modelview[14] = 5.0f;
    mw_state_update_normal_matrix(&s);
    assert(fabsf(s.normal_matrix[0] - 1.0f) < 1e-5f);
    assert(fabsf(s.normal_matrix[4] - 1.0f) < 1e-5f);
    assert(fabsf(s.normal_matrix[8] - 1.0f) < 1e-5f);

    printf("PASS: test_normal_matrix\n");
}

static void test_texgen_state(void)
{
    mw_state s;
    mw_state_init(&s, 320, 240);

    assert(!s.texgen[0].s_enabled && !s.texgen[0].t_enabled);
    assert(s.texgen[0].s_mode == 0x2400 && s.texgen[0].t_mode == 0x2400);

    s.texgen[1].s_enabled = 1;
    s.texgen[1].s_mode = 0x2401; /* GL_OBJECT_LINEAR */
    s.texgen[1].obj_plane_s[0] = 1.0f;
    s.texgen[1].obj_plane_s[1] = 0.5f;

    assert(s.texgen[1].s_enabled == 1);
    assert(s.texgen[1].s_mode == 0x2401);
    assert(fabsf(s.texgen[1].obj_plane_s[1] - 0.5f) < 1e-6f);

    printf("PASS: test_texgen_state\n");
}

static void test_fog_state(void)
{
    mw_state s;
    mw_state_init(&s, 320, 240);

    assert(s.fog_mode == 0x800); /* GL_EXP */
    assert(!s.fog_enabled);

    s.fog_mode = 0x2601; /* GL_LINEAR */
    s.fog_start = 2.0f;
    s.fog_end = 10.0f;
    s.fog_color[1] = 0.5f;

    assert(s.fog_mode == 0x2601);
    assert(fabsf(s.fog_start - 2.0f) < 1e-6f);
    assert(fabsf(s.fog_end - 10.0f) < 1e-6f);
    assert(fabsf(s.fog_color[1] - 0.5f) < 1e-6f);

    printf("PASS: test_fog_state\n");
}

int main(void)
{
    test_state_defaults();
    test_matrix_mvp();
    test_state_updates();
    test_normal_matrix();
    test_texgen_state();
    test_fog_state();
    printf("All state tests PASSED!\n");
    return 0;
}
