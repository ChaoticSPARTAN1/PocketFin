// SPDX-License-Identifier: GPL-3.0-or-later
// Additional terms and permissions: ADDITIONAL-TERMS.md
// Two-clock RAM queue. Gray pointers cross through two-flop synchronizers;
// board constraints must bound Gray bus skew and qualify the clock domains.
// Reset asserts asynchronously in BOTH domains; release is synchronized locally.
module async_word_fifo #(parameter integer DEPTH=1024,parameter integer WIDTH=16)(
    input wire reset_i,
    input wire write_clk_i,input wire [WIDTH-1:0] write_data_i,input wire write_valid_i,
    output wire write_ready_o,
    input wire read_clk_i,output reg [WIDTH-1:0] read_data_o,output reg read_valid_o,
    input wire read_ready_i
);
    localparam integer AW=$clog2(DEPTH);
    reg [WIDTH-1:0] memory[0:DEPTH-1];
    reg [AW:0] wb,wg,rb,rg;
    (* async_reg="true" *) reg [AW:0] rg_w1,rg_w2,wg_r1,wg_r2;
    (* async_reg="true" *) reg [1:0] wr_reset,rd_reset;
    reg full;
    always @(posedge write_clk_i or posedge reset_i)
        if(reset_i)wr_reset<=3;else wr_reset<={wr_reset[0],1'b0};
    always @(posedge read_clk_i or posedge reset_i)
        if(reset_i)rd_reset<=3;else rd_reset<={rd_reset[0],1'b0};
    assign write_ready_o=!reset_i&&!wr_reset[1]&&!full;
    wire push=write_valid_i&&write_ready_o;
    wire [AW:0] wb_next=wb+push;
    wire [AW:0] wg_next=(wb_next>>1)^wb_next;
    wire empty=(rg==wg_r2);
    wire load=(!read_valid_o||read_ready_i)&&!empty;
    wire [AW:0] rb_next=rb+load;
    wire [AW:0] rg_next=(rb_next>>1)^rb_next;
    always @(posedge write_clk_i or posedge reset_i)begin
        if(reset_i)begin wb<=0;wg<=0;full<=0;rg_w1<=0;rg_w2<=0;end
        else if(wr_reset[1])begin wb<=0;wg<=0;full<=0;rg_w1<=0;rg_w2<=0;end
        else begin
            rg_w1<=rg;rg_w2<=rg_w1;
            if(push)memory[wb[AW-1:0]]<=write_data_i;
            wb<=wb_next;wg<=wg_next;
            full<=(wg_next=={~rg_w2[AW:AW-1],rg_w2[AW-2:0]});
        end
    end
    always @(posedge read_clk_i or posedge reset_i)begin
        if(reset_i)begin rb<=0;rg<=0;wg_r1<=0;wg_r2<=0;read_data_o<=0;read_valid_o<=0;end
        else if(rd_reset[1])begin rb<=0;rg<=0;wg_r1<=0;wg_r2<=0;read_data_o<=0;read_valid_o<=0;end
        else begin
            wg_r1<=wg;wg_r2<=wg_r1;
            if(load)begin read_data_o<=memory[rb[AW-1:0]];read_valid_o<=1;end
            else if(read_ready_i)read_valid_o<=0;
            rb<=rb_next;rg<=rg_next;
        end
    end
`ifndef SYNTHESIS
    initial if(DEPTH<4||(DEPTH&(DEPTH-1))!=0)$fatal(1,"Async depth must be a power of two >=4");
`endif
endmodule
