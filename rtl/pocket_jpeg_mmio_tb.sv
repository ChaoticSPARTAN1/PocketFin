// SPDX-License-Identifier: GPL-3.0-or-later
// Additional terms and permissions: ADDITIONAL-TERMS.md
`timescale 1ns/1ps
module pocket_jpeg_mmio_tb;
    reg clk=0;always #5 clk=~clk; // host 100 MHz
    reg jclk=0;always #20.345 jclk=~jclk; // decoder ~24.576 MHz
    reg reset=0,wr=0,rd=0;reg [4:0] addr=0;reg [31:0] wdata=0;
    wire [31:0] rdata;
    pocket_jpeg_mmio #(.INPUT_WORDS(16),.PIXEL_WORDS(16),.WATCHDOG_CYCLES(100000)) dut(
        .clk_i(clk),.reset_i(reset),.jpeg_clk_i(jclk),.reg_wr_i(wr),.reg_rd_i(rd),
        .reg_addr_i(addr),.reg_wdata_i(wdata),.reg_rdata_o(rdata));
    reg [7:0] input_bytes[0:32771];reg [15:0] pixels[0:36863];reg seen[0:36863];
    reg [1023:0] input_path,output_path;integer length,width=128,height=96,count=0,iteration,i,f;
    task write_reg(input [4:0] a,input [31:0] d);
        begin @(negedge clk);addr=a;wdata=d;wr=1;rd=0;@(negedge clk);wr=0;end
    endtask
    task read_reg(input [4:0] a,output [31:0] d);
        begin
            @(negedge clk);addr=a;wr=0;rd=1;
            @(negedge clk);rd=0;@(negedge clk);#1;d=rdata;
        end
    endtask
    // One host bus, interleave input and output to avoid deadlock on tiny FIFOs.
    integer offset,x,y,cycles,use_try=0;reg [31:0] status,value;reg last_sent;
    initial begin
        if(!$value$plusargs("INPUT=%s",input_path)||!$value$plusargs("OUTPUT=%s",output_path)||
           !$value$plusargs("LENGTH=%d",length)||length<4||length>32768)$fatal(1,"Bounded input missing");
        if(!$value$plusargs("WIDTH=%d",width))width=128;
        if(!$value$plusargs("HEIGHT=%d",height))height=96;
        if(!$value$plusargs("TRYPOP=%d",use_try))use_try=0;
        for(i=0;i<32772;i=i+1)input_bytes[i]=0;
        $readmemh(input_path,input_bytes,0,length-1);
        #1;reset=1;repeat(8)@(negedge clk);reset=0;
        read_reg(31,value);if(value!==32'h47454a50)$fatal(1,"No JPEG signature");
        write_reg(3,32'h00010001);write_reg(0,1);read_reg(6,value);
        if(value!==4)$fatal(1,"Bad geometry accepted");
        for(iteration=0;iteration<2;iteration=iteration+1)begin
            write_reg(0,0);write_reg(3,(height<<16)|width);write_reg(0,1);
            count=0;offset=0;cycles=0;last_sent=0;
            for(i=0;i<width*height;i=i+1)seen[i]=0;
            while(count<width*height)begin
                read_reg(0,status);cycles=cycles+1;
                if(status[5])$fatal(1,"JPEG MMIO fault at pixel %0d",count);
                // Try-pop runs regardless of status, proving empty reads are harmless.
                if((use_try||status[2])&&(cycles%13!=0))begin
                    read_reg(use_try?7:4,value);x=value[31:24];y=value[23:16];
                    if(use_try&&value===32'hffffffff)begin end else begin
                    if(x>=width||y>=height||seen[y*width+x])$fatal(1,"Bad/duplicate pixel coordinate");
                    if(iteration==1&&pixels[y*width+x]!==value[15:0])$fatal(1,"Second frame pixel differs after reset");
                    pixels[y*width+x]=value[15:0];seen[y*width+x]=1;count=count+1;
                    end
                end
                if(status[1]&&cycles%7!=0)begin
                    if(offset<length)begin
                        write_reg(1,{input_bytes[offset+3],input_bytes[offset+2],input_bytes[offset+1],input_bytes[offset]});
                        offset=offset+4;
                    end else if(!last_sent)begin write_reg(2,16);write_reg(1,0);last_sent=1;end
                end
                if(cycles>900000)$fatal(1,"JPEG host work bound exceeded");
            end
            read_reg(5,value);if(value!=width*height)$fatal(1,"Read count mismatch");
            read_reg(0,status);if(!status[3]||status[5])$fatal(1,"Frame completion missing");
            if(iteration==0)begin
                f=$fopen(output_path,"wb");if(!f)$fatal(1,"Cannot open private pixels");
                for(i=0;i<width*height;i=i+1)$fwrite(f,"%c%c",pixels[i][7:0],pixels[i][15:8]);
                $fclose(f);
            end
        end
        // Malformed frame: garbage marked as the end of the frame (the app always ends a
        // frame with the last flag). The progress watchdog must fault, then reset cleanly.
        // Input that stops without the last flag is a host stall and does not count; the
        // CPU's own progress timeout covers that case.
        write_reg(0,0);write_reg(0,1);repeat(10)@(negedge clk);write_reg(2,16);write_reg(1,0);
        wait(dut.fault_sync[1]);read_reg(0,status);if(!status[5])$fatal(1,"Watchdog fault hidden");
        write_reg(0,0);read_reg(6,value);if(value!==0)$fatal(1,"Reset did not clear fault");
        read_reg(5,value);if(value!==0)$fatal(1,"Reset did not clear output count");
        $display("PASS: dual-clock JPEG MMIO; two exact complete frames, tiny FIFO stalls, coordinate/count checks, bad geometry and malformed-frame watchdog/reset; pixels=%0d",count);
        $finish;
    end
    initial begin #120000000;$fatal(1,"Bounded JPEG MMIO test timed out");end
endmodule
