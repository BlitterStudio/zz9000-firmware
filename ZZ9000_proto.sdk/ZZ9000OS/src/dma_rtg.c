#include <stdio.h>
#include <xil_types.h>
#include "memorymap.h"
#include "zz_video_modes.h"
#include "gfx.h"
#include "video.h"
#include "xil_printf.h"

void handle_blitter_dma_op(struct ZZ_VIDEO_STATE* vs, uint16_t zdata)
{
    struct GFXData *data = (struct GFXData*)((u32)Z3_SCRATCH_ADDR);
    switch(zdata) {
        case OP_DRAWLINE:
            SWAP16(data->x[0]);		SWAP16(data->x[1]);
            SWAP16(data->y[0]);		SWAP16(data->y[1]);
            SWAP16(data->user[0]);	SWAP16(data->user[1]);

            SWAP16(data->pitch[0]);
            SWAP32(data->offset[0]);

            set_fb((uint32_t*) ((u32)vs->framebuffer + data->offset[0]),
                    data->pitch[0]);

            if (data->user[1] == 0xFFFF && data->mask == 0xFF)
                draw_line_solid(data->x[0], data->y[0], data->x[1], data->y[1],
                        data->user[0], data->rgb[0],
                        data->u8_user[GFXDATA_U8_COLORMODE]);
            else
                draw_line(data->x[0], data->y[0], data->x[1], data->y[1],
                        data->user[0], data->user[1], data->user[2], data->rgb[0], data->rgb[1],
                        data->u8_user[GFXDATA_U8_COLORMODE], data->mask, data->u8_user[GFXDATA_U8_DRAWMODE]);

            break;

        case OP_FILLRECT:
            SWAP16(data->x[0]);		SWAP16(data->x[1]);
            SWAP16(data->y[0]);		SWAP16(data->y[1]);

            SWAP16(data->pitch[0]);
            SWAP32(data->offset[0]);

            set_fb((uint32_t*) ((u32)vs->framebuffer + data->offset[0]),
                    data->pitch[0]);

            if (data->mask == 0xFF)
                fill_rect_solid(data->x[0], data->y[0], data->x[1], data->y[1],
                        data->rgb[0], data->u8_user[GFXDATA_U8_COLORMODE]);
            else
                fill_rect(data->x[0], data->y[0], data->x[1], data->y[1], data->rgb[0],
                        data->u8_user[GFXDATA_U8_COLORMODE], data->mask);
            break;

        case OP_COPYRECT:
        case OP_COPYRECT_NOMASK:
            SWAP16(data->x[0]);		SWAP16(data->x[1]);		SWAP16(data->x[2]);
            SWAP16(data->y[0]);		SWAP16(data->y[1]);		SWAP16(data->y[2]);

            SWAP16(data->pitch[0]);		SWAP16(data->pitch[1]);
            SWAP32(data->offset[0]);	SWAP32(data->offset[1]);

            set_fb((uint32_t*) ((u32)vs->framebuffer + data->offset[0]),
                    data->pitch[0]);

            switch (zdata) {
            case 3: // Regular BlitRect
                if (data->mask == 0xFF || (data->mask != 0xFF && data->u8_user[GFXDATA_U8_COLORMODE] != MNTVA_COLOR_8BIT))
                    copy_rect_nomask(data->x[0], data->y[0], data->x[1], data->y[1], data->x[2],
                                    data->y[2], data->u8_user[GFXDATA_U8_COLORMODE],
                                    (uint32_t*) ((u32)vs->framebuffer + data->offset[0]),
                                    data->pitch[0], MINTERM_SRC);
                else
                    copy_rect(data->x[0], data->y[0], data->x[1], data->y[1], data->x[2],
                            data->y[2], data->u8_user[GFXDATA_U8_COLORMODE],
                            (uint32_t*) ((u32)vs->framebuffer + data->offset[0]),
                            data->pitch[0], data->mask);
                break;
            case 4: // BlitRectNoMaskComplete
                copy_rect_nomask(data->x[0], data->y[0], data->x[1], data->y[1], data->x[2],
                                data->y[2], data->u8_user[GFXDATA_U8_COLORMODE],
                                (uint32_t*) ((u32)vs->framebuffer + data->offset[1]),
                                data->pitch[1], data->minterm);
                break;
            }
            break;

        case OP_RECT_PATTERN:
        case OP_RECT_TEMPLATE: {
            SWAP16(data->x[0]);		SWAP16(data->x[1]);		SWAP16(data->x[2]);
            SWAP16(data->y[0]);		SWAP16(data->y[1]);		SWAP16(data->y[2]);

            SWAP16(data->pitch[0]);		SWAP16(data->pitch[1]);
            SWAP32(data->offset[0]);	SWAP32(data->offset[1]);

            uint8_t* tmpl_data = (uint8_t*) ((u32)vs->framebuffer
                    + data->offset[1]);
            set_fb((uint32_t*) ((u32)vs->framebuffer + data->offset[0]),
                    data->pitch[0]);


            uint8_t bpp = 2 * data->u8_user[GFXDATA_U8_COLORMODE];
            if (bpp == 0)
                bpp = 1;
            uint16_t loop_rows = 0;

            if (zdata == OP_RECT_PATTERN) {
                SWAP16(data->user[0]);

                loop_rows = data->user[0];

                pattern_fill_rect(data->u8_user[GFXDATA_U8_COLORMODE],
                        data->x[0], data->y[0], data->x[1], data->y[1],
                        data->u8_user[GFXDATA_U8_DRAWMODE], data->mask,
                        data->rgb[0], data->rgb[1], data->x[2], data->y[2],
                        tmpl_data, 16, loop_rows);
            }
            else {
                template_fill_rect(data->u8_user[GFXDATA_U8_COLORMODE], data->x[0],
                        data->y[0], data->x[1], data->y[1], data->u8_user[GFXDATA_U8_DRAWMODE], data->mask,
                        data->rgb[0], data->rgb[1], data->x[2], data->y[2], tmpl_data,
                        data->pitch[1]);
            }

            break;
        }

        case OP_P2C:
        case OP_P2D: {
            SWAP16(data->x[0]);		SWAP16(data->x[1]);		SWAP16(data->x[2]);
            SWAP16(data->y[0]);		SWAP16(data->y[1]);		SWAP16(data->y[2]);

            SWAP16(data->pitch[0]);		SWAP16(data->pitch[1]);
            SWAP32(data->offset[0]);	SWAP32(data->offset[1]);

            SWAP16(data->user[0]);
            SWAP16(data->user[1]);

            uint8_t* bmp_data = (uint8_t*) ((u32)vs->framebuffer
                    + data->offset[1]);

            set_fb((uint32_t*) ((u32)vs->framebuffer + data->offset[0]),
                    data->pitch[0]);

            if (zdata == OP_P2C) {
                p2c_rect(data->x[0], 0, data->x[1], data->y[1], data->x[2],
                        data->y[2], data->minterm, data->user[1], data->mask,
                        data->user[0], data->pitch[1], bmp_data);
            }
            else {
                p2d_rect(data->x[0], 0, data->x[1], data->y[1], data->x[2],
                        data->y[2], data->minterm, data->user[1], data->mask, data->user[0],
                        data->rgb[0], data->pitch[1], bmp_data, data->u8_user[GFXDATA_U8_COLORMODE]);
            }
            break;
        }

        case OP_INVERTRECT:
            SWAP16(data->x[0]);		SWAP16(data->x[1]);
            SWAP16(data->y[0]);		SWAP16(data->y[1]);

            SWAP16(data->pitch[0]);
            SWAP32(data->offset[0]);

            set_fb((uint32_t*) ((u32)vs->framebuffer + data->offset[0]),
                    data->pitch[0]);
            invert_rect(data->x[0], data->y[0], data->x[1], data->y[1],
                    data->mask, data->u8_user[GFXDATA_U8_COLORMODE]);
            break;

        case OP_SPRITE_XY:
            if (!vs->sprite_showing)
                break;

            SWAP16(data->x[0]);     SWAP16(data->y[0]);

            vs->sprite_x_base = (int16_t)data->x[0];
            vs->sprite_y_base = (int16_t)data->y[0];

            update_hw_sprite_pos();
            break;

        case OP_SPRITE_CLUT_BITMAP:
        case OP_SPRITE_BITMAP: {
            SWAP16(data->x[0]);		SWAP16(data->x[1]);
            SWAP16(data->y[0]);		SWAP16(data->y[1]);

            SWAP32(data->offset[1]);

            uint8_t* bmp_data;

            if (zdata == OP_SPRITE_BITMAP)
                bmp_data = (uint8_t*) ((u32)vs->framebuffer + data->offset[1]);
            else
                bmp_data = (uint8_t*) ((u32) ADDR_ADJ + data->offset[1]);

            clear_hw_sprite();

            vs->sprite_x_offset = (int16_t)data->x[0];
            vs->sprite_y_offset = (int16_t)data->y[0];
            vs->sprite_width  = data->x[1];
            if (zdata == OP_SPRITE_CLUT_BITMAP) {
            	vs->sprite_x_offset = -(vs->sprite_x_offset);
            	vs->sprite_y_offset = -(vs->sprite_y_offset);
            }
            vs->sprite_height = data->y[1];

            if (zdata == OP_SPRITE_BITMAP) {
                update_hw_sprite(bmp_data);
            }
            else {
                //printf("Making a %dx%d cursor (%i %i)\n", sprite_width, sprite_height, sprite_x_offset, sprite_y_offset);
                update_hw_sprite_clut(bmp_data, data->clut1,
                		vs->sprite_width, vs->sprite_height, data->u8offset);
            }
            update_hw_sprite_pos();
            break;
        }
        case OP_SPRITE_COLOR: {
            vs->sprite_colors[data->u8offset] = data->rgb[0];
            if (data->u8offset != 0 && vs->sprite_colors[data->u8offset] == 0xff00ff)
            	vs->sprite_colors[data->u8offset] = 0xfe00fe;
            break;
        }

        case OP_PAN:
            SWAP32(data->offset[0]);
            SWAP16(data->x[0]);
            SWAP16(data->y[0]);
            SWAP16(data->x[1]);

            vs->sprite_x_offset = (int16_t)data->x[0];
            vs->sprite_y_offset = (int16_t)data->y[0];

            vs->framebuffer_pan_width = data->x[1];
            u32 framebuffer_color_format = data->u8_user[GFXDATA_U8_COLORMODE];
            vs->framebuffer_pan_offset = data->offset[0] + (data->x[0] << data->u8_user[GFXDATA_U8_COLORMODE]);
            vs->framebuffer_pan_offset += (data->y[0] * (vs->framebuffer_pan_width << framebuffer_color_format));
            break;

        case OP_SET_SPLIT_POS:
            SWAP16(data->y[0]);
            SWAP32(data->offset[0]);
            vs->bgbuf_offset = data->offset[0];

            vs->split_request_pos = data->y[0];
            break;

        default:
            break;
    }
}
