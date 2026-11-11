# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Make the PocketFin core's source tree: the pinned openfpgaCore runtime plus PocketFin's changes.

Nothing upstream is edited. The script exports the exact pinned revisions (upstream.lock.json) from
the clones that tools/fetch_upstreams.py made, applies every PocketFin change with exact-text checks
(it stops if the upstream text is not what the patch expects), adds PocketFin's own RTL from rtl/,
and checks the result against core/expected-sources.json, the hashes of the tree the released
bitstream was built from.

Output: build/pocket-media-core (Git-ignored). Then build the core with tools/Build-PocketMediaCore.ps1.

    py -3 tools/prepare_pocket_media_core.py               # uses core/generated/ for CPU, MIF and OS
    py -3 tools/prepare_pocket_media_core.py --cpu-sources # also exports VexiiRiscv to regenerate the CPU
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import shutil
import subprocess
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / 'upstream'
OUT = ROOT / 'build/pocket-media-core'
LOCK = json.loads((ROOT / 'upstream.lock.json').read_text(encoding='utf-8'))['fetch']
CORE_REV = LOCK['openfpgaCore']['commit']
JPEG_REV = LOCK['core_jpeg']['commit']
CPU_REV = LOCK['VexiiRiscv']['commit']
SPINAL_REV = LOCK['VexiiRiscv']['submodules']['ext/SpinalHDL']['commit']
RVLS_REV = LOCK['VexiiRiscv']['submodules']['ext/rvls']['commit']
MEDIA_SOURCES = ['async_word_fifo.v', 'byte_fifo.v', 'gb_link_byte.v', 'gb_link_stream.v',
                 'pocket_media_link.v', 'pocket_jpeg_mmio.v', 'pocket_media_accel.v']


def archive(source, revision, destination):
    """Extract exactly `revision` of the Git repository at `source` into `destination`."""
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', revision + '^{commit}'], text=True).strip()
    if actual != revision:
        raise SystemExit(f'{source.name}: pinned revision {revision} not found; run tools/fetch_upstreams.py')
    data = subprocess.check_output(['git', '-C', str(source), '-c', 'core.autocrlf=false', 'archive', revision])
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        tar.extractall(destination, filter='data')


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise ValueError('Upstream source changed; patch does not apply: ' + old[:60])
    return text.replace(old, new)


def read(path):
    return path.read_text(encoding='utf-8')


def write(path, text):
    # Upstream files are LF; keep them LF on every platform.
    path.write_text(text, encoding='utf-8', newline='\n')


# ---- the hardware JPEG decoder (core_jpeg, Apache-2.0) ----------------------------------------------

def port_jpeg(source, output):
    """Four RAMs lose a write port every caller ties low (lets Quartus infer block RAM), and the input
    stage's marker tokens are qualified by inport_valid_i (a stale FF/D9 between words ended pictures
    early). Both changes are marked LOCAL in the files."""
    files = sorted((source / 'src_v').glob('*.v'))
    for file in files:
        if any(c != "1'b0" for c in re.findall(r'\.wr1_i\(([^)]+)\)', read(file))):
            raise ValueError('Port-1 write is no longer statically disabled')
    output.mkdir(parents=True, exist_ok=True)
    changed = set()
    for file in files:
        text = read(file)
        text, count = re.subn(r'    if \(wr1_i\)\n        ram\[addr1_i\](?:\[(?:15|31):0\])? <= data1_i(?:\[(?:15|31):0\])?;\n', '', text)
        if count:
            if count != 1:
                raise ValueError('Unexpected RAM source shape')
            text = '// LOCAL PORT: unused port-1 write removed; all callers tie wr1_i low.\n' + text
            at = text.rfind('endmodule')
            text = (text[:at] + '`ifndef SYNTHESIS\nalways @(posedge clk1_i) if (wr1_i) '
                    '$fatal(1, "Unused JPEG RAM write asserted");\n`endif\n' + text[at:])
            changed.add(file.name)
        if file.name == 'jpeg_input.v':
            text, count = re.subn(r"wire (token_\w+_w)(\s*)= \(last_b_q == 8'hFF && ",
                                  r"wire \1\2= (inport_valid_i && last_b_q == 8'hFF && ", text)
            if count != 12:
                raise ValueError('Unexpected JPEG input token shape')
            text = '// LOCAL FIX: marker tokens qualified by inport_valid_i (stale bytes between words).\n' + text
        write(output / file.name, text)
    if changed != {'jpeg_idct_ram_dp.v', 'jpeg_idct_transpose_ram.v', 'jpeg_output_y_ram.v', 'jpeg_output_cx_ram.v'}:
        raise ValueError('Unexpected RAM adaptation inventory')


