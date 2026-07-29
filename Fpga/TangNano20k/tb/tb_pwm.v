// PWM testbench: duty, dead-time, complementary non-overlap. Fail-fast ($fatal).
`timescale 1ns / 1ps

module tb_pwm;
    localparam integer CLK_HZ   = 1_000_000; // 1 MHz sim clock for speed
    localparam real    CLK_PER  = 1000.0;    // ns
    localparam integer FREQ_HZ  = 1000;      // 1 kHz switching
    localparam integer DEAD_NS  = 10_000;    // 10 us @ 1 MHz = 10 ticks
    localparam integer DUTY_U   = 2500;      // 25.00%
    localparam integer DUTY_V   = 5000;      // 50.00%
    localparam integer DUTY_W   = 7500;      // 75.00%

    reg clk = 0;
    reg rst_n = 0;
    always #(CLK_PER/2.0) clk = ~clk;

    wire pwm_uh, pwm_ul, pwm_vh, pwm_vl, pwm_wh, pwm_wl;
    wire fault_bad_freq, fault_bad_duty, fault_bad_dead, running;

    pwm_complementary #(.CLK_HZ(CLK_HZ)) dut (
        .clk(clk),
        .rst_n(rst_n),
        .enable(1'b1),
        .freq_hz(FREQ_HZ[15:0]),
        .deadtime_ns(DEAD_NS[15:0]),
        .duty_u(DUTY_U[15:0]),
        .duty_v(DUTY_V[15:0]),
        .duty_w(DUTY_W[15:0]),
        .pwm_uh(pwm_uh), .pwm_ul(pwm_ul),
        .pwm_vh(pwm_vh), .pwm_vl(pwm_vl),
        .pwm_wh(pwm_wh), .pwm_wl(pwm_wl),
        .fault_bad_freq(fault_bad_freq),
        .fault_bad_duty(fault_bad_duty),
        .fault_bad_dead(fault_bad_dead),
        .running(running)
    );

    integer overlap_u, overlap_v, overlap_w;
    integer high_u, high_v, high_w;
    integer both_off_u;
    integer sample_clks;
    integer expected_period_clks;
    integer period_samples;
    integer last_uh;
    integer rise_count;
    real    t_rise_a, t_rise_b;
    real    meas_period_ns;
    real    duty_meas_u, duty_meas_v, duty_meas_w;
    integer dead_ok_events;
    integer i;

    initial begin
        $dumpfile("tb_pwm.vcd");
        $dumpvars(0, tb_pwm);

        overlap_u = 0; overlap_v = 0; overlap_w = 0;
        high_u = 0; high_v = 0; high_w = 0;
        both_off_u = 0;
        sample_clks = 0;
        rise_count = 0;
        dead_ok_events = 0;
        last_uh = 0;

        expected_period_clks = CLK_HZ / FREQ_HZ; // center-aligned full period

        rst_n = 0;
        repeat (20) @(posedge clk);
        rst_n = 1;
        // Warm up past startup edge so period uses steady rising edges
        repeat (expected_period_clks * 2) @(posedge clk);

        if (!running) begin
            $display("FAIL: PWM not running (cfg_valid?)");
            $fatal(1);
        end
        if (fault_bad_freq || fault_bad_dead) begin
            $display("FAIL: unexpected config fault freq=%0d dead=%0d",
                     fault_bad_freq, fault_bad_dead);
            $fatal(1);
        end

        // Measure over several periods
        period_samples = expected_period_clks * 5;
        for (i = 0; i < period_samples; i = i + 1) begin
            @(posedge clk);
            sample_clks = sample_clks + 1;

            if (pwm_uh && pwm_ul) overlap_u = overlap_u + 1;
            if (pwm_vh && pwm_vl) overlap_v = overlap_v + 1;
            if (pwm_wh && pwm_wl) overlap_w = overlap_w + 1;

            if (pwm_uh) high_u = high_u + 1;
            if (pwm_vh) high_v = high_v + 1;
            if (pwm_wh) high_w = high_w + 1;
            if (!pwm_uh && !pwm_ul) both_off_u = both_off_u + 1;

            // Period from UH rising edges (skip first captured edge pair index 0->1
            // by recording edges 2 and 3 after warmup)
            if (pwm_uh && !last_uh) begin
                if (rise_count == 2)
                    t_rise_a = $realtime;
                else if (rise_count == 3)
                    t_rise_b = $realtime;
                rise_count = rise_count + 1;
            end
            last_uh = pwm_uh;
        end

        if (overlap_u != 0 || overlap_v != 0 || overlap_w != 0) begin
            $display("FAIL: complementary overlap U=%0d V=%0d W=%0d",
                     overlap_u, overlap_v, overlap_w);
            $fatal(1);
        end

        duty_meas_u = (100.0 * high_u) / sample_clks;
        duty_meas_v = (100.0 * high_v) / sample_clks;
        duty_meas_w = (100.0 * high_w) / sample_clks;

        // Allow slack for dead-time eating into high-side pulse
        if (duty_meas_u < 15.0 || duty_meas_u > 30.0) begin
            $display("FAIL: duty U meas=%0.2f%% expected~25%%", duty_meas_u);
            $fatal(1);
        end
        if (duty_meas_v < 40.0 || duty_meas_v > 55.0) begin
            $display("FAIL: duty V meas=%0.2f%% expected~50%%", duty_meas_v);
            $fatal(1);
        end
        if (duty_meas_w < 65.0 || duty_meas_w > 80.0) begin
            $display("FAIL: duty W meas=%0.2f%% expected~75%%", duty_meas_w);
            $fatal(1);
        end

        if (both_off_u < 10) begin
            $display("FAIL: expected dead-time both-off windows on U, got %0d clks",
                     both_off_u);
            $fatal(1);
        end

        if (rise_count < 4) begin
            $display("FAIL: not enough UH rising edges (%0d)", rise_count);
            $fatal(1);
        end

        meas_period_ns = t_rise_b - t_rise_a;
        // Expected period = 1e9/FREQ_HZ ns = 1e6 ns at 1 kHz; allow ±5%
        if (meas_period_ns < 950000.0 || meas_period_ns > 1050000.0) begin
            $display("FAIL: period meas=%0.1f ns expected~1e6 ns", meas_period_ns);
            $fatal(1);
        end

        // Spot-check: after UH falling, UL must stay low for dead_ticks
        begin : dead_check
            integer saw_fall;
            integer low_wait;
            integer dead_ticks_exp;
            saw_fall = 0;
            dead_ticks_exp = (DEAD_NS * CLK_HZ + 500_000_000) / 1_000_000_000;
            last_uh = pwm_uh;
            for (i = 0; i < expected_period_clks * 3; i = i + 1) begin
                @(posedge clk);
                if (last_uh && !pwm_uh) begin
                    saw_fall = 1;
                    low_wait = 0;
                    while (low_wait < dead_ticks_exp) begin
                        if (pwm_ul) begin
                            $display("FAIL: UL rose during dead-time after UH fall (t=%0d)",
                                     low_wait);
                            $fatal(1);
                        end
                        @(posedge clk);
                        low_wait = low_wait + 1;
                    end
                    dead_ok_events = dead_ok_events + 1;
                end
                last_uh = pwm_uh;
            end
            if (dead_ok_events < 1) begin
                $display("FAIL: no UH falling edge for dead-time check");
                $fatal(1);
            end
        end

        $display("PASS: tb_pwm duty U=%.2f%% V=%.2f%% W=%.2f%% period=%.0fns overlaps=0 dead_ok=%0d",
                 duty_meas_u, duty_meas_v, duty_meas_w, meas_period_ns, dead_ok_events);
        $finish;
    end

    // Watchdog
    initial begin
        #50_000_000;
        $display("FAIL: tb_pwm timeout");
        $fatal(1);
    end
endmodule
