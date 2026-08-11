/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Source-level regression checks for video-pipeline RTL invariants that are
 * difficult to exercise without a Verilog simulator in the host test setup.
 * Functional pixel-level verification lives in video_formatter_tb.v and
 * videocap_sampler_tb.v (their runners require Vivado xsim).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_FORMATTER_PATH "../../video_formatter.v"
#define OVERLAY_LINEBUFFER_PATH "../../video_overlay_linebuffer.v"
#define PROJECT_TCL_PATH "../../zz9000_project.tcl"
#define MNTZORRO_PATH "../../mntzorro.v"
#define VIDEOCAP_SAMPLER_PATH "../../videocap_sampler.v"
#define VIDEO_C_PATH "../../ZZ9000_proto.sdk/ZZ9000OS/src/video.c"
#define HDMI_C_PATH "../../ZZ9000_proto.sdk/ZZ9000OS/src/hdmi.c"

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

	/* Keep multiline source invariants independent of the checkout's native
	 * line endings. Git may materialize Verilog as CRLF on Windows even when
	 * this host test runs under WSL. */
	{
		size_t read_pos;
		size_t write_pos = 0U;

		for (read_pos = 0U; read_pos < (size_t)size; read_pos++) {
			if (buffer[read_pos] != '\r')
				buffer[write_pos++] = buffer[read_pos];
		}
		buffer[write_pos] = '\0';
	}

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

static int require_source_contains(const char *source, const char *text,
		const char *needle)
{
	if (!strstr(text, needle)) {
		printf("%s: missing expected source pattern: %s\n", source, needle);
		return 0;
	}

	return 1;
}

static int require_source_absent(const char *source, const char *text,
		const char *needle, const char *message)
{
	if (strstr(text, needle)) {
		printf("%s: %s: %s\n", source, message, needle);
		return 0;
	}

	return 1;
}

