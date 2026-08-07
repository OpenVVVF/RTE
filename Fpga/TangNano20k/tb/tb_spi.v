// SPI slave + register file testbench (simulated master, no MCU). Fail-fast.
`timescale 1ns / 1ps

module tb_spi;
    localparam integer CLK_HZ  = 27_000_000;
    localparam real    CLK_PER = 37.037; // ~27 MHz

    reg clk = 0;
    reg rst_n = 0;
    always #(CLK_PER/2.0) clk = ~clk;

    reg  spi_sclk = 0;
    reg  spi_cs_n = 1;
    reg  spi_mosi = 0;
    wire spi_miso;

    wire        pwm_enable;
    wire [15:0] freq_hz, deadtime_ns, duty_u, duty_v, duty_w;
    reg         pwm_running = 0;
    reg         fault_bad_freq = 0, fault_bad_duty = 0, fault_bad_dead = 0;

    spi_regs #(.CLK_HZ(CLK_HZ)) dut (
        .clk(clk),
        .rst_n(rst_n),
        .spi_sclk(spi_sclk),
        .spi_cs_n(spi_cs_n),
        .spi_mosi(spi_mosi),
        .spi_miso(spi_miso),
        .pwm_enable(pwm_enable),
        .freq_hz(freq_hz),
        .deadtime_ns(deadtime_ns),
        .duty_u(duty_u),
        .duty_v(duty_v),
        .duty_w(duty_w),
        .pwm_running(pwm_running),
        .fault_bad_freq(fault_bad_freq),
        .fault_bad_duty(fault_bad_duty),
        .fault_bad_dead(fault_bad_dead)
    );

    localparam real SCK_HALF = 200.0; // 2.5 MHz SPI

    task spi_write;
        input [6:0] addr;
        input [15:0] data;
        integer bi;
        reg [23:0] frame;
        begin
            frame = {1'b0, addr, data};
            spi_cs_n = 0;
            spi_sclk = 0;
            spi_mosi = 0;
            #(SCK_HALF);
            for (bi = 23; bi >= 0; bi = bi - 1) begin
                spi_mosi = frame[bi];
                #(SCK_HALF);
                spi_sclk = 1;
                #(SCK_HALF);
                spi_sclk = 0;
            end
            #(SCK_HALF);
            spi_cs_n = 1;
            spi_mosi = 0;
            #(SCK_HALF * 4);
        end
    endtask

    task spi_read;
        input  [6:0] addr;
        output [15:0] data;
        integer bi;
        reg [7:0] cmd;
        begin
            cmd = {1'b1, addr};
            data = 16'h0000;
            spi_cs_n = 0;
            spi_sclk = 0;
            spi_mosi = 0;
            #(SCK_HALF);
            // Address byte
            for (bi = 7; bi >= 0; bi = bi - 1) begin
                spi_mosi = cmd[bi];
                #(SCK_HALF);
                spi_sclk = 1;
                #(SCK_HALF);
                spi_sclk = 0;
            end
            // Data bytes (MOSI dummy 0); sample MISO on rising
            for (bi = 15; bi >= 0; bi = bi - 1) begin
                spi_mosi = 1'b0;
                #(SCK_HALF);
                spi_sclk = 1;
                data[bi] = spi_miso;
                #(SCK_HALF);
                spi_sclk = 0;
            end
            #(SCK_HALF);
            spi_cs_n = 1;
            #(SCK_HALF * 4);
        end
    endtask

    reg [15:0] rdata;

    initial begin
        $dumpfile("tb_spi.vcd");
        $dumpvars(0, tb_spi);

        rst_n = 0;
        repeat (30) @(posedge clk);
        rst_n = 1;
        repeat (30) @(posedge clk);

        // Defaults
        if (freq_hz !== 16'd10000 || deadtime_ns !== 16'd1000) begin
            $display("FAIL: default freq/dead %0d %0d", freq_hz, deadtime_ns);
            $fatal(1);
        end
        if (duty_u !== 16'd5000 || duty_v !== 16'd5000 || duty_w !== 16'd5000) begin
            $display("FAIL: default duty %0d %0d %0d", duty_u, duty_v, duty_w);
            $fatal(1);
        end
        if (pwm_enable !== 1'b0) begin
            $display("FAIL: PWM should be disabled at reset");
            $fatal(1);
        end

        // MAGIC / VERSION
        spi_read(7'h00, rdata);
        if (rdata !== 16'h544E) begin
            $display("FAIL: MAGIC got %04h", rdata);
            $fatal(1);
        end
        spi_read(7'h01, rdata);
        if (rdata !== 16'h0001) begin
            $display("FAIL: VERSION got %04h", rdata);
            $fatal(1);
        end

        // Scratch loopback
        spi_write(7'h0A, 16'hA5A5);
        spi_read(7'h0A, rdata);
        if (rdata !== 16'hA5A5) begin
            $display("FAIL: SCRATCH got %04h", rdata);
            $fatal(1);
        end

        // Program duties / freq / dead / enable
        spi_write(7'h05, 16'd8000);
        spi_write(7'h06, 16'd1500);
        spi_write(7'h07, 16'd1111);
        spi_write(7'h08, 16'd2222);
        spi_write(7'h09, 16'd3333);
        spi_write(7'h04, 16'h0001);

        if (freq_hz !== 16'd8000) begin
            $display("FAIL: FREQ %0d", freq_hz);
            $fatal(1);
        end
        if (deadtime_ns !== 16'd1500) begin
            $display("FAIL: DEAD %0d", deadtime_ns);
            $fatal(1);
        end
        if (duty_u !== 16'd1111 || duty_v !== 16'd2222 || duty_w !== 16'd3333) begin
            $display("FAIL: DUTY %0d %0d %0d", duty_u, duty_v, duty_w);
            $fatal(1);
        end
        if (pwm_enable !== 1'b1) begin
            $display("FAIL: enable not set");
            $fatal(1);
        end

        // Read-back CTRL / FREQ
        spi_read(7'h04, rdata);
        if (rdata !== 16'h0001) begin
            $display("FAIL: CTRL read %04h", rdata);
            $fatal(1);
        end
        spi_read(7'h05, rdata);
        if (rdata !== 16'd8000) begin
            $display("FAIL: FREQ read %04h", rdata);
            $fatal(1);
        end

        // STATUS with running
        pwm_running = 1'b1;
        #(SCK_HALF * 8);
        spi_read(7'h02, rdata);
        if (rdata[0] !== 1'b1 || rdata[2] !== 1'b1) begin
            $display("FAIL: STATUS %04h expected EN+RUNNING", rdata);
            $fatal(1);
        end

        // Sticky fault + W1C
        fault_bad_freq = 1'b1;
        repeat (5) @(posedge clk);
        fault_bad_freq = 1'b0;
        spi_read(7'h03, rdata);
        if (rdata[0] !== 1'b1) begin
            $display("FAIL: FAULT bit0 not set %04h", rdata);
            $fatal(1);
        end
        spi_write(7'h03, 16'h0001);
        spi_read(7'h03, rdata);
        if (rdata[0] !== 1'b0) begin
            $display("FAIL: FAULT W1C failed %04h", rdata);
            $fatal(1);
        end

        // CLK_HZ split
        spi_read(7'h0B, rdata);
        if (rdata !== CLK_HZ[15:0]) begin
            $display("FAIL: CLK_LO %04h", rdata);
            $fatal(1);
        end
        spi_read(7'h0C, rdata);
        if (rdata !== CLK_HZ[31:16]) begin
            $display("FAIL: CLK_HI %04h", rdata);
            $fatal(1);
        end

        $display("PASS: tb_spi register map + Mode-0 slave OK");
        $finish;
    end

    initial begin
        #20_000_000;
        $display("FAIL: tb_spi timeout");
        $fatal(1);
    end
endmodule
