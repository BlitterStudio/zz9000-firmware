/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "minigl_present.h"

int main(void)
{
    const uint32_t src[6] = {0x11223344,0x55667788,0x99aabbcc,
                             0xff000080,0x00ff0040,0x0000ff20};
    uint8_t vram[128], unchanged[128];
    ZZ9KMiniGLPacketPresent p = {0};
    const uint8_t expected[8][4] = {
        {0x11,0x22,0x33,0x44},{0x33,0x22,0x11,0x44},
        {0x44,0x11,0x22,0x33},{0x11,0x06,0,0},
        {0x06,0x11,0,0},{0x11,0x22,0x33,0},
        {0x33,0x22,0x11,0},{0x44,0x33,0x22,0x11}
    };
    unsigned format;
    p.target_width=3; p.target_height=2;
    p.dest_offset=0x10008; p.dest_pitch=24;
    p.window_x=1; p.window_y=1; p.window_w=2; p.window_h=2;
    for (format=0; format<8; ++format) {
        unsigned bytes = (format==3||format==4)?2:(format==5||format==6)?3:4;
        memset(vram,0xa5,sizeof(vram));
        p.dest_format=format;
        assert(minigl_present_copy(&p,src,vram,sizeof(vram)));
        assert(memcmp(vram+8+24+bytes,expected[format],bytes)==0);
        assert(vram[8+24+bytes-1]==0xa5);
        assert(vram[8+24+3*bytes]==0xa5);
        assert(vram[8+48+bytes-1]==0xa5);
    }
    /* An obscured window copies a source subrectangle, not a rescaled frame. */
    memset(vram,0xa5,sizeof(vram));
    p.dest_format=ZZ9K_MGL_PIXFMT_RGBA32;
    p.source_x=1; p.source_y=1; p.window_w=2; p.window_h=1;
    assert(minigl_present_copy(&p,src,vram,sizeof(vram)));
    assert(vram[36]==0 && vram[37]==255 && vram[38]==0 && vram[39]==0x40);
    assert(vram[40]==0 && vram[41]==0 && vram[42]==255 && vram[43]==0x20);
    assert(vram[60]==0xa5);
    {
        const uint16_t rgb565[6] = {0xf800U, 0x07e0U, 0x001fU,
                                    0xffffU, 0x0000U, 0x7befU};
        p.source_x=0; p.source_y=0; p.window_x=0; p.window_y=0;
        p.window_w=3; p.window_h=2; p.dest_pitch=24;
        memset(vram, 0xa5, sizeof(vram));
        p.dest_format=ZZ9K_MGL_PIXFMT_RGB565_LE;
        assert(minigl_present_copy_rgb565(&p,rgb565,vram,sizeof(vram)));
        assert(vram[8] == 0 && vram[9] == 0xf8);
        assert(vram[10] == 0xe0 && vram[11] == 7);
        p.dest_format=ZZ9K_MGL_PIXFMT_RGBA32;
        memset(vram, 0xa5, sizeof(vram));
        assert(minigl_present_copy_rgb565(&p,rgb565,vram,sizeof(vram)));
        assert(vram[8] == 255 && vram[9] == 0 && vram[10] == 0 && vram[11] == 255);
        assert(vram[12] == 0 && vram[13] == 255 && vram[14] == 0 && vram[15] == 255);
    }
    p.dest_format=ZZ9K_MGL_PIXFMT_RGBA32;
    p.source_x=1; p.source_y=1; p.window_x=1; p.window_y=1;
    p.window_w=2; p.window_h=1; p.dest_pitch=24;
    memcpy(unchanged,vram,sizeof(vram));
    p.window_w=3;
    assert(!minigl_present_copy(&p,src,vram,sizeof(vram)));
    assert(memcmp(vram,unchanged,sizeof(vram))==0);
    p.window_w=2; p.dest_pitch=8;
    assert(!minigl_present_copy(&p,src,vram,sizeof(vram)));
    p.dest_pitch=24; p.window_y=UINT32_MAX;
    assert(!minigl_present_copy(&p,src,vram,sizeof(vram)));
    p.window_y=1; p.dest_offset=UINT32_MAX;
    assert(!minigl_present_copy(&p,src,vram,sizeof(vram)));
    p.dest_offset=0xffff;
    assert(!minigl_present_copy(&p,src,vram,sizeof(vram)));
    p.dest_offset=0x10008; p.dest_format=99;
    assert(!minigl_present_copy(&p,src,vram,sizeof(vram)));
    p.dest_format=0; p.target_width=ZZ9K_MINIGL_MAX_WIDTH+1;
    assert(!minigl_present_copy(&p,src,vram,sizeof(vram)));
    assert(memcmp(vram,unchanged,sizeof(vram))==0);
    puts("MiniGL presentation: explicit pitch, 8 pixel formats, cliprects and bounds passed");
    return 0;
}
