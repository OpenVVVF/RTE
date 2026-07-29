// Stub: SVPWM shell for later FOC (not in active PWM path).
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
    wire unused_v_ok = |valpha | |vbeta;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            duty_u <= 16'd5000;
            duty_v <= 16'd5000;
            duty_w <= 16'd5000;
        end else if (enable) begin
            // Placeholder 50% — real SVPWM from valpha/vbeta later
            duty_u <= 16'd5000;
            duty_v <= 16'd5000;
            duty_w <= 16'd5000;
        end else if (unused_v_ok) begin
            // idle
        end
    end
endmodule
