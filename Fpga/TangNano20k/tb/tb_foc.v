// Testbench for Tang Nano 20K Hardware FOC Module Pipeline
`timescale 1ns / 1ps

module tb_foc;
    reg clk;
    reg rst_n;
    reg enable;

    reg signed [15:0] iu, iv;
    reg        [15:0] theta;
    wire signed [15:0] ialpha, ibeta;
    wire signed [15:0] id, iq;
    wire        [15:0] duty_u, duty_v, duty_w;

    // Clock generation: 27 MHz (37.037 ns period)
    always #18.518 clk = ~clk;

    // Instance Clarke transform
    foc_clarke_stub clarke_inst (
        .clk(clk), .rst_n(rst_n), .enable(enable),
        .iu(iu), .iv(iv),
        .ialpha(ialpha), .ibeta(ibeta)
    );

    // Instance Park transform
    foc_park_stub park_inst (
        .clk(clk), .rst_n(rst_n), .enable(enable),
        .ialpha(ialpha), .ibeta(ibeta), .theta(theta),
        .id(id), .iq(iq)
    );

    // Instance SVPWM modulator
    foc_svpwm_stub svpwm_inst (
        .clk(clk), .rst_n(rst_n), .enable(enable),
        .valpha(ialpha), .vbeta(ibeta),
        .duty_u(duty_u), .duty_v(duty_v), .duty_w(duty_w)
    );

    initial begin
        $dumpfile("tb_foc.vcd");
        $dumpvars(0, tb_foc);

        clk = 0;
        rst_n = 0;
        enable = 0;
        iu = 0;
        iv = 0;
        theta = 0;

        #100;
        rst_n = 1;
        #100;
        enable = 1;

        // Test vector 1: iu = 1000, iv = -500, theta = 0
        iu = 16'sd1000;
        iv = -16'sd500;
        theta = 16'd0;
        #200;

        $display("FOC Vector 1: Iu=%d, Iv=%d -> Ialpha=%d, Ibeta=%d -> Id=%d, Iq=%d -> Duty U=%d V=%d W=%d",
                 iu, iv, ialpha, ibeta, id, iq, duty_u, duty_v, duty_w);

        if (duty_u < 4000 || duty_u > 6500) begin
            $display("FAIL: duty_u out of expected range");
            $finish;
        end

        $display("PASS: tb_foc hardware acceleration pipeline OK");
        $finish;
    end
endmodule
