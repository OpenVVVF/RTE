// Stub: Clarke transform shell for later FOC (not in active PWM path).
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
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            ialpha <= 16'sd0;
            ibeta  <= 16'sd0;
        end else if (enable) begin
            // Placeholder: ialpha≈iu, ibeta≈(iu+2*iv)/sqrt(3) — not implemented
            ialpha <= iu;
            ibeta  <= iv;
        end
    end
endmodule
