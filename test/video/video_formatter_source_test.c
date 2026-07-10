/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Source-level regression checks for video_formatter.v invariants that are
 * difficult to exercise without a Verilog simulator in the host test setup.
 * Functional pixel-level verification lives in video_formatter_tb.v
 * (run with test/video/run_formatter_sim.sh, requires Vivado xsim).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_FORMATTER_PATH "../../video_formatter.v"

static char *read_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	char *buffer;
	long size;

	if (!fp) {
		perror(path);
		return NULL;
	}

	if (fseek(fp, 0, SEEK_END) != 0) {
		perror("fseek");
		fclose(fp);
		return NULL;
	}

	size = ftell(fp);
	if (size < 0) {
		perror("ftell");
		fclose(fp);
		return NULL;
	}

	if (fseek(fp, 0, SEEK_SET) != 0) {
		perror("fseek");
		fclose(fp);
		return NULL;
	}

	buffer = malloc((size_t)size + 1U);
	if (!buffer) {
		fclose(fp);
		return NULL;
	}

	if (fread(buffer, 1U, (size_t)size, fp) != (size_t)size) {
		perror("fread");
		free(buffer);
		fclose(fp);
		return NULL;
	}

	buffer[size] = '\0';
	fclose(fp);
	return buffer;
}

static int require_contains(const char *text, const char *needle)
{
	if (!strstr(text, needle)) {
		printf("video_formatter.v: missing expected source pattern: %s\n", needle);
		return 0;
	}

	return 1;
}

static int require_absent(const char *text, const char *needle,
                          const char *message)
{
	if (strstr(text, needle)) {
		printf("video_formatter.v: %s: %s\n", message, needle);
		return 0;
	}

	return 1;
}

/*
 * The line buffer must be one asymmetric-port block RAM: 64-bit writes
 * (one VDMA beat per write, tkeep as byte enables), 32-bit reads addressed
 * by counter_scanout. Splitting it into even/odd word banks caused the
 * 2026-07 vertical-column regression.
 */
static int test_line_buffer_is_asymmetric_bram(const char *text)
{
	int ok = 1;

	ok &= require_contains(text, "xpm_memory_sdpram #(");
	ok &= require_contains(text, ".MEMORY_PRIMITIVE(\"block\")");
	ok &= require_contains(text, ".CLOCKING_MODE(\"independent_clock\")");
	ok &= require_contains(text, ".WRITE_DATA_WIDTH_A(64)");
	ok &= require_contains(text, ".BYTE_WRITE_WIDTH_A(8)");
	ok &= require_contains(text, ".READ_DATA_WIDTH_B(32)");
	ok &= require_contains(text, ".READ_LATENCY_B(1)");
	ok &= require_contains(text, ".addra(inptr[11:1])");
	ok &= require_contains(text, ".addrb(counter_scanout)");
	ok &= require_contains(text, ".dina(m_axis_vid_tdata)");
	ok &= require_absent(text, "line_buffer_even",
	                     "split even/odd line buffer banks remain");
	ok &= require_absent(text, "line_buffer_odd",
	                     "split even/odd line buffer banks remain");
	ok &= require_absent(text, "reg [31:0] line_buffer",
	                     "inferred line buffer remains");

	return ok ? 0 : 1;
}

/*
 * The BRAM's READ_LATENCY_B(1) output must feed pixout32 directly so the
 * scanout pipeline keeps the original one-cycle line-buffer latency: the
 * counter_subpixel byte/halfword unpacking for 8/16/15 bpp is phase-locked
 * to it. An extra register here swaps/duplicates pixel columns.
 */
static int test_scanout_pipeline_keeps_master_alignment(const char *text)
{
	int ok = 1;

	ok &= require_contains(text, "wire [31:0] pixout32;");
	ok &= require_contains(text, ".doutb(pixout32)");
	ok &= require_contains(text, "localparam PIPE_DELAY = 4;");
	ok &= require_absent(text, "counter_scanout_odd_d1",
	                     "extra bank-select delay register remains");
	ok &= require_absent(text, "counter_y_d3",
	                     "extra scanline parity delay remains");
	ok &= require_absent(text, "pixout32 <=",
	                     "pixout32 must come straight from the BRAM output");

	return ok ? 0 : 1;
}

/*
 * tkeep must gate the write byte lanes and the inptr word advance so the
 * partial tlast beat of an odd-word line lands correctly.
 */
static int test_64bit_tkeep_drives_writes(const char *text)
{
	int ok = 1;

	ok &= require_contains(text, "wire pixin_lo_valid = |m_axis_vid_tkeep[3:0];");
	ok &= require_contains(text, "wire pixin_hi_valid = |m_axis_vid_tkeep[7:4];");
	ok &= require_contains(text,
	    "wire [1:0] pixin_word_count = {1'b0, pixin_lo_valid} + {1'b0, pixin_hi_valid};");
	ok &= require_contains(text, "? m_axis_vid_tkeep : 8'h00;");
	ok &= require_absent(text, "wire pixin_lo_valid = 1'b1;",
	                     "stale always-valid lower-lane pattern remains");
	ok &= require_absent(text, "wire pixin_hi_valid = 1'b1;",
	                     "stale always-valid upper-lane pattern remains");
	ok &= require_absent(text, "wire [1:0] pixin_word_count = 2'd2;",
	                     "stale fixed two-word beat count remains");

	return ok ? 0 : 1;
}

static int test_dpms_gates_only_external_syncs(const char *text)
{
	int ok = 1;

	ok &= require_contains(text, "localparam OP_DPMS=21;");
	ok &= require_contains(text, "reg [1:0] dpms_level = DPMS_ON;");
	ok &= require_contains(text, "OP_DPMS: begin");
	ok &= require_contains(text, "dpms_level <= control_data_in[1:0];");
	ok &= require_contains(text,
	    "vga_dpms_level == DPMS_STANDBY || vga_dpms_level == DPMS_OFF");
	ok &= require_contains(text,
	    "vga_dpms_level == DPMS_SUSPEND || vga_dpms_level == DPMS_OFF");
	ok &= require_contains(text, "dvi_hsync <= 0^vga_sync_polarity;");
	ok &= require_contains(text, "dvi_vsync <= 0^vga_sync_polarity;");
	ok &= require_absent(text, "dvi_active_video <= 0; // DPMS",
	                     "DPMS must not stop the internal raster/vblank path");

	return ok ? 0 : 1;
}

int main(void)
{
	char *text = read_file(VIDEO_FORMATTER_PATH);
	int result;

	if (!text)
		return 1;

	result = test_line_buffer_is_asymmetric_bram(text);
	if (!result)
		result = test_scanout_pipeline_keeps_master_alignment(text);
	if (!result)
		result = test_64bit_tkeep_drives_writes(text);
	if (!result)
		result = test_dpms_gates_only_external_syncs(text);
	free(text);

	return result;
}
