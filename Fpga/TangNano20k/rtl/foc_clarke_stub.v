// Fixed-Point Q15 Hardware Clarke Transform: 3-phase (U, V) -> (alpha, beta)
`timescale 1ns / 1ps

module foc_clarke_stub (
    input  wire               clk,
    input  wire               rst_n,
    input  wire               enable,
    input  wire signed [15:0] iu,
    input  wire signed [15:0] iv,
    output reg  signed [15:0] ialpha,
    output reg  signed [15:0] ibeta
);
    // Constant 1/sqrt(3) in Q15 format: 0.577350269 * 32768 = 18919 (0x49E6)
    localparam signed [15:0] ONE_OVER_SQRT3_Q15 = 16'sd18919;

    wire signed [16:0] sum_term = $signed(iu) + ($signed(iv) <<< 1); // iu + 2*iv
    wire signed [32:0] product  = sum_term * ONE_OVER_SQRT3_Q15;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            ialpha <= 16'sd0;
            ibeta  <= 16'sd0;
        end else if (enable) begin
            ialpha <= iu;
            ibeta  <= product[30:15]; // Scale Q15 back down
        end
    end
endmodule
