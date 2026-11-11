// SPDX-License-Identifier: GPL-3.0-or-later
// Additional terms and permissions: ADDITIONAL-TERMS.md
`timescale 1ns/1ps
module pocket_media_link_tb;
    parameter integer SI_FILTER=4;
    reg clk=0;always #20 clk=~clk;
    reg peer_clk=0;always #23 peer_clk=~peer_clk;
    reg reset_n=0,wr=0,rd=0;reg [4:0] addr=0;reg [31:0] wdata=0;
    wire [31:0] rdata;wire so,oe,sck,sckoe,irq;
    wire pso,poe,psck,psckoe,pready,pvalid;wire [7:0] pdata;
    reg peer_enable=0,peer_send=0,peer_receive=1;reg [7:0] peer_data=0;
    integer upstream=0;real guard_start,low_start;
    pocket_media_link #(.CLK_HZ(25000000),.FIFO_BYTES(16),.FAST_MAX(500),.SI_FILTER(SI_FILTER)) dut(
        .clk(clk),.reset_n(reset_n),.reg_wr(wr),.reg_rd(rd),.reg_addr(addr),.reg_wdata(wdata),.reg_rdata(rdata),
        .link_si_i(poe?pso:1'b1),.link_so_o(so),.link_so_oe(oe),.link_sck_i(1'b1),
        .link_sck_o(sck),.link_sck_oe(sckoe),.link_sd_i(1'b0),.link_sd_o(),.link_sd_oe(),.irq(irq));
    gb_link_stream #(.HALF_CYCLES(43),.QUIET_CYCLES(1024),.FIFO_DEPTH(16),.DROP_IDLE_RX(1)) peer(
        .clk(peer_clk),.rst(!reset_n),.enable_i(peer_enable),.master_i(1'b0),.resync_i(1'b0),
        .tx_data_i(peer_data),.tx_valid_i(peer_send),.tx_ready_o(pready),
        .rx_data_o(pdata),.rx_valid_o(pvalid),.rx_ready_i(peer_receive),.tx_level_o(),.rx_level_o(),
        .link_si_i(oe?so:1'b1),.link_clk_i(sckoe?sck:1'b1),
        .link_so_o(pso),.link_so_oe(poe),.link_clk_o(psck),.link_clk_oe(psckoe),.busy_o(),.timeout_o(),.overflow_o());
    always @(posedge peer_clk)if(pvalid&&peer_receive)begin
        if(pdata!==8'h7e)$fatal(1,"MMIO TX byte changed");upstream=upstream+1;
    end
    task write_reg(input [4:0] a,input [31:0] d);
        begin @(negedge clk);addr=a;wdata=d;wr=1;@(negedge clk);wr=0;end
    endtask
    task read_reg(input [4:0] a,input [31:0] expected);
        begin
            @(negedge clk);addr=a;rd=1;
            @(negedge clk);rd=0;
            @(negedge clk);#1;
            if(rdata!==expected)$fatal(1,"MMIO read mismatch at word %0d: %x",a,rdata);
        end
    endtask
    task send_peer(input [7:0] data);
        begin
            @(negedge peer_clk);while(!pready)@(negedge peer_clk);
            peer_data=data;peer_send=1;@(negedge peer_clk);peer_send=0;
        end
    endtask
    initial begin
        repeat(8)@(negedge clk);reset_n=1;
        read_reg(31,32'h4b4c4a50);read_reg(7,1562);read_reg(8,0); // word 7: slowest half-period (8 kHz)
        if(oe||sckoe)$fatal(1,"Disabled Pocket drives cable");
        peer_enable=1;repeat(2200)@(negedge peer_clk);
        write_reg(0,13); // enable + master + automatic idle polling
        wait(dut.active);repeat(30000)@(negedge clk);
        read_reg(5,0);if(irq)$fatal(1,"Polling idles created CPU interrupts");
        send_peer(8'h7e);wait(irq);read_reg(5,1);read_reg(2,32'h7e);
        repeat(5)@(negedge clk);if(irq)$fatal(1,"RX pop did not clear interrupt");
        write_reg(1,32'h7e);wait(upstream==1);
        read_reg(2,32'hffffffff);read_reg(6,8);write_reg(6,8);read_reg(6,0);
        // Faster clock: limits clamp, and bytes still cross in both directions.
        read_reg(3,48);write_reg(3,2);read_reg(3,8);write_reg(3,5000);read_reg(3,1562);
        write_reg(3,8);read_reg(3,8);
        write_reg(1,32'h7e);wait(upstream==2);
        send_peer(8'h7e);wait(irq);read_reg(5,1);read_reg(2,32'h7e);
        // Slower clocks. Up to FAST_MAX the shared engine runs them; above it the wide
        // counter does, after a fresh clock-high guard of two quiet intervals (2 ms).
        write_reg(3,400);read_reg(3,400);if(dut.use_slow)$fatal(1,"shared engine not used at 400");
        write_reg(1,32'h7e);wait(upstream==3);
        send_peer(8'h7e);wait(irq);read_reg(5,1);read_reg(2,32'h7e);
        write_reg(3,800);read_reg(3,800);wait(dut.use_slow); // changes between bytes
        guard_start=$realtime;@(negedge sck);
        if($realtime-guard_start<1990000.0)$fatal(1,"slow engine started inside the 2 ms guard: %0f ns",$realtime-guard_start);
        @(posedge sck);low_start=$realtime;@(negedge sck);
        if($realtime-low_start<31900.0||$realtime-low_start>32100.0)$fatal(1,"slow half-period is %0f ns, not 32000",$realtime-low_start);
        write_reg(1,32'h7e);wait(upstream==4);
        send_peer(8'h7e);wait(irq);read_reg(5,1);read_reg(2,32'h7e);
        read_reg(6,0);
        write_reg(3,8);read_reg(3,8);wait(!dut.use_slow);guard_start=$realtime;@(negedge sck);
        if($realtime-guard_start<1990000.0)$fatal(1,"shared engine restarted inside the 2 ms guard");
        write_reg(1,32'h7e);wait(upstream==5);
        send_peer(8'h7e);wait(irq);read_reg(5,1);read_reg(2,32'h7e);
        // Disable polling; fill TX faster than the cable to check overflow flag.
        write_reg(0,5);repeat(40)write_reg(1,32'h7e);
        @(negedge clk);addr=6;#1;if(!rdata[2])$fatal(1,"TX drop not reported");
        write_reg(0,2);repeat(4)@(negedge clk);read_reg(4,0);read_reg(5,0);read_reg(6,0);
        read_reg(3,48); // reset returns to the safe rate
        if(oe||sckoe||irq)$fatal(1,"Soft reset did not release/flush Pocket link");
        $display("PASS: Pocket MMIO byte transport signature, automatic idle polling without RX interrupts, bidirectional data at default, raised and slowed clock rates (shared engine and wide counter, 2 ms guard on change), rate clamps, slowest-rate word, FIFO/IRQ reads, W1C errors and reset/release");
        $finish;
    end
    initial begin #80000000;$fatal(1,"Bounded Pocket MMIO test timed out");end
endmodule