def prepare(cpu_sources=False, out=OUT, upstream=UPSTREAM):
    if out.exists():
        shutil.rmtree(out)
    archive(upstream / 'openfpgaCore', CORE_REV, out)
    target = out / 'src/fpga/targets/pocket'
    cpu = out / 'src/fpga/vendor/vexriscv/VexiiRiscv'
    if cpu_sources:
        archive(upstream / 'VexiiRiscv', CPU_REV, cpu)
        archive(upstream / 'VexiiRiscv/ext/SpinalHDL', SPINAL_REV, cpu / 'ext/SpinalHDL')
        archive(upstream / 'VexiiRiscv/ext/rvls', RVLS_REV, cpu / 'ext/rvls')
    # Generated once from the pinned VexiiRiscv (MIT) by tools/Build-MediaCPU.ps1; kept so a core
    # build needs only Quartus. --cpu-sources plus Build-MediaCPU.ps1 regenerates it.
    cpu.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / 'core/generated/VexiiRiscv_media.v', cpu / 'VexiiRiscv_media.v')

    media = target / 'pocket_media'
    media.mkdir(exist_ok=True)
    for name in MEDIA_SOURCES:
        shutil.copyfile(ROOT / 'rtl' / name, media / name)
    shutil.copyfile(ROOT / 'rtl/pocket_media_cdc.sdc', media / 'pocket_media_cdc.sdc')
    jpeg_source = out.parent / 'core_jpeg-src'
    if jpeg_source.exists():
        shutil.rmtree(jpeg_source)
    archive(upstream / 'core_jpeg', JPEG_REV, jpeg_source)
    port_jpeg(jpeg_source, media / 'jpeg')

    # The runtime's link block becomes pocket_media_accel (byte link + JPEG), on a 90 MHz CPU clock.
    top = read(target / 'core_top.v')
    top = replace(top, 'wire [4:0]  link_reg_addr;', 'wire [5:0]  link_reg_addr;')
    top = replace(top, 'link_lite #(\n    .CLK_HZ(100000000),', 'pocket_media_accel #(\n    .CLK_HZ(90000000),')
    top = replace(top, ') link0 (\n    .clk(clk_cpu),', ') link0 (\n    .clk(clk_cpu),\n    .jpeg_clk_i(clk_core_24576),')
    write(target / 'core_top.v', top)
    periph = out / 'src/fpga/common/axi_periph_slave.v'
    text = read(periph)
    text = replace(text, 'output reg  [4:0]  link_reg_addr,', 'output reg  [5:0]  link_reg_addr,')
    text = replace(text, "localparam [31:0] HW_CONTRACT_REV = 32'd1;", "localparam [31:0] HW_CONTRACT_REV = 32'h504a0001;")
    for old, new in [('link_reg_addr <= ar_addr[6:2];', 'link_reg_addr <= ar_addr[7:2];'),
                     ('link_reg_addr <= periph_next_addr[6:2];', 'link_reg_addr <= periph_next_addr[7:2];'),
                     ('link_reg_addr  <= req_addr[6:2];', 'link_reg_addr  <= req_addr[7:2];')]:
        text = replace(text, old, new)
    # Upstream bug: the first W beat arriving after AW advanced the address by four bytes.
    text = replace(text, 'if (!burst_is_fixed)\n                        req_addr <= req_addr_plus4;',
                   'if (!burst_is_fixed && burst_count != 0)\n                        req_addr <= req_addr_plus4;')
    write(periph, text)
    # Exit saves: Reset Enter must not clear nonvolatile datatable entries (pocket_save_exit_patch.py).
    from pocket_save_exit_patch import patch_bridge_cmd, patch_bridge_cmd_test
    bridge = target / 'core_bridge_cmd.v'
    write(bridge, patch_bridge_cmd(read(bridge)))
    bridge_test = out / 'src/fpga/test/tb_core_bridge_cmd_main.cpp'
    write(bridge_test, patch_bridge_cmd_test(read(bridge_test)))
    # The link registers mean something different now, so FPGA and kernel get their own contract number.
    regs = out / 'src/firmware/os/hal/regs.h'
    write(regs, replace(read(regs), '#define OS_EXPECTED_HW_CONTRACT 1u', '#define OS_EXPECTED_HW_CONTRACT 0x504a0001u'))
    link = out / 'src/firmware/os/hal/link.c'
    text = read(link)
    for signature, result in [('void of_link_init(int master) {', ''), ('int of_link_send(uint32_t word) {', ' -1'),
                              ('int of_link_recv(uint32_t *word) {', ' -1'), ('int of_link_rx_available(void) {', ' 0')]:
        # LOCAL: the media core owns a different wire protocol, so the old link driver stands down.
        text = replace(text, signature, signature + '\n    if (REG32(0x4d00007cu) == 0x4b4c4a50u) return' + result + ';')
    write(link, text)
    net = out / 'src/firmware/os/hal/net.c'
    text = read(net)
    for signature in ('int of_net_host_start(void)\n{', 'int of_net_join(void)\n{'):
        text = replace(text, signature, signature + '\n    if (REG32(0x4d00007cu) == 0x4b4c4a50u) return -1;')
    write(net, text)
    qsf = read(target / 'ap_core.qsf')
    qsf = replace(qsf, 'set_global_assignment -name VERILOG_FILE link_lite.v',
                  '\n'.join('set_global_assignment -name VERILOG_FILE pocket_media/' + s for s in MEDIA_SOURCES))
    qsf += '\n# LOCAL: media-specific CPU and features; no inherited GPU/game-only macros.\n'
    for file in sorted((media / 'jpeg').glob('*.v')):
        qsf += 'set_global_assignment -name VERILOG_FILE pocket_media/jpeg/' + file.name + '\n'
    for define in ('INCLUDE_LINK', 'INCLUDE_DIRECT_COLOR', 'INCLUDE_CLK90'):
        qsf += 'set_global_assignment -name VERILOG_MACRO "' + define + '=1"\n'
    qsf += 'set_global_assignment -name VERILOG_FILE ../../vendor/vexriscv/VexiiRiscv/VexiiRiscv_media.v\n\n'  # blank line as in the released tree
    qsf += 'set_global_assignment -name SDC_FILE pocket_media/pocket_media_cdc.sdc\n'
    for pattern in ('*|link0|jpeg|*queue|wg_r*', '*|link0|jpeg|*queue|rg_w*', '*|link0|jpeg|fault_sync*',
                    '*|link0|jpeg|done_sync*', '*|link0|jpeg|idle_sync*'):
        qsf += 'set_instance_assignment -name SYNCHRONIZER_IDENTIFICATION FORCED_IF_ASYNCHRONOUS -to ' + pattern + '\n'
    # Fitter seed 9 is the one the released bitstream was placed with (it meets timing); the version line
    # is what Quartus 21.1.1 writes back into the file.
    qsf = replace(qsf, 'set_global_assignment -name SEED 6', 'set_global_assignment -name SEED 9')
    qsf = replace(qsf, 'set_global_assignment -name LAST_QUARTUS_VERSION "25.1std.0 Lite Edition"',
                  'set_global_assignment -name LAST_QUARTUS_VERSION "21.1.1 Lite Edition"')
    qsf = replace(qsf, 'set_global_assignment -name NUM_PARALLEL_PROCESSORS ALL',
                  'set_global_assignment -name NUM_PARALLEL_PROCESSORS 2')
    write(target / 'ap_core.qsf', qsf)
    config = out / 'src/fpga/vendor/vexriscv/configs/media.cfg'
    original = read(config.with_name('os25.cfg'))
    original = replace(original, 'ICACHE_SETS=256', 'ICACHE_SETS=128')
    original = replace(original, 'DCACHE_SETS=1024', 'DCACHE_SETS=512')
    write(config, '# LOCAL MEDIA: single-issue,16KiB I$,64KiB D$,90MHz target.\n' + original)
    # The bootloader image and paired kernel the released core was built with (tools/Build-MediaFirmware.ps1
    # rebuilds both from this tree with the RISC-V compiler).
    shutil.copyfile(ROOT / 'core/generated/firmware.mif', target / 'firmware.mif')
    (out / 'src/firmware/os/bld/pocket').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / 'core/generated/os.bin', out / 'src/firmware/os/bld/pocket/os.bin')
    shutil.rmtree(jpeg_source)
    return out


def text_sha256(path):
    """SHA-256 of a file with CRLF read as LF, so the check does not depend on Git's line-ending settings."""
    return hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def check(out=OUT):
    """Every file PocketFin adds or changes must match core/expected-sources.json (the released core's tree)."""
    expected = json.loads((ROOT / 'core/expected-sources.json').read_text(encoding='utf-8'))['sha256']
    bad = [name for name, digest in expected.items()
           if not (out / name).is_file() or text_sha256(out / name) != digest]
    if bad:
        raise SystemExit('These files differ from the released core\'s sources: ' + ', '.join(bad))
    return len(expected)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cpu-sources', action='store_true', help='also export VexiiRiscv/SpinalHDL/rvls')
    parser.add_argument('--upstream', type=Path, default=UPSTREAM, help='folder holding the fetched clones')
    parser.add_argument('--out', type=Path, default=OUT)
    args = parser.parse_args()
    folder = prepare(args.cpu_sources, args.out, args.upstream)
    count = check(folder)
    print(f'Core source tree ready in {folder.relative_to(ROOT) if folder.is_relative_to(ROOT) else folder}: '
          f'{count} files match the released core. Next: tools/Build-PocketMediaCore.ps1')
