`timescale 1 ns / 1 ps
/*
 * Map sequential capture words into one rotated framebuffer row.
 *
 * The destination rotation must happen before the row stride is applied.
 * Moving the VDMA frame start instead crosses DDR row boundaries and mixes
 * pixels from adjacent captured scanlines.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

module videocap_writeback_layout #(
    /* Both values must stay aligned to the writeback burst length (16
     * pixels), because dest_x supplies one address for the complete burst. */
    parameter integer LINE_WIDTH = 1280,
    parameter integer ROTATE_PIXELS = 64
) (
    input  wire        full_width,
    input  wire [11:0] source_x,
    output wire [11:0] dest_x
);

localparam integer WRAP_AT = LINE_WIDTH - ROTATE_PIXELS;

assign dest_x = (full_width && source_x < LINE_WIDTH) ?
    ((source_x >= WRAP_AT) ?
        source_x - WRAP_AT : source_x + ROTATE_PIXELS) :
    source_x;

endmodule
