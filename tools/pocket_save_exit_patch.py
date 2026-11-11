# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Exit-save fix for the staged Pocket media core's core_bridge_cmd.v (pinned upstream untouched).

On Reset Enter (the host's quit/reload handshake) the pinned bridge clears datatable words 0-15,
assuming read-only slots sit in entries 0-7 and saves in 8-18 (the SDK template's dense layout).
The Pocket fills the datatable only for slots that have a file, in data.json order, so the Jellyfin
package (slots 0-4, one pack, then saves 10-19) puts the log (10) and watch state (11) in entries
6 and 7. The clear zeroed their id/size words before the host read them back for the exit save, so
nothing was written. The patched clear keeps any entry whose id is a nonvolatile slot (8-19).
"""
from prepare_pocket_media_core import replace

OLD_CLEAR = '''    if(b_datatable_clear_active) begin
        if(b_datatable_clear_addr < 5'd16) begin
            b_datatable_wren <= 1;
            b_datatable_addr <= {5'd0, b_datatable_clear_addr};
            b_datatable_clear_addr <= b_datatable_clear_addr + 5'd1;
        end else begin'''

NEW_CLEAR = '''    if(b_datatable_clear_active) begin
        // LOCAL (Pocket Jellyfin): clear entries 0-7 one ENTRY at a time and keep any
        // entry whose id word names a nonvolatile slot (ids 8-19, save.c nvslot_map).
        // The Pocket fills the datatable only for slots that have a file, in data.json
        // order, so a layout with optional slots puts save entries inside 0-7. The host
        // reads those size words back for the exit save after this Reset Enter; the
        // unconditional 16-word clear zeroed them and the save was never written.
        // b_datatable_clear_addr counts entries here (0-8), not words.
        if(b_datatable_clear_addr < 5'd8) begin
            case(b_datatable_clear_phase)
            2'd0, 2'd1: begin
                // present the id word; the BRAM registers it, q_b follows a cycle later
                b_datatable_addr <= {4'd0, b_datatable_clear_addr, 1'b0};
                b_datatable_clear_phase <= b_datatable_clear_phase + 2'd1;
            end
            2'd2: begin
                b_datatable_addr <= {4'd0, b_datatable_clear_addr, 1'b0};
                if(b_datatable_q[15:0] >= 16'd8 && b_datatable_q[15:0] <= 16'd19) begin
                    b_datatable_clear_addr <= b_datatable_clear_addr + 5'd1;
                    b_datatable_clear_phase <= 2'd0;
                end else begin
                    b_datatable_wren <= 1;              // zero the id word
                    b_datatable_clear_phase <= 2'd3;
                end
            end
            2'd3: begin
                b_datatable_wren <= 1;                  // zero the size word
                b_datatable_addr <= {4'd0, b_datatable_clear_addr, 1'b1};
                b_datatable_clear_addr <= b_datatable_clear_addr + 5'd1;
                b_datatable_clear_phase <= 2'd0;
            end
            endcase
        end else begin'''


def patch_bridge_cmd_test(text):
    """The fork's Verilator test (not runnable here): read-only ids in 0-7, plus a compacted layout.

    The pinned test filled every word with 0xA5000000|word, so entries 4-7 carried ids 8, 10, 12
    and 14, which the fix now keeps. Give entries 0-7 read-only ids, and add the case that broke.
    """
    text = replace(text, '''    for (uint32_t word = 0; word < 20; word++)
        write_dt_word(word, 0xA5000000u | word);
''', '''    for (uint32_t word = 0; word < 20; word++)
        write_dt_word(word, word < 16 && !(word & 1) ? (word >> 1) : (0xA5000000u | word));
''')
    added = '''
/* LOCAL: compacted layout (slots 0-4, one pack 20, then saves 10-19): the
 * save entries at 6 and 7 must survive Reset Enter, the read-only ones clear. */
static void test_reset_enter_keeps_compacted_save_entries() {
    std::printf("test_reset_enter_keeps_compacted_save_entries:\\n");
    init_inputs();
    const uint32_t ids[10] = {0, 1, 2, 3, 4, 20, 10, 11, 12, 13};
    for (uint32_t e = 0; e < 10; e++) {
        write_dt_word(e * 2u, ids[e]);
        write_dt_word(e * 2u + 1u, 0x1000u + e);
    }
    tb->status_setup_done = 1;
    check_eq("compacted-ready", wait_target_cmd(0x636D0140u) ? 1u : 0u, 1u);
    ack_target_cmd();
    host_cmd(0x0011);
    host_cmd(0x0010);
    for (int i = 0; i < 64; i++)
        tick();
    for (uint32_t e = 0; e < 10; e++) {
        bool keep = ids[e] >= 8 && ids[e] <= 19;
        char tag[48];
        std::snprintf(tag, sizeof(tag), "compacted-entry-%u-id", e);
        check_eq(tag, read_dt_word(e * 2u), keep ? ids[e] : 0u);
        std::snprintf(tag, sizeof(tag), "compacted-entry-%u-size", e);
        check_eq(tag, read_dt_word(e * 2u + 1u), keep ? 0x1000u + e : 0u);
    }
}

int main('''
    text = replace(text, '\nint main(', added)
    return replace(text, '    test_reset_enter_clears_readonly_datatable_only();\n',
                   '    test_reset_enter_clears_readonly_datatable_only();\n'
                   '    test_reset_enter_keeps_compacted_save_entries();\n')


def patch_bridge_cmd(text):
    """Pinned core_bridge_cmd.v text (LF newlines) -> the staged fork's text."""
    text = replace(text, '    b_datatable_clear_addr <= 0;\nend',
                   '    b_datatable_clear_addr <= 0;\n    b_datatable_clear_phase <= 0;\nend')
    text = replace(text, OLD_CLEAR, NEW_CLEAR)
    text = replace(text, '                b_datatable_clear_addr <= 0;\n                hstate <= ST_CLEAR_DATATABLE;',
                   '                b_datatable_clear_addr <= 0;\n                b_datatable_clear_phase <= 0;\n'
                   '                hstate <= ST_CLEAR_DATATABLE;')
    return replace(text, '    reg     [4:0]   b_datatable_clear_addr;\n',
                   '    reg     [4:0]   b_datatable_clear_addr;\n    reg     [1:0]   b_datatable_clear_phase;\n')
