// SPDX-License-Identifier: GPL-3.0-or-later
// Vivado 2018.3 xpm_memory_sdpram predates the CASCADE_HEIGHT parameter
// used by RasterIX's upstream DualPortRam.  Package this compatible module
// instead of the vendored definition; its interface and memory behavior match.
module DualPortRam #(
    parameter MEM_WIDTH = 16,
    parameter WRITE_STROBE_WIDTH = 4,
    parameter MEMORY_PRIMITIVE = "block",
    parameter ADDR_WIDTH = 8,
    localparam WRITE_MASK_SIZE = MEM_WIDTH / WRITE_STROBE_WIDTH
) (
    input wire clk,
    input wire reset,
    input wire [MEM_WIDTH - 1 : 0] writeData,
    input wire write,
    input wire [ADDR_WIDTH - 1 : 0] writeAddr,
    input wire [WRITE_MASK_SIZE - 1 : 0] writeMask,
    output wire [MEM_WIDTH - 1 : 0] readData,
    input wire [ADDR_WIDTH - 1 : 0] readAddr
);
    genvar i;
    generate
        for (i = 0; i < WRITE_MASK_SIZE; i = i + 1) begin : gen_byte_ram
            xpm_memory_sdpram #(
                .ADDR_WIDTH_A(ADDR_WIDTH),
                .ADDR_WIDTH_B(ADDR_WIDTH),
                .AUTO_SLEEP_TIME(0),
                .BYTE_WRITE_WIDTH_A(WRITE_STROBE_WIDTH),
                .CLOCKING_MODE("common_clock"),
                .ECC_MODE("no_ecc"),
                .MEMORY_INIT_FILE("none"),
                .MEMORY_INIT_PARAM("0"),
                .MEMORY_OPTIMIZATION("true"),
                .MEMORY_PRIMITIVE(MEMORY_PRIMITIVE),
                .MEMORY_SIZE((2**ADDR_WIDTH) * WRITE_STROBE_WIDTH),
                .MESSAGE_CONTROL(0),
                .READ_DATA_WIDTH_B(WRITE_STROBE_WIDTH),
                .READ_LATENCY_B(1),
                .READ_RESET_VALUE_B("0"),
                .RST_MODE_A("SYNC"),
                .RST_MODE_B("SYNC"),
                .SIM_ASSERT_CHK(0),
                .USE_EMBEDDED_CONSTRAINT(0),
                .USE_MEM_INIT(0),
                .WAKEUP_TIME("disable_sleep"),
                .WRITE_DATA_WIDTH_A(WRITE_STROBE_WIDTH),
                .WRITE_MODE_B("read_first")
            ) xpm_memory_sdpram_inst (
                .dbiterrb(),
                .doutb(readData[((i + 1) * WRITE_STROBE_WIDTH) - 1 : i * WRITE_STROBE_WIDTH]),
                .sbiterrb(),
                .addra(writeAddr),
                .addrb(readAddr),
                .clka(clk),
                .clkb(clk),
                .dina(writeData[((i + 1) * WRITE_STROBE_WIDTH) - 1 : i * WRITE_STROBE_WIDTH]),
                .ena(1'b1),
                .enb(1'b1),
                .injectdbiterra(1'b0),
                .injectsbiterra(1'b0),
                .regceb(1'b0),
                .rstb(reset),
                .sleep(1'b0),
                .wea(writeMask[i] & write)
            );
        end
    endgenerate
endmodule
