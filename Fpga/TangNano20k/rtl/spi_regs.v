// SPI Mode-0 slave + 16-bit register file (see docs/register_map.md).
`timescale 1ns / 1ps

module spi_regs #(
    parameter integer CLK_HZ = 27_000_000
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        spi_sclk,
    input  wire        spi_cs_n,
    input  wire        spi_mosi,
    output wire        spi_miso,
    output wire        pwm_enable,
    output wire [15:0] freq_hz,
    output wire [15:0] deadtime_ns,
    output wire [15:0] duty_u,
    output wire [15:0] duty_v,
    output wire [15:0] duty_w,
    input  wire        pwm_running,
    input  wire        fault_bad_freq,
    input  wire        fault_bad_duty,
    input  wire        fault_bad_dead
);

    localparam [6:0] A_MAGIC   = 7'h00;
    localparam [6:0] A_VERSION = 7'h01;
    localparam [6:0] A_STATUS  = 7'h02;
    localparam [6:0] A_FAULT   = 7'h03;
    localparam [6:0] A_CTRL    = 7'h04;
    localparam [6:0] A_FREQ    = 7'h05;
    localparam [6:0] A_DEAD    = 7'h06;
    localparam [6:0] A_DUTY_U  = 7'h07;
    localparam [6:0] A_DUTY_V  = 7'h08;
    localparam [6:0] A_DUTY_W  = 7'h09;
    localparam [6:0] A_SCRATCH = 7'h0A;
    localparam [6:0] A_CLK_LO  = 7'h0B;
    localparam [6:0] A_CLK_HI  = 7'h0C;

    reg [15:0] reg_ctrl;
    reg [15:0] reg_freq;
    reg [15:0] reg_dead;
    reg [15:0] reg_duty_u;
    reg [15:0] reg_duty_v;
    reg [15:0] reg_duty_w;
    reg [15:0] reg_scratch;
    reg [15:0] reg_fault;

    assign pwm_enable  = reg_ctrl[0];
    assign freq_hz     = reg_freq;
    assign deadtime_ns = reg_dead;
    assign duty_u      = reg_duty_u;
    assign duty_v      = reg_duty_v;
    assign duty_w      = reg_duty_w;

    wire [15:0] status_val = {13'b0, pwm_running, |reg_fault, reg_ctrl[0]};

    // Synchronize SPI into clk domain
    reg [2:0] sclk_q, csn_q;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sclk_q <= 3'b000;
            csn_q  <= 3'b111;
        end else begin
            sclk_q <= {sclk_q[1:0], spi_sclk};
            csn_q  <= {csn_q[1:0],  spi_cs_n};
        end
    end

    wire sclk_rise =  sclk_q[1] & ~sclk_q[2];
    wire sclk_fall = ~sclk_q[1] &  sclk_q[2];
    wire csn_fall  = ~csn_q[1]  &  csn_q[2];
    wire csn_act   = ~csn_q[1];

    reg [4:0]  bit_idx;
    reg        cmd_read;
    reg [6:0]  cmd_addr;
    reg [7:0]  addr_shift;
    reg [15:0] data_shift_in;
    reg [15:0] data_shift_out;
    reg        miso_bit;
    reg        miso_oe;

    wire [7:0] addr_byte_next = {addr_shift[6:0], spi_mosi};
    wire [15:0] data_word_next = {data_shift_in[14:0], spi_mosi};

    assign spi_miso = (miso_oe && csn_act) ? miso_bit : 1'bz;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            reg_ctrl       <= 16'h0000;
            reg_freq       <= 16'd10000;
            reg_dead       <= 16'd1000;
            reg_duty_u     <= 16'd5000;
            reg_duty_v     <= 16'd5000;
            reg_duty_w     <= 16'd5000;
            reg_scratch    <= 16'h0000;
            reg_fault      <= 16'h0000;
            bit_idx        <= 5'd0;
            cmd_read       <= 1'b0;
            cmd_addr       <= 7'd0;
            addr_shift     <= 8'd0;
            data_shift_in  <= 16'd0;
            data_shift_out <= 16'd0;
            miso_bit       <= 1'b0;
            miso_oe        <= 1'b0;
        end else begin
            if (fault_bad_freq) reg_fault[0] <= 1'b1;
            if (fault_bad_duty) reg_fault[1] <= 1'b1;
            if (fault_bad_dead) reg_fault[2] <= 1'b1;

            if (csn_fall) begin
                bit_idx    <= 5'd0;
                addr_shift <= 8'd0;
                data_shift_in <= 16'd0;
                miso_oe    <= 1'b0;
                miso_bit   <= 1'b0;
            end else if (!csn_act) begin
                miso_oe <= 1'b0;
            end else begin
                if (sclk_rise) begin
                    if (bit_idx < 5'd8) begin
                        addr_shift <= addr_byte_next;
                        if (bit_idx == 5'd7) begin
                            cmd_read <= addr_byte_next[7];
                            cmd_addr <= addr_byte_next[6:0];
                            // Preload read data (STATUS/FAULT see live values)
                            case (addr_byte_next[6:0])
                                A_MAGIC:   data_shift_out <= 16'h544E;
                                A_VERSION: data_shift_out <= 16'h0001;
                                A_STATUS:  data_shift_out <= status_val;
                                A_FAULT:   data_shift_out <= reg_fault;
                                A_CTRL:    data_shift_out <= reg_ctrl;
                                A_FREQ:    data_shift_out <= reg_freq;
                                A_DEAD:    data_shift_out <= reg_dead;
                                A_DUTY_U:  data_shift_out <= reg_duty_u;
                                A_DUTY_V:  data_shift_out <= reg_duty_v;
                                A_DUTY_W:  data_shift_out <= reg_duty_w;
                                A_SCRATCH: data_shift_out <= reg_scratch;
                                A_CLK_LO:  data_shift_out <= CLK_HZ[15:0];
                                A_CLK_HI:  data_shift_out <= CLK_HZ[31:16];
                                default:   data_shift_out <= 16'h0000;
                            endcase
                        end
                    end else begin
                        data_shift_in <= data_word_next;
                        if (bit_idx == 5'd23 && !cmd_read) begin
                            case (cmd_addr)
                                A_CTRL:    reg_ctrl    <= data_word_next;
                                A_FREQ:    reg_freq    <= data_word_next;
                                A_DEAD:    reg_dead    <= data_word_next;
                                A_DUTY_U:  reg_duty_u  <= data_word_next;
                                A_DUTY_V:  reg_duty_v  <= data_word_next;
                                A_DUTY_W:  reg_duty_w  <= data_word_next;
                                A_SCRATCH: reg_scratch <= data_word_next;
                                A_FAULT:   reg_fault   <= reg_fault & ~data_word_next;
                                default: ;
                            endcase
                        end
                    end
                    bit_idx <= bit_idx + 1'b1;
                end

                if (sclk_fall) begin
                    if (bit_idx >= 5'd8 && bit_idx < 5'd24 && cmd_read) begin
                        miso_oe        <= 1'b1;
                        miso_bit       <= data_shift_out[15];
                        data_shift_out <= {data_shift_out[14:0], 1'b0};
                    end else begin
                        miso_oe  <= 1'b0;
                        miso_bit <= 1'b0;
                    end
                end
            end
        end
    end

endmodule
