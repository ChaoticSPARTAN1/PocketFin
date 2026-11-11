// SPDX-License-Identifier: GPL-3.0-or-later
// Additional terms and permissions: ADDITIONAL-TERMS.md
// CPU binding around the separately licensed Apache-2.0 core_jpeg.
// clk_i = CPU, jpeg_clk_i = existing Pocket 24.576 MHz video PLL output.
// Word regs: 0 CTRL enable/status, 1 encoded DATA write, 2 META(strb[3:0],last[4]),
// 3 expected width/height, 4 PIXEL read/pop {x8,y8,RGB565}, 5 pixels read,
// 6 faults, 7 PIXEL try-pop (same data; all-ones when empty, no fault, so the
// CPU needs one bus read per pixel), 30 decoder revision, 31 "PJEG" signature.
// CTRL0 resets/flushes between EVERY frame.
// Input can contain up to the bounded PJAV video payload; CPU owns that bound.
module pocket_jpeg_mmio #(
    parameter integer INPUT_WORDS=256,parameter integer PIXEL_WORDS=256,
    parameter integer WATCHDOG_CYCLES=5000000
)(
    input wire clk_i,input wire reset_i,input wire jpeg_clk_i,
    input wire reg_wr_i,input wire reg_rd_i,input wire [4:0] reg_addr_i,
    input wire [31:0] reg_wdata_i,output reg [31:0] reg_rdata_o
);
    reg enable_q;
    reg [31:0] geometry_q;
    reg [4:0] meta_q;
    reg [4:0] cpu_fault;
    reg [15:0] read_pixels;
    reg [31:0] pixel_snapshot;
    wire fifo_reset=reset_i||!enable_q;
    wire valid_geometry=(geometry_q[15:0]==128||geometry_q[15:0]==160||geometry_q[15:0]==256)&&
        (geometry_q[31:16]==96||geometry_q[31:16]==128||geometry_q[31:16]==144);
    wire [15:0] pixel_count=geometry_q[8:0]*geometry_q[23:16];
    wire input_ready,input_valid,input_accept;
    wire [36:0] input_data;
    wire input_write=reg_wr_i&&reg_addr_i==1&&enable_q;
    wire output_valid,output_ready;
    wire [31:0] output_data;
    wire pixel_try=reg_rd_i&&reg_addr_i==7;
    wire pixel_read=(reg_rd_i&&reg_addr_i==4)||pixel_try;
    reg [1:0] jpeg_reset_q;
    (* async_reg="true" *) reg [1:0] fault_sync,done_sync,idle_sync;
    reg [1:0] jpeg_fault;
    reg jpeg_done;
    wire core_idle;
    assign output_ready=pixel_read&&output_valid;
    always @(posedge jpeg_clk_i or posedge fifo_reset)
        if(fifo_reset)jpeg_reset_q<=3;else jpeg_reset_q<={jpeg_reset_q[0],1'b0};
    always @(posedge clk_i or posedge fifo_reset)
        if(fifo_reset)begin fault_sync<=0;done_sync<=0;idle_sync<=0;end
        else begin
            fault_sync<={fault_sync[0],|jpeg_fault};
            done_sync<={done_sync[0],jpeg_done};
            idle_sync<={idle_sync[0],core_idle};
        end
    async_word_fifo #(.DEPTH(INPUT_WORDS),.WIDTH(37)) input_queue(
        .reset_i(fifo_reset),.write_clk_i(clk_i),.write_data_i({meta_q,reg_wdata_i}),
        .write_valid_i(input_write),.write_ready_o(input_ready),
        .read_clk_i(jpeg_clk_i),.read_data_o(input_data),.read_valid_o(input_valid),.read_ready_i(input_accept));
    wire pixel_valid,pixel_accept;
    wire [15:0] width,height,x,y;
    wire [7:0] r,g,b;
    wire output_queue_ready;
    wire dimensions_ok=(width==geometry_q[15:0])&&(height==geometry_q[31:16])&&x<width&&y<height;
    // After a complete picture the core can emit a trailing MCU row (seen on a real frame: 16 MCUs at
    // y=144 after 36,864 correct pixels). It is drained and dropped, never queued or counted as a fault.
    assign pixel_accept=(jpeg_done||!dimensions_ok||output_queue_ready)&&!(|jpeg_fault);
    async_word_fifo #(.DEPTH(PIXEL_WORDS),.WIDTH(32)) output_queue(
        .reset_i(fifo_reset),.write_clk_i(jpeg_clk_i),
        .write_data_i({x[7:0],y[7:0],r[7:3],g[7:2],b[7:3]}),
        .write_valid_i(pixel_valid&&dimensions_ok&&!jpeg_done&&!(|jpeg_fault)),.write_ready_o(output_queue_ready),
        .read_clk_i(clk_i),.read_data_o(output_data),.read_valid_o(output_valid),.read_ready_i(output_ready));
    wire core_input_accept;
    assign input_accept=core_input_accept&&!(|jpeg_fault)&&!jpeg_reset_q[1];
    jpeg_core #(.SUPPORT_WRITABLE_DHT(0)) decoder(
        .clk_i(jpeg_clk_i),.rst_i(jpeg_reset_q[1]),
        .inport_valid_i(input_valid&&!(|jpeg_fault)&&!jpeg_reset_q[1]),.inport_data_i(input_data[31:0]),
        .inport_strb_i(input_data[35:32]),.inport_last_i(input_data[36]),.inport_accept_o(core_input_accept),
        .outport_valid_o(pixel_valid),.outport_accept_i(pixel_accept),
        .outport_width_o(width),.outport_height_o(height),.outport_pixel_x_o(x),.outport_pixel_y_o(y),
        .outport_pixel_r_o(r),.outport_pixel_g_o(g),.outport_pixel_b_o(b),.idle_o(core_idle));
    localparam integer WW=$clog2(WATCHDOG_CYCLES+1);
    reg [WW-1:0] watchdog;
    reg started,last_seen;
    // The watchdog catches a decoder that is stuck on a malformed frame. It must not
    // count time the decoder spends waiting on the CPU (output queue full, or more input
    // still to come): the CPU drains pixels over the bus in several passes and may be
    // busy elsewhere between them, which tripped this on valid frames on hardware.
    // Any progress restarts it.
    wire progress=(input_valid&&input_accept)||(pixel_valid&&pixel_accept);
    wire host_stall=(pixel_valid&&dimensions_ok&&!output_queue_ready)||(!input_valid&&!last_seen);
    reg [15:0] decoded_pixels;
    always @(posedge jpeg_clk_i or posedge fifo_reset)begin
        if(fifo_reset)begin jpeg_fault<=0;jpeg_done<=0;watchdog<=0;started<=0;last_seen<=0;decoded_pixels<=0;end
        else if(jpeg_reset_q[1])begin jpeg_fault<=0;jpeg_done<=0;watchdog<=0;started<=0;last_seen<=0;decoded_pixels<=0;end
        else begin
            if(input_valid&&input_accept)begin started<=1;if(input_data[36])last_seen<=1;end
            if(started&&!jpeg_done&&!(|jpeg_fault))begin
                if(progress||host_stall)watchdog<=0;
                else if(watchdog==WATCHDOG_CYCLES-1)jpeg_fault[1]<=1;
                else watchdog<=watchdog+1'b1;
            end
            if(pixel_valid&&pixel_accept&&!jpeg_done)begin
                if(!dimensions_ok||decoded_pixels>=pixel_count)jpeg_fault[0]<=1;
                else begin
                    decoded_pixels<=decoded_pixels+1'b1;
                    if(decoded_pixels==pixel_count-1)jpeg_done<=1;
                end
            end
        end
    end
    always @(posedge clk_i)begin
        if(reset_i)begin
            enable_q<=0;geometry_q<={16'd96,16'd128};meta_q<=15;cpu_fault<=0;read_pixels<=0;pixel_snapshot<=32'hffffffff;
        end else begin
            if(pixel_read)pixel_snapshot<=output_valid?output_data:32'hffffffff;
            if(input_write&&!input_ready)cpu_fault[0]<=1;
            if(pixel_read&&!pixel_try&&!output_valid)cpu_fault[1]<=1;
            if(pixel_read&&output_valid)read_pixels<=read_pixels+1'b1;
            if(reg_wr_i)case(reg_addr_i)
                0:begin
                    if(!reg_wdata_i[0])begin enable_q<=0;cpu_fault<=0;read_pixels<=0;meta_q<=15;end
                    else if(!enable_q)begin
                        if(valid_geometry)enable_q<=1;
                        else cpu_fault[2]<=1;
                    end
                end
                2:meta_q<=reg_wdata_i[4:0];
                3:if(!enable_q)geometry_q<=reg_wdata_i;else cpu_fault[4]<=1;
                default:;
            endcase
        end
    end
    wire complete=done_sync[1]&&(read_pixels==pixel_count);
    always @*begin
        reg_rdata_o=0;
        case(reg_addr_i)
            0:reg_rdata_o={26'b0,((|cpu_fault)||fault_sync[1]),idle_sync[1],complete,
                output_valid&&!fifo_reset,input_ready&&!fifo_reset,enable_q};
            2:reg_rdata_o={27'b0,meta_q};
            3:reg_rdata_o=geometry_q;
            4,7:reg_rdata_o=pixel_read?(output_valid?output_data:32'hffffffff):pixel_snapshot;
            5:reg_rdata_o={16'b0,read_pixels};
            6:reg_rdata_o={26'b0,fault_sync[1],cpu_fault};
            30:reg_rdata_o=32'd4; // Decoder revision: 2 adds 256-wide frames and try-pop; 3 progress watchdog; 4 core input fix (markers need a valid byte) and trailing output after a complete picture dropped.
            31:reg_rdata_o=32'h47454a50;
            default:;
        endcase
    end
endmodule
