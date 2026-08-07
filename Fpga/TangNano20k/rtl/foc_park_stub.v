// Fixed-Point Q15 Hardware Park Transform: (alpha, beta, theta) -> (d, q)
`timescale 1ns / 1ps

module foc_park_stub (
    input  wire               clk,
    input  wire               rst_n,
    input  wire               enable,
    input  wire signed [15:0] ialpha,
    input  wire signed [15:0] ibeta,
    input  wire        [15:0] theta,
    output reg  signed [15:0] id,
    output reg  signed [15:0] iq
);

    wire signed [15:0] sin_val;
    wire signed [15:0] cos_val;

    // Instance 256-entry Q1.15 sine/cosine lookup table using upper 8 bits of theta
    sincos_lut lut_inst (
        .clk(clk),
        .angle(theta[15:8]),
        .sin_out(sin_val),
        .cos_out(cos_val)
    );

    wire signed [31:0] alpha_cos = $signed(ialpha) * $signed(cos_val);
    wire signed [31:0] beta_sin  = $signed(ibeta)  * $signed(sin_val);
    wire signed [31:0] alpha_sin = $signed(ialpha) * $signed(sin_val);
    wire signed [31:0] beta_cos  = $signed(ibeta)  * $signed(cos_val);

    wire signed [31:0] d_sum = alpha_cos + beta_sin;
    wire signed [31:0] q_sum = beta_cos  - alpha_sin;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            id <= 16'sd0;
            iq <= 16'sd0;
        end else if (enable) begin
            id <= d_sum[30:15]; // Shift Q15 product back down
            iq <= q_sum[30:15];
        end
    end
endmodule
