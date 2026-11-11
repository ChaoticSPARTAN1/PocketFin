# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
# Local FIFO CDC bounds for Quartus Standard 21.1 / Cyclone V.
# Source gray MSBs can merge into the matching binary pointer MSBs. Check
# every endpoint, including that equivalent register, instead of a quiet cut.
# set_net_delay is independent of the source design's async clock groups.
# 8 ns is below the fastest 90 MHz pointer period (11.111 ns), with margin
# for cell/clock skew. Inspect report_net_delay and CDC reports after routing.
foreach pj_queue {input_queue output_queue} {
    foreach {pj_gray pj_binary pj_meta} {wg wb wg_r1 rg rb rg_w1} {
        # Include the binary MSB unconditionally: physical optimization may
        # duplicate a lower Gray bit, so a total source count of nine does not
        # prove that bit 8 was included.
        set pj_sources [get_registers -nowarn [list "*|link0|jpeg|${pj_queue}|${pj_gray}\[*\]" "*|link0|jpeg|${pj_queue}|${pj_binary}\[8\]"]]
        set pj_targets [get_registers "*|link0|jpeg|${pj_queue}|${pj_meta}\[*\]"]
        set pj_source_bits {}
        set pj_target_bits {}
        foreach_in_collection pj_node $pj_sources {
            set pj_name [get_node_info -name $pj_node]
            if {[regexp [format {\|%s\[([0-9]+)\]} $pj_gray] $pj_name -> pj_bit]} {
                lappend pj_source_bits $pj_bit
            } elseif {[regexp [format {\|%s\[8\]} $pj_binary] $pj_name]} {
                lappend pj_source_bits 8
            }
        }
        foreach_in_collection pj_node $pj_targets {
            if {[regexp [format {\|%s\[([0-9]+)\]} $pj_meta] [get_node_info -name $pj_node] -> pj_bit]} {
                lappend pj_target_bits $pj_bit
            }
        }
        if {[lsort -integer -unique $pj_source_bits] ne {0 1 2 3 4 5 6 7 8} ||
            [lsort -integer -unique $pj_target_bits] ne {0 1 2 3 4 5 6 7 8}} {
            error "Media FIFO pointer CDC selector mismatch: $pj_queue / $pj_gray"
        }
        set_net_delay -max 8.0 -from $pj_sources -to $pj_targets
    }
}
