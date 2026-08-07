// Tang Nano 20K top: SPI register file + 6-channel complementary PWM.
// FOC stubs are instantiated but not in the PWM path (PWM stays independently runnable).
`timescale 1ns / 1ps

module tangnano20k_top #(
    parameter integer CLK_HZ = 27_000_000
) (
    input  wire clk_27m,
    input  wire rst_n,          // active-low (KEY, pressed=0)
    // Complementary PWM outs
    output wire pwm_uh,
    output wire pwm_ul,
    output wire pwm_vh,
    output wire pwm_vl,
    output wire pwm_wh,
    output wire pwm_wl,
    // Optional status LED (active-low on Tang Nano 20K)
    output wire led_n,
    // SPI slave (MCU master later)
    input  wire spi_sclk,
    input  wire spi_cs_n,
    input  wire spi_mosi,
    output wire spi_miso
);

    wire        pwm_enable;
    wire [15:0] freq_hz;
    wire [15:0] deadtime_ns;
    wire [15:0] duty_u, duty_v, duty_w;
    wire        pwm_running;
    wire        fault_bad_freq, fault_bad_duty, fault_bad_dead;

    spi_regs #(.CLK_HZ(CLK_HZ)) u_spi (
        .clk(clk_27m),
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

    pwm_complementary #(.CLK_HZ(CLK_HZ)) u_pwm (
        .clk(clk_27m),
        .rst_n(rst_n),
        .enable(pwm_enable),
        .freq_hz(freq_hz),
        .deadtime_ns(deadtime_ns),
        .duty_u(duty_u),
        .duty_v(duty_v),
        .duty_w(duty_w),
        .pwm_uh(pwm_uh),
        .pwm_ul(pwm_ul),
        .pwm_vh(pwm_vh),
        .pwm_vl(pwm_vl),
        .pwm_wh(pwm_wh),
        .pwm_wl(pwm_wl),
        .fault_bad_freq(fault_bad_freq),
        .fault_bad_duty(fault_bad_duty),
        .fault_bad_dead(fault_bad_dead),
        .running(pwm_running)
    );

    // LED: lit (low) when PWM enabled and running
    assign led_n = ~(pwm_enable & pwm_running);

    // --- FOC stub shells (unused outputs; kept for later join) ---
    wire [15:0] stub_alpha, stub_beta;
    wire [15:0] stub_id, stub_iq;
    wire [15:0] stub_du, stub_dv, stub_dw;

    foc_clarke_stub u_clarke (
        .clk(clk_27m), .rst_n(rst_n), .enable(1'b0),
        .iu(16'sd0), .iv(16'sd0),
        .ialpha(stub_alpha), .ibeta(stub_beta)
    );
    foc_park_stub u_park (
        .clk(clk_27m), .rst_n(rst_n), .enable(1'b0),
        .ialpha(stub_alpha), .ibeta(stub_beta), .theta(16'd0),
        .id(stub_id), .iq(stub_iq)
    );
    foc_svpwm_stub u_svpwm (
        .clk(clk_27m), .rst_n(rst_n), .enable(1'b0),
        .valpha(16'sd0), .vbeta(16'sd0),
        .duty_u(stub_du), .duty_v(stub_dv), .duty_w(stub_dw)
    );

endmodule
