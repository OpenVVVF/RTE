// Hardware Space Vector Modulator (SVPWM): (valpha, vbeta) -> 3-phase duty (duty_u, duty_v, duty_w) in 0.01% units (0..10000)
`timescale 1ns / 1ps

module foc_svpwm_stub (
    input  wire               clk,
    input  wire               rst_n,
    input  wire               enable,
    input  wire signed [15:0] valpha,
    input  wire signed [15:0] vbeta,
    output reg         [15:0] duty_u,
    output reg         [15:0] duty_v,
    output reg         [15:0] duty_w
);
    // Constant sqrt(3)/2 in Q15 format: 0.86602540378 * 32768 = 28378 (0x6ED9)
    localparam signed [15:0] SQRT3_OVER_2_Q15 = 16'sd28378;

    wire signed [31:0] beta_term = $signed(vbeta) * SQRT3_OVER_2_Q15;
    wire signed [15:0] beta_scaled = beta_term[30:15];

    wire signed [15:0] va = valpha;
    wire signed [15:0] vb = -$signed(valpha >>> 1) + beta_scaled;
    wire signed [15:0] vc = -$signed(valpha >>> 1) - beta_scaled;

    // Find min and max among va, vb, vc
    reg signed [15:0] vmin;
    reg signed [15:0] vmax;

    always @(*) begin
        vmin = va;
        if (vb < vmin) vmin = vb;
        if (vc < vmin) vmin = vc;

        vmax = va;
        if (vb > vmax) vmax = vb;
        if (vc > vmax) vmax = vc;
    end

    wire signed [15:0] voffset = (vmin + vmax) >>> 1;

    wire signed [15:0] va_mod = va - voffset;
    wire signed [15:0] vb_mod = vb - voffset;
    wire signed [15:0] vc_mod = vc - voffset;

    // Convert Q15 modulation [-32768, +32767] to duty [0, 10000]
    // duty = 5000 + (v_mod * 5000 / 32768) = 5000 + (v_mod * 625 / 4096)
    wire signed [31:0] du_calc = 32'sd5000 + (($signed(va_mod) * 32'sd625) >>> 12);
    wire signed [31:0] dv_calc = 32'sd5000 + (($signed(vb_mod) * 32'sd625) >>> 12);
    wire signed [31:0] dw_calc = 32'sd5000 + (($signed(vc_mod) * 32'sd625) >>> 12);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            duty_u <= 16'd5000;
            duty_v <= 16'd5000;
            duty_w <= 16'd5000;
        end else if (enable) begin
            duty_u <= (du_calc < 0) ? 16'd0 : ((du_calc > 10000) ? 16'd10000 : du_calc[15:0]);
            duty_v <= (dv_calc < 0) ? 16'd0 : ((dv_calc > 10000) ? 16'd10000 : dv_calc[15:0]);
            duty_w <= (dw_calc < 0) ? 16'd0 : ((dw_calc > 10000) ? 16'd10000 : dw_calc[15:0]);
        end else begin
            duty_u <= 16'd5000;
            duty_v <= 16'd5000;
            duty_w <= 16'd5000;
        end
    end
endmodule
