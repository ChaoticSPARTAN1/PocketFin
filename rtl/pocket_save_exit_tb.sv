// SPDX-License-Identifier: GPL-3.0-or-later
// Additional terms and permissions: ADDITIONAL-TERMS.md
// Exit-save handshake bench for core_bridge_cmd (tools/test_pocket_save_exit.py drives it).
//
// Plays the Pocket host and the core around a quit:
//   1. the host writes the datatable over the bridge (0xF8002000), one entry per slot with a file;
//   2. the host issues Reset Exit, the core runs; the kernel commits save sizes on port A
//      (SAVE_DT_WORD -> datatable word entry*2+1, as core_top's arbiter does);
//   3. quit: the host issues Reset Enter (0x0010) and waits for the OK result, the CPU either
//      acknowledges (+ACK=1) or never does and the ~56 ms timeout runs (+ACK=0, the real case
//      while the security wipe is still running);
//   4. the host reads the datatable back over the bridge; the bench dumps it for the checker;
//   5. for every entry whose id is a nonvolatile slot (8-19) with a nonzero size the host asks
//      0x0080 Data Slot Request Read (slot id in parameter 0) before reading the save window,
//      exactly as the Pocket's own debug log shows at an unload ("Reading core data slot BRAM
//      table ... Slot says it is N bytes ... 0x0080 ... Readback"); the result codes are
//      appended to the dump as "rr <id> <result>" lines.
`timescale 1ns/1ps
`default_nettype none

