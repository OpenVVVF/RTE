// Stub: Park transform shell for later FOC (not in active PWM path).
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
    // Keep theta in the sensitivity cone for later sin/cos use
    wire unused_theta_ok = |theta;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            id <= 16'sd0;
            iq <= 16'sd0;
        end else if (enable) begin
            id <= ialpha;
            iq <= ibeta;
        end else if (unused_theta_ok) begin
            // idle
        end
    end
endmodule
