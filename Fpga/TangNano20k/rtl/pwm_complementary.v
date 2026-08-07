// Complementary PWM with dead-time (center-aligned), TIM1 electrical intent.
// High-side ideal: CNT < CCR (PWM mode 1). Low-side = complementary + dead-time.
`timescale 1ns / 1ps

module pwm_complementary #(
    parameter integer CLK_HZ    = 27_000_000,
    parameter integer COUNTER_W = 16
) (
    input  wire                    clk,
    input  wire                    rst_n,
    input  wire                    enable,
    input  wire [15:0]             freq_hz,
    input  wire [15:0]             deadtime_ns,
    input  wire [15:0]             duty_u,   // 0..10000 = 0..100.00%
    input  wire [15:0]             duty_v,
    input  wire [15:0]             duty_w,
    output wire                    pwm_uh,
    output wire                    pwm_ul,
    output wire                    pwm_vh,
    output wire                    pwm_vl,
    output wire                    pwm_wh,
    output wire                    pwm_wl,
    output reg                     fault_bad_freq,
    output reg                     fault_bad_duty,
    output reg                     fault_bad_dead,
    output wire                    running
);

    localparam integer MAX_ARR = (1 << COUNTER_W) - 1;

    reg [COUNTER_W-1:0] arr;
    reg [COUNTER_W-1:0] cmp_u, cmp_v, cmp_w;
    reg [COUNTER_W-1:0] dead_ticks;
    reg [COUNTER_W-1:0] cnt;
    reg                 dir_up;
    reg                 cfg_valid;

    reg [63:0] half_period;
    reg [63:0] dead_calc;
    reg [15:0] du, dv, dw;

    always @(*) begin
        du = (duty_u > 16'd10000) ? 16'd10000 : duty_u;
        dv = (duty_v > 16'd10000) ? 16'd10000 : duty_v;
        dw = (duty_w > 16'd10000) ? 16'd10000 : duty_w;

        if (freq_hz == 16'd0)
            half_period = 64'd0;
        else
            half_period = CLK_HZ / (2 * freq_hz);

        // 64-bit math avoids 32-bit overflow (ns * Hz)
        dead_calc = (deadtime_ns * 64'd1 * CLK_HZ + 64'd500_000_000) / 64'd1_000_000_000;
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            arr            <= {COUNTER_W{1'b0}};
            cmp_u          <= {COUNTER_W{1'b0}};
            cmp_v          <= {COUNTER_W{1'b0}};
            cmp_w          <= {COUNTER_W{1'b0}};
            dead_ticks     <= {COUNTER_W{1'b0}};
            cfg_valid      <= 1'b0;
            fault_bad_freq <= 1'b0;
            fault_bad_duty <= 1'b0;
            fault_bad_dead <= 1'b0;
        end else begin
            if (duty_u > 16'd10000 || duty_v > 16'd10000 || duty_w > 16'd10000)
                fault_bad_duty <= 1'b1;

            if (freq_hz == 16'd0 || half_period < 32'd2 || half_period > MAX_ARR) begin
                fault_bad_freq <= 1'b1;
                cfg_valid      <= 1'b0;
            end else begin
                arr   <= half_period[COUNTER_W-1:0];
                cmp_u <= (du * half_period) / 32'd10000;
                cmp_v <= (dv * half_period) / 32'd10000;
                cmp_w <= (dw * half_period) / 32'd10000;

                if (dead_calc >= half_period) begin
                    fault_bad_dead <= 1'b1;
                    dead_ticks     <= (half_period > 0) ?
                                      (half_period[COUNTER_W-1:0] - 1'b1) :
                                      {COUNTER_W{1'b0}};
                end else begin
                    dead_ticks <= dead_calc[COUNTER_W-1:0];
                end
                cfg_valid <= 1'b1;
            end
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cnt    <= {COUNTER_W{1'b0}};
            dir_up <= 1'b1;
        end else if (!(enable && cfg_valid)) begin
            cnt    <= {COUNTER_W{1'b0}};
            dir_up <= 1'b1;
        end else begin
            if (dir_up) begin
                if (cnt >= arr) begin
                    dir_up <= 1'b0;
                    if (cnt > 0)
                        cnt <= cnt - 1'b1;
                end else
                    cnt <= cnt + 1'b1;
            end else begin
                if (cnt == {COUNTER_W{1'b0}}) begin
                    dir_up <= 1'b1;
                    cnt    <= (arr > 0) ? {{(COUNTER_W-1){1'b0}}, 1'b1} : {COUNTER_W{1'b0}};
                end else
                    cnt <= cnt - 1'b1;
            end
        end
    end

    assign running = enable && cfg_valid;

    wire ideal_u = (cnt < cmp_u);
    wire ideal_v = (cnt < cmp_v);
    wire ideal_w = (cnt < cmp_w);

    deadtime_pair #(.W(COUNTER_W)) u_pair (
        .clk(clk), .rst_n(rst_n), .enable(running),
        .dead_ticks(dead_ticks), .ideal_high(ideal_u),
        .out_high(pwm_uh), .out_low(pwm_ul)
    );
    deadtime_pair #(.W(COUNTER_W)) v_pair (
        .clk(clk), .rst_n(rst_n), .enable(running),
        .dead_ticks(dead_ticks), .ideal_high(ideal_v),
        .out_high(pwm_vh), .out_low(pwm_vl)
    );
    deadtime_pair #(.W(COUNTER_W)) w_pair (
        .clk(clk), .rst_n(rst_n), .enable(running),
        .dead_ticks(dead_ticks), .ideal_high(ideal_w),
        .out_high(pwm_wh), .out_low(pwm_wl)
    );

endmodule