module pocket_save_exit_tb;
    reg clk = 0;
    always #6.734 clk = ~clk;   // clk_74a

    reg  [31:0] bridge_addr = 0;
    reg         bridge_rd = 0, bridge_wr = 0;
    reg  [31:0] bridge_wr_data = 0;
    wire [31:0] bridge_rd_data;
    reg         shutdown_ack_s = 0;
    wire        reset_n, shutdown_pending;
    reg  [9:0]  dt_addr = 0;
    reg         dt_wren = 0;
    reg  [31:0] dt_data = 0;
    wire [31:0] dt_q;

    wire        unused_bits;
    wire [31:0] rtc_epoch, rtc_date, rtc_time;
    wire [15:0] rr_id, rw_id, up_id;
    wire [31:0] rw_size, up_size;

    core_bridge_cmd dut (
        .clk(clk), .reset_n(reset_n),
        .bridge_endian_little(1'b0), .bridge_addr(bridge_addr), .bridge_rd(bridge_rd),
        .bridge_rd_data(bridge_rd_data), .bridge_wr(bridge_wr), .bridge_wr_data(bridge_wr_data),
        .shutdown_pending(shutdown_pending), .shutdown_ack_s(shutdown_ack_s),
        .status_boot_done(1'b1), .status_setup_done(1'b1), .status_running(reset_n),
        .dataslot_requestread(), .dataslot_requestread_id(rr_id),
        .dataslot_requestread_ack(1'b1), .dataslot_requestread_ok(1'b1),
        .dataslot_requestwrite(), .dataslot_requestwrite_id(rw_id), .dataslot_requestwrite_size(rw_size),
        .dataslot_requestwrite_ack(1'b1), .dataslot_requestwrite_ok(1'b1),
        .dataslot_update(), .dataslot_update_id(up_id), .dataslot_update_size(up_size),
        .dataslot_allcomplete(),
        .rtc_epoch_seconds(rtc_epoch), .rtc_date_bcd(rtc_date), .rtc_time_bcd(rtc_time), .rtc_valid(),
        .savestate_supported(1'b0), .savestate_addr(32'd0), .savestate_size(32'd0),
        .savestate_maxloadsize(32'd0), .savestate_start(), .savestate_start_ack(1'b0),
        .savestate_start_busy(1'b0), .savestate_start_ok(1'b0), .savestate_start_err(1'b0),
        .savestate_load(), .savestate_load_ack(1'b0), .savestate_load_busy(1'b0),
        .savestate_load_ok(1'b0), .savestate_load_err(1'b0), .osnotify_inmenu(),
        .target_dataslot_read(1'b0), .target_dataslot_write(1'b0),
        .target_dataslot_getfile(1'b0), .target_dataslot_openfile(1'b0),
        .target_dataslot_ack(), .target_dataslot_done(), .target_dataslot_err(),
        .target_dataslot_id(16'd0), .target_dataslot_slotoffset(32'd0),
        .target_dataslot_bridgeaddr(32'd0), .target_dataslot_length(32'd0),
        .target_buffer_param_struct(32'd0), .target_buffer_resp_struct(32'd0),
        .datatable_addr(dt_addr), .datatable_wren(dt_wren), .datatable_data(dt_data), .datatable_q(dt_q)
    );

    integer clock_count = 0;
    always @(posedge clk) clock_count <= clock_count + 1;

    task automatic tick(input integer n);
        repeat (n) @(posedge clk);
        #1;
    endtask

    task automatic host_write(input [31:0] addr, input [31:0] data);
        bridge_addr = addr; bridge_wr_data = data; tick(1);
        bridge_wr = 1; tick(1); bridge_wr = 0; tick(2);
    endtask

    // io_bridge_peripheral holds the address 4 cycles, then pulses rd (reads are 1-word buffered;
    // a fixed-address re-read returns the settled word).
    task automatic host_read(input [31:0] addr, output [31:0] data);
        bridge_addr = addr; tick(4);
        bridge_rd = 1; tick(1); bridge_rd = 0; tick(2);
        data = bridge_rd_data;
    endtask

    task automatic host_command(input [15:0] cmd, input integer limit, output integer cycles);
        reg [31:0] status;
        integer started;
        host_write(32'hF8000000, {16'h434D, cmd});
        started = clock_count;
        cycles = 0;
        status = 0;
        while (status[31:16] != 16'h4F4B && cycles < limit) begin
            tick(256);
            host_read(32'hF8000000, status);
            cycles = clock_count - started;
        end
        if (status[31:16] != 16'h4F4B) begin
            $display("FAIL host command %h never completed (status %h)", cmd, status);
            $finish;
        end
    endtask

    reg [31:0] table_in [0:127];
    reg [31:0] commits [0:31];  // {word, value} pairs, 0xFFFFFFFF ends
    integer i, fd, cycles, ack;
    reg [31:0] word;
    string dt_file, commit_file, out_file;

    // Cyclone V registers without an initial value power up as 0 (hstate/tstate = idle).
    initial begin
        dut.hstate = 0; dut.tstate = 0; dut.host_cmd_start = 0; dut.host_0 = 0;
    end

    initial begin
        for (i = 0; i < 32; i = i + 1) commits[i] = 32'hFFFFFFFF;
        if (!$value$plusargs("DT=%s", dt_file) || !$value$plusargs("COMMIT=%s", commit_file) ||
            !$value$plusargs("OUT=%s", out_file) || !$value$plusargs("ACK=%d", ack)) begin
            $display("FAIL missing plusargs"); $finish;
        end
        $readmemh(dt_file, table_in);
        $readmemh(commit_file, commits);
        tick(8);
        // 1. Load: the host writes every datatable word it owns.
        for (i = 0; i < 128; i = i + 1) host_write(32'hF8002000 + i * 4, table_in[i]);
        // 2. The core runs; the kernel commits the sizes it wrote (port A, entry-resolved).
        host_command(16'h0011, 1 << 12, cycles);
        for (i = 0; i < 32 && commits[i] !== 32'hFFFFFFFF; i = i + 2) begin
            dt_addr = commits[i][9:0]; dt_data = commits[i + 1]; dt_wren = 1; tick(1); dt_wren = 0; tick(2);
        end
        // 3. Quit.
        fork
            begin
                if (ack) begin
                    wait (shutdown_pending);
                    tick(64);
                    shutdown_ack_s = 1;
                end
            end
        join_none
        host_command(16'h0010, 1 << 24, cycles);
        if (reset_n) begin $display("FAIL core not held in reset after Reset Enter"); $finish; end
        $display("reset_enter_cycles %0d ack %0d", cycles, ack);
        // 4. Exit save: the host reads the table back.
        fd = $fopen(out_file, "w");
        for (i = 0; i < 128; i = i + 1) begin
            host_read(32'hF8002000 + i * 4, word);
            $fdisplay(fd, "%08h", word);
            table_in[i] = word;
        end
        // 5. The host asks to read each nonvolatile slot the table still names (0x0080 with the
        //    slot id in parameter 0); result 0 = ready. The core must answer while held in reset.
        for (i = 0; i < 64; i = i + 1) begin
            if (table_in[i * 2][15:0] >= 16'd8 && table_in[i * 2][15:0] <= 16'd19 && table_in[i * 2 + 1] != 0) begin
                host_write(32'hF8000020, {16'd0, table_in[i * 2][15:0]});
                host_command(16'h0080, 1 << 12, cycles);
                host_read(32'hF8000000, word);
                $fdisplay(fd, "rr %0d %0d", table_in[i * 2][15:0], word[15:0]);
            end
        end
        $fclose(fd);
        $display("DONE");
        $finish;
    end
endmodule

`default_nettype wire
