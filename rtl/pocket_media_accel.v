// SPDX-License-Identifier: GPL-3.0-or-later
// Additional terms and permissions: ADDITIONAL-TERMS.md
// Shared 0x4D MMIO window: byte-link at +0x00, JPEG at +0x80.
// Requires six address bits on the parent register bus (byte address [7:2]).
module pocket_media_accel #(
    parameter integer CLK_HZ=100000000,parameter integer SCK_HZ=256000
)(
    input wire clk,input wire reset_n,input wire jpeg_clk_i,
    input wire reg_wr,input wire reg_rd,input wire [5:0] reg_addr,input wire [31:0] reg_wdata,
    output wire [31:0] reg_rdata,
    input wire link_si_i,output wire link_so_o,output wire link_so_oe,
    input wire link_sck_i,output wire link_sck_o,output wire link_sck_oe,
    input wire link_sd_i,output wire link_sd_o,output wire link_sd_oe,
    output wire irq
);
    wire [31:0] link_data,jpeg_data;
    assign reg_rdata=reg_addr[5]?jpeg_data:link_data;
    pocket_media_link #(.CLK_HZ(CLK_HZ),.SCK_HZ(SCK_HZ)) link(
        .clk(clk),.reset_n(reset_n),.reg_wr(reg_wr&&!reg_addr[5]),.reg_rd(reg_rd&&!reg_addr[5]),
        .reg_addr(reg_addr[4:0]),.reg_wdata(reg_wdata),.reg_rdata(link_data),
        .link_si_i(link_si_i),.link_so_o(link_so_o),.link_so_oe(link_so_oe),
        .link_sck_i(link_sck_i),.link_sck_o(link_sck_o),.link_sck_oe(link_sck_oe),
        .link_sd_i(link_sd_i),.link_sd_o(link_sd_o),.link_sd_oe(link_sd_oe),.irq(irq));
    pocket_jpeg_mmio jpeg(
        .clk_i(clk),.reset_i(!reset_n),.jpeg_clk_i(jpeg_clk_i),
        .reg_wr_i(reg_wr&&reg_addr[5]),.reg_rd_i(reg_rd&&reg_addr[5]),
        .reg_addr_i(reg_addr[4:0]),.reg_wdata_i(reg_wdata),.reg_rdata_o(jpeg_data));
endmodule
