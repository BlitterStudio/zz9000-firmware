`timescale 1ns/1ps

module texenv_modulate_tb;
    reg clock = 0;
    reg resetn = 0;
    reg [31:0] conf;
    reg s_valid;
    reg [31:0] s_texSrcColor;
    reg [31:0] s_primaryColor;
    wire m_valid;
    wire [31:0] m_color;
    integer checks = 0;

    always #5 clock = ~clock;

    TexEnv tex_env (
        .aclk(clock),
        .resetn(resetn),
        .conf(conf),
        .s_ready(),
        .s_valid(s_valid),
        .s_user(1'b0),
        .s_previousColor(32'b0),
        .s_texSrcColor(s_texSrcColor),
        .s_primaryColor(s_primaryColor),
        .s_envColor(32'b0),
        .m_ready(1'b1),
        .m_valid(m_valid),
        .m_user(),
        .m_color(m_color)
    );

    task check_modulate_white;
        input [31:0] texel;
        begin
            while (m_valid) @(posedge clock);
            s_texSrcColor = texel;
            s_primaryColor = 32'hffffffff;
            s_valid = 1'b1;
            @(posedge clock);
            s_valid = 1'b0;
            wait (m_valid);
            if (!m_valid || m_color !== texel) begin
                $display("MISMATCH texel=%08x actual=%08x valid=%b", texel,
                         m_color, m_valid);
                $fatal(1);
            end
            checks = checks + 1;
        end
    endtask

    initial begin
        /* RGB/alpha MODULATE, texture x primary, SRC_COLOR/SRC_ALPHA. */
        conf = 32'h00288201;
        s_valid = 1'b0;
        s_texSrcColor = 32'b0;
        s_primaryColor = 32'b0;
        repeat (2) @(posedge clock);
        resetn = 1'b1;
        check_modulate_white(32'ha07040ff);
        check_modulate_white(32'h000000ff);
        check_modulate_white(32'hffffffff);
        $display("RESULT PASS checks=%0d", checks);
        $finish;
    end
endmodule