/*
 * The line buffer must be one asymmetric-port block RAM with two complete
 * scanline banks: 64-bit writes (one VDMA beat per write, tkeep as byte
 * enables), 32-bit reads addressed by source-line bank and counter_scanout.
 * Splitting adjacent words into even/odd memories caused the 2026-07
 * vertical-column regression; selecting whole-line banks does not change the
 * established 64-to-32-bit unpack phase.
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
	ok &= require_contains(text,
	    ".addra({line_buffer_write_bank, inptr[11:1]})");
	ok &= require_contains(text,
	    ".addrb({scanout_source_line[0], counter_scanout})");
	ok &= require_contains(text,
	    ".MEMORY_SIZE(LINE_BUFFER_MEMORY_BITS)");
	ok &= require_contains(text,
	    "wire line_buffer_write_bank = pixin_framestart ? 1'b0 : input_line_bank;");
	ok &= require_contains(text,
	    "ready_for_vdma <= 1;\n            last_line_fetch <= 0;\n            next_input_state <= 4'h1;");
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

static int test_vertical_scale_shift_and_sprite_control_are_independent(
	const char *text)
{
	int ok = 1;

	ok &= require_contains(text, "reg [1:0] scale_y = 2'd1;");
	ok &= require_contains(text, "reg [1:0] scale_y_effective;");
	ok &= require_contains(text, "scale_y_effective <= scale_y;");
	ok &= require_contains(text,
	    "? ((counter_y - vga_scale_y_factor) >> vga_scale_y)");
	ok &= require_absent(text, "control_interlace ? 2'd0 : scale_y",
	                     "interlace still bypasses configured vertical scaling");
	ok &= require_absent(text, "control_interlace ? 2'd0 : vga_scale_y",
	                     "interlace still bypasses scanout row duplication");
	ok &= require_contains(text, "reg [1:0] vga_scale_y = 2'd0;");
	ok &= require_contains(text, "scale_y  <= control_data_in[2:1];");
	ok &= require_contains(text, "sprite_dbl <= control_data_in[3];");
	ok &= require_absent(text, "sprite_dbl <= control_data_in[1];",
	                     "sprite doubling still aliases vertical scaling");

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

static int test_overlay_rearms_only_after_generation_ack(const char *text)
{
	int ok = 1;

	ok &= require_contains(text, ".accepted_generation(overlay_accepted_generation)");
	ok &= require_contains(text, "reg [11:0] overlay_fetch_line = 0;");
	ok &= require_contains(text,
	    "wire [11:0] overlay_displayed_line = overlay_local_y >= 0");
	ok &= require_contains(text, ".fetch_line(overlay_fetch_line)");
	ok &= require_contains(text, ".fetch_request(overlay_fetch_request)");
	ok &= require_contains(text, ".displayed_line(overlay_displayed_line)");
	ok &= require_contains(text,
	    "overlay_fetch_line == overlay_displayed_line");
	ok &= require_contains(text,
	    "if ({4'b0, overlay_displayed_line} + 16'd1 < vga_overlay_height)");
	ok &= require_contains(text,
	    "overlay_fetch_line <= overlay_displayed_line + 1'b1;");
	ok &= require_contains(text,
	    "overlay_accepted_generation !=\n               vga_overlay_frame_generation");
	ok &= require_contains(text,
	    "vga_overlay_frame_generation <= overlay_accepted_generation;");
	ok &= require_contains(text, "overlay_fetch_request <= 1;");
	ok &= require_absent(text,
	    "if (counter_y == vga_v_sync_start && counter_x == 0)\n    overlay_requested_line <= 0;",
	    "overlay still prefetches line zero before the firmware vblank commit");
	ok &= require_absent(text, ".read_bank(overlay_fetch_line[0])",
	    "fetch selection still changes the displayed BRAM bank");

	return ok ? 0 : 1;
}

static int test_overlay_line_metadata_uses_atomic_cdc(const char *text)
{
	int ok = 1;

	ok &= require_contains(text, ") fetch_line_cdc (");
	ok &= require_contains(text, ") completed_line_cdc (");
	ok &= require_contains(text, ") generation_cdc (");
	ok &= require_contains(text, ".DEST_EXT_HSK(0)");
	ok &= require_contains(text, "if (fetch_line_axis_valid)");
	ok &= require_contains(text, "if (completed_line_pixel_valid)");
	ok &= require_contains(text, "if (accepted_generation_pixel_valid)");
	ok &= require_absent(text, "fetch_line_sync1",
	    "binary fetch-line bus still uses independent bit synchronizers");
	ok &= require_absent(text, "line0_sync1",
	    "ready-line tag still crosses independently from its valid flag");
	ok &= require_absent(text, "valid0_sync1",
	    "ready-valid flag still crosses independently from its line tag");

	return ok ? 0 : 1;
}

static int test_overlay_stream_has_no_prefetch_fifo(const char *text)
{
	int ok = 1;

	ok &= require_absent(text, "axis_data_fifo_1",
	    "overlay FIFO can prefetch the old surface across a frame handoff");
	ok &= require_contains(text,
	    "[get_bd_intf_pins axi_vdma_1/M_AXIS_MM2S] [get_bd_intf_pins video_formatter_0/overlay_axis]");

	return ok ? 0 : 1;
}

static int test_videocap_writeback_uses_axi_bursts(const char *text)
{
	int ok = 1;

	ok &= require_source_contains("mntzorro.v", text, "m01_axi_awlen <= 'hf;");
	ok &= require_source_contains("mntzorro.v", text, "m01_axi_awburst <= 'h1;");
	ok &= require_source_contains("mntzorro.v", text, "vc_beat = 0;");
	ok &= require_source_contains("mntzorro.v", text,
	    "assign m01_axi_wdata   = vcap_rdata;");
	ok &= require_source_contains("mntzorro.v", text,
	    "m01_axi_wlast <= (vc_beat == 5'd14);");
	ok &= require_source_contains("mntzorro.v", text,
	    "if (vc_beat == 5'd15) begin");
	ok &= require_source_contains("mntzorro.v", text,
	    "// Hold each BRAM address until its W beat is accepted.");
	ok &= require_source_contains("mntzorro.v", text,
	    "videocap_save_state <= 5;");
	ok &= require_source_contains("mntzorro.v", text, "4'h5: begin");
	ok &= require_source_contains("mntzorro.v", text,
	    "wire [10:0] vcap_wdata_source_x =\n"
	    "      videocap_save_x[10:0];");
	ok &= require_source_absent("mntzorro.v", text,
	    "videocap_save_x[10:0] - 1'b1",
	    "accepted-beat owner still assumes speculative BRAM prefetch");
	ok &= require_source_absent("mntzorro.v", text,
	    "Advance the address here so beat zero still sees",
	    "burst still advances the BRAM address before WREADY");
	ok &= require_source_absent("mntzorro.v", text, "m01_axi_wdata_out",
	    "registered write data breaks consecutive BRAM-fed beats");
	ok &= require_source_absent("mntzorro.v", text, "m01_axi_awburst <= 'h0;",
	    "capture writeback still uses fixed-address transactions");

	return ok ? 0 : 1;
}

static int test_videocap_write_probe_samples_accepted_axi_data(const char *text)
{
	int ok = 1;

	ok &= require_source_contains("mntzorro.v", text,
	    "localparam [15:0] VCAP_PROBE_DATA_BASE = 16'h0120;");
	ok &= require_source_contains("mntzorro.v", text,
	    "localparam [9:0] VCAP_PROBE_LINE = 10'd120;");
	ok &= require_source_contains("mntzorro.v", text,
	    "localparam [11:0] VCAP_PROBE_SOURCE_X = 12'd928;");
	ok &= require_source_contains("mntzorro.v", text,
	    "localparam [11:0] VCAP_PROBE_DEST_X = 12'd928;");
	ok &= require_source_contains("mntzorro.v", text,
	    ".ROTATE_PIXELS(0)");
	ok &= require_source_contains("mntzorro.v", text,
	    "vc_saving_line == VCAP_PROBE_LINE &&");
	ok &= require_source_contains("mntzorro.v", text,
	    "videocap_write_x == VCAP_PROBE_DEST_X");
	ok &= require_source_contains("mntzorro.v", text,
	    "m01_axi_wvalid_out && m01_axi_wready && vcap_probe_burst_active");
	ok &= require_source_contains("mntzorro.v", text,
	    "vcap_probe_data[vc_beat[3:0]] <= m01_axi_wdata;");
	ok &= require_source_contains("mntzorro.v", text,
	    "VCAP_PROBE_CONTROL,");
	ok &= require_source_contains("mntzorro.v", text,
	    "vcap_probe_arm_toggle <= ~vcap_probe_arm_toggle;");

	return ok ? 0 : 1;
}

static int test_videocap_probe_compares_sampler_and_line_owner(
	const char *mntzorro, const char *sampler)
{
	int ok = 1;

	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "parameter integer PROBE_LINE = 120");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "parameter integer PROBE_SOURCE_X = 928");
	ok &= require_source_contains("mntzorro.v", mntzorro,
	    ".PROBE_LINE(VCAP_PROBE_LINE)");
	ok &= require_source_contains("mntzorro.v", mntzorro,
	    ".PROBE_SOURCE_X(VCAP_PROBE_SOURCE_X)");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "probe_data[31:0] <= capture_store_word;");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "wire capture_head_valid = capture_banking_cap ?");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "1'b1 : (cap_x > 11'd2);");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "probe_seen_mask[14:0] == 15'h7fff");
	ok &= require_source_contains("mntzorro.v", mntzorro,
	    "vcap_sampler_probe_valid_axi &&");
	ok &= require_source_contains("mntzorro.v", mntzorro,
	    "vcap_probe_owner[vc_beat[3:0]] <= {");
	ok &= require_source_contains("mntzorro.v", mntzorro,
	    "localparam [15:0] VCAP_PROBE_SAMPLER_DATA_BASE = 16'h0170;");
	ok &= require_source_contains("mntzorro.v", mntzorro,
	    "localparam [15:0] VCAP_PROBE_OWNER_BASE = 16'h01c0;");

	return ok ? 0 : 1;
}

static int test_videocap_probe_captures_pre_crop_samples(
	const char *mntzorro, const char *sampler)
{
	int ok = 1;

	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "wire [11:0] probe_precrop_start = crop_h_local - 12'd64;");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "reg [31:0] probe_precrop_mem [0:63];");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "{1'b0, sample_x} >= probe_precrop_start");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "{1'b0, sample_x} < crop_h_local");
	ok &= require_source_contains("mntzorro.v", mntzorro,
	    "localparam [15:0] VCAP_PRE_CROP_PROBE_DATA_BASE = 16'h0300;");
	ok &= require_source_contains("mntzorro.v", mntzorro,
	    "localparam [15:0] VCAP_PRE_CROP_PROBE_META = 16'h02e0;");
	ok &= require_source_contains("mntzorro.v", mntzorro,
	    "VCAP_PRE_CROP_PROBE_DATA_BASE + 16'h0100");

	return ok ? 0 : 1;
}

static int test_videocap_full_width_owns_completed_bank(const char *mntzorro,
	const char *sampler)
{
	int ok = 1;

	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "localparam integer LINEBUF_BANKS = (FULLRATE != 0) ? 2 : 1;");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "wire capture_banking_cap = (FULLRATE != 0) && ctl_full_width_cap;");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "wire read_banking_axi = (FULLRATE != 0) && ctl_full_width;");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "capture_banking_cap ? capture_bank : 1'b0, cap_x");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "read_banking_axi ? buf_rbank : 1'b0, buf_raddr[10:0]");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "if (!cap_x_done && cap_x >= 11'd1279) begin");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "cap_line_toggle <= ~cap_line_toggle;");
	ok &= require_source_absent("videocap_sampler.v", sampler, "11'h400",
	    "full-width completion still stops at 1024 of 1280 samples");

	ok &= require_source_contains("mntzorro.v", mntzorro,
	    ") videocap_line_cdc (");
	ok &= require_source_contains("mntzorro.v", mntzorro,
	    "vcap_line_toggle, vcap_write_bank, vcap_y[9:0]");
	ok &= require_source_contains("mntzorro.v", mntzorro,
	    ".buf_rbank(vc_saving_bank)");
	ok &= require_source_contains("mntzorro.v", mntzorro,
	    "vc_saving_bank <= videocap_bank_sync;");
	ok &= require_source_contains("mntzorro.v", mntzorro,
	    "vcap_line_payload_axi[11] != vcap_line_toggle_seen");

	return ok ? 0 : 1;
}

static int test_videocap_interlace_uses_raw_horizontal_phase(
	const char *sampler)
{
	int ok = 1;

	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "reg [11:0] phase_x = 0;");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "reg [11:0] phase_line_period = 0;");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "(phase_x > vsync_phase_x) ?");
	ok &= require_source_contains("videocap_sampler.v", sampler,
	    "vsync_phase_abs_plus_threshold <= {1'b0, phase_line_period}");
	ok &= require_source_absent("videocap_sampler.v", sampler,
	    "(cap_x > vsync_x)",
	    "interlace phase still depends on cropped/scaled cap_x");

	return ok ? 0 : 1;
}

static int test_mode_switch_retrains_display(const char *video,
	const char *hdmi)
{
	int ok = 1;
	const char *prepare;
	const char *clock;
	const char *enable;

	ok &= require_source_contains("video.c", video,
	    "hdmi_ctrl_prepare_mode(vmode);");
	ok &= require_source_contains("video.c", video,
	    "CLK_WIZ_STATUS_LOCKED");
	ok &= require_source_contains("video.c", video,
	    "CLK_WIZ_RECONFIG_LOAD");
	ok &= require_source_contains("video.c", video,
	    "hdmi_ctrl_enable_output();");
	ok &= require_source_contains("hdmi.c", hdmi,
	    "SII9022_SYS_CTRL_PWR_DWN");
	ok &= require_source_contains("hdmi.c", hdmi,
	    "hdmi_set_video_mode(mode->hmax, mode->vmax, mode->phz, mode->vhz");

	prepare = strstr(video, "hdmi_ctrl_prepare_mode(vmode);");
	clock = prepare ? strstr(prepare, "pixelclock_init_2(vmode);") : NULL;
	enable = clock ? strstr(clock, "hdmi_ctrl_enable_output();") : NULL;
	if (!prepare || !clock || !enable) {
		printf("video.c: display transition is not prepare -> clock -> enable\n");
		ok = 0;
	}

	return ok ? 0 : 1;
}

int main(void)
{
	char *text = read_file(VIDEO_FORMATTER_PATH);
	char *linebuffer;
	char *project;
	char *mntzorro;
	char *sampler;
	char *video;
	char *hdmi;
	int result;

	if (!text)
		return 1;

	result = test_line_buffer_is_asymmetric_bram(text);
	if (!result)
		result = test_scanout_pipeline_keeps_master_alignment(text);
	if (!result)
		result = test_vertical_scale_shift_and_sprite_control_are_independent(text);
	if (!result)
		result = test_64bit_tkeep_drives_writes(text);
	if (!result)
		result = test_dpms_gates_only_external_syncs(text);
	if (!result)
		result = test_overlay_rearms_only_after_generation_ack(text);
	free(text);

	linebuffer = read_file(OVERLAY_LINEBUFFER_PATH);
	if (!linebuffer)
		return 1;
	if (!result)
		result = test_overlay_line_metadata_uses_atomic_cdc(linebuffer);
	free(linebuffer);

	project = read_file(PROJECT_TCL_PATH);
	if (!project)
		return 1;
	if (!result)
		result = test_overlay_stream_has_no_prefetch_fifo(project);
	free(project);

	mntzorro = read_file(MNTZORRO_PATH);
	if (!mntzorro)
		return 1;
	if (!result)
		result = test_videocap_writeback_uses_axi_bursts(mntzorro);
	if (!result)
		result = test_videocap_write_probe_samples_accepted_axi_data(mntzorro);
	sampler = read_file(VIDEOCAP_SAMPLER_PATH);
	if (!sampler) {
		free(mntzorro);
		return 1;
	}
	if (!result)
		result = test_videocap_full_width_owns_completed_bank(mntzorro,
			sampler);
	if (!result)
		result = test_videocap_probe_compares_sampler_and_line_owner(
			mntzorro, sampler);
	if (!result)
		result = test_videocap_probe_captures_pre_crop_samples(
			mntzorro, sampler);
	if (!result)
		result = test_videocap_interlace_uses_raw_horizontal_phase(sampler);
	free(sampler);
	free(mntzorro);

	video = read_file(VIDEO_C_PATH);
	hdmi = read_file(HDMI_C_PATH);
	if (!video || !hdmi) {
		free(video);
		free(hdmi);
		return 1;
	}
	if (!result)
		result = test_mode_switch_retrains_display(video, hdmi);
	free(video);
	free(hdmi);

	return result;
}
