// Rising-edge delayed complementary pair (non-overlap dead-time).
`timescale 1ns / 1ps

module deadtime_pair #(
    parameter integer W = 16
) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         enable,
    input  wire [W-1:0] dead_ticks,
    input  wire         ideal_high,
    output reg          out_high,
    output reg          out_low
);

    wire ideal_low = ~ideal_high;

    reg [W-1:0] dly_h, dly_l;
    reg         next_h, next_l;
    reg [W-1:0] next_dly_h, next_dly_l;

    always @(*) begin
        next_h     = out_high;
        next_l     = out_low;
        next_dly_h = dly_h;
        next_dly_l = dly_l;

        if (!enable) begin
            next_h     = 1'b0;
            next_l     = 1'b0;
            next_dly_h = {W{1'b0}};
            next_dly_l = {W{1'b0}};
        end else begin
            // High: fall immediately, rise after dead-time countdown
            if (!ideal_high) begin
                next_h     = 1'b0;
                next_dly_h = dead_ticks;
            end else if (dly_h != {W{1'b0}}) begin
                next_h     = 1'b0;
                next_dly_h = dly_h - 1'b1;
            end else begin
                next_h = 1'b1;
            end

            // Low: fall immediately, rise after dead-time countdown
            if (!ideal_low) begin
                next_l     = 1'b0;
                next_dly_l = dead_ticks;
            end else if (dly_l != {W{1'b0}}) begin
                next_l     = 1'b0;
                next_dly_l = dly_l - 1'b1;
            end else begin
                next_l = 1'b1;
            end

            // Hard safety: never both on
            if (next_h && next_l) begin
                next_h = 1'b0;
                next_l = 1'b0;
            end
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            out_high <= 1'b0;
            out_low  <= 1'b0;
            dly_h    <= {W{1'b0}};
            dly_l    <= {W{1'b0}};
        end else begin
            out_high <= next_h;
            out_low  <= next_l;
            dly_h    <= next_dly_h;
            dly_l    <= next_dly_l;
        end
    end

endmodule
