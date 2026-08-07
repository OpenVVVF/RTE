// 256-entry Q1.15 Sine/Cosine Lookup Table for FPGA FOC Engine
// Input: angle [7:0] representing [0, 2pi) in 256 steps
// Output: sin_out [15:0], cos_out [15:0] in signed Q1.15 format (-32768 to +32767)

module sincos_lut (
    input  wire        clk,
    input  wire [7:0]  angle,
    output reg  signed [15:0] sin_out,
    output reg  signed [15:0] cos_out
);

    wire [7:0] cos_angle = angle + 8'd64; // Cosine is Sine shifted by 90 deg (64/256)

    function signed [15:0] get_sin_q15;
        input [7:0] idx;
        begin
            case (idx)
                8'h00: get_sin_q15 = 16'sh0000; 8'h01: get_sin_q15 = 16'sh0324; 8'h02: get_sin_q15 = 16'sh0647; 8'h03: get_sin_q15 = 16'sh096a;
                8'h04: get_sin_q15 = 16'sh0c8c; 8'h05: get_sin_q15 = 16'sh0fac; 8'h06: get_sin_q15 = 16'sh12cb; 8'h07: get_sin_q15 = 16'sh15e8;
                8'h08: get_sin_q15 = 16'sh1903; 8'h09: get_sin_q15 = 16'sh1c1c; 8'h0a: get_sin_q15 = 16'sh1f32; 8'h0b: get_sin_q15 = 16'sh2246;
                8'h0c: get_sin_q15 = 16'sh2557; 8'h0d: get_sin_q15 = 16'sh2865; 8'h0e: get_sin_q15 = 16'sh2b70; 8'h0f: get_sin_q15 = 16'sh2e77;
                8'h10: get_sin_q15 = 16'sh3179; 8'h11: get_sin_q15 = 16'sh3478; 8'h12: get_sin_q15 = 16'sh3773; 8'h13: get_sin_q15 = 16'sh3a6a;
                8'h14: get_sin_q15 = 16'sh3d5d; 8'h15: get_sin_q15 = 16'sh404d; 8'h16: get_sin_q15 = 16'sh4337; 8'h17: get_sin_q15 = 16'sh461c;
                8'h18: get_sin_q15 = 16'sh48fd; 8'h19: get_sin_q15 = 16'sh4bd7; 8'h1a: get_sin_q15 = 16'sh4eab; 8'h1b: get_sin_q15 = 16'sh517b;
                8'h1c: get_sin_q15 = 16'sh5443; 8'h1d: get_sin_q15 = 16'sh5706; 8'h1e: get_sin_q15 = 16'sh59c3; 8'h1f: get_sin_q15 = 16'sh5c79;
                8'h20: get_sin_q15 = 16'sh5f29; 8'h21: get_sin_q15 = 16'sh61d3; 8'h22: get_sin_q15 = 16'sh6475; 8'h23: get_sin_q15 = 16'sh670f;
                8'h24: get_sin_q15 = 16'sh69a2; 8'h25: get_sin_q15 = 16'sh6c2d; 8'h26: get_sin_q15 = 16'sh6eb0; 8'h27: get_sin_q15 = 16'sh712a;
                8'h28: get_sin_q15 = 16'sh739c; 8'h29: get_sin_q15 = 16'sh7604; 8'h2a: get_sin_q15 = 16'sh7861; 8'h2b: get_sin_q15 = 16'sh7ab5;
                8'h2c: get_sin_q15 = 16'sh7cfe; 8'h2d: get_sin_q15 = 16'sh7f3c; 8'h2e: get_sin_q15 = 16'sh7f6e; 8'h2f: get_sin_q15 = 16'sh7f95;
                8'h30: get_sin_q15 = 16'sh7fb1; 8'h31: get_sin_q15 = 16'sh7fc2; 8'h32: get_sin_q15 = 16'sh7fc8; 8'h33: get_sin_q15 = 16'sh7fc2;
                8'h34: get_sin_q15 = 16'sh7fb1; 8'h35: get_sin_q15 = 16'sh7f95; 8'h36: get_sin_q15 = 16'sh7f6e; 8'h37: get_sin_q15 = 16'sh7f3c;
                8'h38: get_sin_q15 = 16'sh7cfe; 8'h39: get_sin_q15 = 16'sh7ab5; 8'h3a: get_sin_q15 = 16'sh7861; 8'h3b: get_sin_q15 = 16'sh7604;
                8'h3c: get_sin_q15 = 16'sh739c; 8'h3d: get_sin_q15 = 16'sh712a; 8'h3e: get_sin_q15 = 16'sh6eb0; 8'h3f: get_sin_q15 = 16'sh6c2d;
                8'h40: get_sin_q15 = 16'sh69a2; 8'h41: get_sin_q15 = 16'sh670f; 8'h42: get_sin_q15 = 16'sh6475; 8'h43: get_sin_q15 = 16'sh61d3;
                8'h44: get_sin_q15 = 16'sh5f29; 8'h45: get_sin_q15 = 16'sh5c79; 8'h46: get_sin_q15 = 16'sh59c3; 8'h47: get_sin_q15 = 16'sh5706;
                8'h48: get_sin_q15 = 16'sh5443; 8'h49: get_sin_q15 = 16'sh517b; 8'h4a: get_sin_q15 = 16'sh4eab; 8'h4b: get_sin_q15 = 16'sh4bd7;
                8'h4c: get_sin_q15 = 16'sh48fd; 8'h4d: get_sin_q15 = 16'sh461c; 8'h4e: get_sin_q15 = 16'sh4337; 8'h4f: get_sin_q15 = 16'sh404d;
                8'h50: get_sin_q15 = 16'sh3d5d; 8'h51: get_sin_q15 = 16'sh3a6a; 8'h52: get_sin_q15 = 16'sh3773; 8'h53: get_sin_q15 = 16'sh3478;
                8'h54: get_sin_q15 = 16'sh3179; 8'h55: get_sin_q15 = 16'sh2e77; 8'h56: get_sin_q15 = 16'sh2b70; 8'h57: get_sin_q15 = 16'sh2865;
                8'h58: get_sin_q15 = 16'sh2557; 8'h59: get_sin_q15 = 16'sh2246; 8'h5a: get_sin_q15 = 16'sh1f32; 8'h5b: get_sin_q15 = 16'sh1c1c;
                8'h5c: get_sin_q15 = 16'sh1903; 8'h5d: get_sin_q15 = 16'sh15e8; 8'h5e: get_sin_q15 = 16'sh12cb; 8'h5f: get_sin_q15 = 16'sh0fac;
                8'h60: get_sin_q15 = 16'sh0c8c; 8'h61: get_sin_q15 = 16'sh096a; 8'h62: get_sin_q15 = 16'sh0647; 8'h63: get_sin_q15 = 16'sh0324;
                8'h64: get_sin_q15 = 16'sh0000; 8'h65: get_sin_q15 = -16'sh0324; 8'h66: get_sin_q15 = -16'sh0647; 8'h67: get_sin_q15 = -16'sh096a;
                8'h68: get_sin_q15 = -16'sh0c8c; 8'h69: get_sin_q15 = -16'sh0fac; 8'h6a: get_sin_q15 = -16'sh12cb; 8'h6b: get_sin_q15 = -16'sh15e8;
                8'h6c: get_sin_q15 = -16'sh1903; 8'h6d: get_sin_q15 = -16'sh1c1c; 8'h6e: get_sin_q15 = -16'sh1f32; 8'h6f: get_sin_q15 = -16'sh2246;
                8'h70: get_sin_q15 = -16'sh2557; 8'h71: get_sin_q15 = -16'sh2865; 8'h72: get_sin_q15 = -16'sh2b70; 8'h73: get_sin_q15 = -16'sh2e77;
                8'h74: get_sin_q15 = -16'sh3179; 8'h75: get_sin_q15 = -16'sh3478; 8'h76: get_sin_q15 = -16'sh3773; 8'h77: get_sin_q15 = -16'sh3a6a;
                8'h78: get_sin_q15 = -16'sh3d5d; 8'h79: get_sin_q15 = -16'sh404d; 8'h7a: get_sin_q15 = -16'sh4337; 8'h7b: get_sin_q15 = -16'sh461c;
                8'h7c: get_sin_q15 = -16'sh48fd; 8'h7d: get_sin_q15 = -16'sh4bd7; 8'h7e: get_sin_q15 = -16'sh4eab; 8'h7f: get_sin_q15 = -16'sh517b;
                8'h80: get_sin_q15 = -16'sh5443; 8'h81: get_sin_q15 = -16'sh5706; 8'h82: get_sin_q15 = -16'sh59c3; 8'h83: get_sin_q15 = -16'sh5c79;
                8'h84: get_sin_q15 = -16'sh5f29; 8'h85: get_sin_q15 = -16'sh61d3; 8'h86: get_sin_q15 = -16'sh6475; 8'h87: get_sin_q15 = -16'sh670f;
                8'h88: get_sin_q15 = -16'sh69a2; 8'h89: get_sin_q15 = -16'sh6c2d; 8'h8a: get_sin_q15 = -16'sh6eb0; 8'h8b: get_sin_q15 = -16'sh712a;
                8'h8c: get_sin_q15 = -16'sh739c; 8'h8d: get_sin_q15 = -16'sh7604; 8'h8e: get_sin_q15 = -16'sh7861; 8'h8f: get_sin_q15 = -16'sh7ab5;
                8'h90: get_sin_q15 = -16'sh7cfe; 8'h91: get_sin_q15 = -16'sh7f3c; 8'h92: get_sin_q15 = -16'sh7f6e; 8'h93: get_sin_q15 = -16'sh7f95;
                8'h94: get_sin_q15 = -16'sh7fb1; 8'h95: get_sin_q15 = -16'sh7fc2; 8'h96: get_sin_q15 = -16'sh7fc8; 8'h97: get_sin_q15 = -16'sh7fc2;
                8'h98: get_sin_q15 = -16'sh7fb1; 8'h99: get_sin_q15 = -16'sh7f95; 8'h9a: get_sin_q15 = -16'sh7f6e; 8'h9b: get_sin_q15 = -16'sh7f3c;
                8'h9c: get_sin_q15 = -16'sh7cfe; 8'h9d: get_sin_q15 = -16'sh7ab5; 8'h9e: get_sin_q15 = -16'sh7861; 8'h9f: get_sin_q15 = -16'sh7604;
                8'ha0: get_sin_q15 = -16'sh739c; 8'ha1: get_sin_q15 = -16'sh712a; 8'ha2: get_sin_q15 = -16'sh6eb0; 8'ha3: get_sin_q15 = -16'sh6c2d;
                8'ha4: get_sin_q15 = -16'sh69a2; 8'ha5: get_sin_q15 = -16'sh670f; 8'ha6: get_sin_q15 = -16'sh6475; 8'ha7: get_sin_q15 = -16'sh61d3;
                8'ha8: get_sin_q15 = -16'sh5f29; 8'ha9: get_sin_q15 = -16'sh5c79; 8'haa: get_sin_q15 = -16'sh59c3; 8'hab: get_sin_q15 = -16'sh5706;
                8'hac: get_sin_q15 = -16'sh5443; 8'had: get_sin_q15 = -16'sh517b; 8'hae: get_sin_q15 = -16'sh4eab; 8'haf: get_sin_q15 = -16'sh4bd7;
                8'hb0: get_sin_q15 = -16'sh48fd; 8'hb1: get_sin_q15 = -16'sh461c; 8'hb2: get_sin_q15 = -16'sh4337; 8'hb3: get_sin_q15 = -16'sh404d;
                8'hb4: get_sin_q15 = -16'sh3d5d; 8'hb5: get_sin_q15 = -16'sh3a6a; 8'hb6: get_sin_q15 = -16'sh3773; 8'hb7: get_sin_q15 = -16'sh3478;
                8'hb8: get_sin_q15 = -16'sh3179; 8'hb9: get_sin_q15 = -16'sh2e77; 8'hba: get_sin_q15 = -16'sh2b70; 8'hbb: get_sin_q15 = -16'sh2865;
                8'hbc: get_sin_q15 = -16'sh2557; 8'hbd: get_sin_q15 = -16'sh2246; 8'hbe: get_sin_q15 = -16'sh1f32; 8'hbf: get_sin_q15 = -16'sh1c1c;
                8'hc0: get_sin_q15 = -16'sh1903; 8'hc1: get_sin_q15 = -16'sh15e8; 8'hc2: get_sin_q15 = -16'sh12cb; 8'hc3: get_sin_q15 = -16'sh0fac;
                8'hc4: get_sin_q15 = -16'sh0c8c; 8'hc5: get_sin_q15 = -16'sh096a; 8'hc6: get_sin_q15 = -16'sh0647; 8'hc7: get_sin_q15 = -16'sh0324;
                8'hc8: get_sin_q15 = 16'sh0000; 8'hcb: get_sin_q15 = 16'sh096a; 8'hcc: get_sin_q15 = 16'sh0c8c; 8'hcd: get_sin_q15 = 16'sh0fac;
                8'hce: get_sin_q15 = 16'sh12cb; 8'hcf: get_sin_q15 = 16'sh15e8; 8'hd0: get_sin_q15 = 16'sh1903; 8'hd1: get_sin_q15 = 16'sh1c1c;
                8'hd2: get_sin_q15 = 16'sh1f32; 8'hd3: get_sin_q15 = 16'sh2246; 8'hd4: get_sin_q15 = 16'sh2557; 8'hd5: get_sin_q15 = 16'sh2865;
                8'hd6: get_sin_q15 = 16'sh2b70; 8'hd7: get_sin_q15 = 16'sh2e77; 8'hd8: get_sin_q15 = 16'sh3179; 8'hd9: get_sin_q15 = 16'sh3478;
                8'hda: get_sin_q15 = 16'sh3773; 8'hdb: get_sin_q15 = 16'sh3a6a; 8'hdc: get_sin_q15 = 16'sh3d5d; 8'hdd: get_sin_q15 = 16'sh404d;
                8'hde: get_sin_q15 = 16'sh4337; 8'hdf: get_sin_q15 = 16'sh461c; 8'he0: get_sin_q15 = 16'sh48fd; 8'he1: get_sin_q15 = 16'sh4bd7;
                8'he2: get_sin_q15 = 16'sh4eab; 8'he3: get_sin_q15 = 16'sh517b; 8'he4: get_sin_q15 = 16'sh5443; 8'he5: get_sin_q15 = 16'sh5706;
                8'he6: get_sin_q15 = 16'sh59c3; 8'he7: get_sin_q15 = 16'sh5c79; 8'he8: get_sin_q15 = 16'sh5f29; 8'he9: get_sin_q15 = 16'sh61d3;
                8'hea: get_sin_q15 = 16'sh6475; 8'heb: get_sin_q15 = 16'sh670f; 8'hec: get_sin_q15 = 16'sh69a2; 8'hed: get_sin_q15 = 16'sh6c2d;
                8'hee: get_sin_q15 = 16'sh6eb0; 8'hef: get_sin_q15 = 16'sh712a; 8'hf0: get_sin_q15 = 16'sh739c; 8'hf1: get_sin_q15 = 16'sh7604;
                8'hf2: get_sin_q15 = 16'sh7861; 8'hf3: get_sin_q15 = 16'sh7ab5; 8'hf4: get_sin_q15 = 16'sh7cfe; 8'hf5: get_sin_q15 = 16'sh7f3c;
                8'hf6: get_sin_q15 = 16'sh7f6e; 8'hf7: get_sin_q15 = 16'sh7f95; 8'hf8: get_sin_q15 = 16'sh7fb1; 8'hf9: get_sin_q15 = 16'sh7fc2;
                8'hfa: get_sin_q15 = 16'sh7fc8; 8'hfb: get_sin_q15 = 16'sh7fc2; 8'hfc: get_sin_q15 = 16'sh7fb1; 8'hfd: get_sin_q15 = 16'sh7f95;
                8'hfe: get_sin_q15 = 16'sh7f6e; 8'hff: get_sin_q15 = 16'sh7f3c;
                default: get_sin_q15 = 16'sh0000;
            endcase
        end
    endfunction

    always @(posedge clk) begin
        sin_out <= get_sin_q15(angle);
        cos_out <= get_sin_q15(cos_angle);
    end

endmodule
