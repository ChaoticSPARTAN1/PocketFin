# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Exit save: do the Pocket Jellyfin package's save slots survive the core's quit handshake?

PC-only. Runs the core's real core_bridge_cmd.v (Icarus) as the Pocket host would drive it at a quit:
datatable written at load, save sizes committed by the kernel, Reset Enter, then the host reads the
datatable back to size each nonvolatile .sav it writes (pocket_save_exit_tb.sv).

Layouts come from package_release.slot_tables(), for 0-12 packs, under both fill models:
  compacted: one entry per slot that has a file, in data.json order (openfpgaOS save.c / core_top.v
             notes from hardware: "the Pocket compacts the table to loaded slots");
  dense:     one entry per declared slot (openfpgaOS commit 354af8d's reading);
  chip32:    what this core really gets (core.json names a Chip32 loader): an entry, at the
             declaration index, only for a slot the loader LOADFs. Run for the SDK loader (no
             LOADF: the Pocket debug log of 2026-11-09 17:13, every save skipped) and for the
             package's loader (tools/pocket_loader.py).

The pinned bridge (openfpgaCore 618a3eb) must show the defect on the one-pack layout, and the staged
bridge must keep every save entry in every case. Also checks the staged file is exactly the
pinned file plus tools/pocket_save_exit_patch.py, and runs the real ~56 ms no-acknowledge
timeout once (the case on hardware: the kernel's CRAM0 security wipe outlasts it).

The host side follows the Pocket's own debug log of an unload (Analogue OS 2.3, /System/Logs):
Reset Enter first, then for each nonvolatile slot "Reading core data slot BRAM table. Slot ID
[N] idx I / Slot says it is S bytes / 0x0080 Data slot: Request Read / Readback". The bench
issues that Request Read for every save entry the table still names and the checker requires
result 0. The kernel side is modelled as the app sees it: fopen("slot:N","wb") commits size 0
(O_TRUNC), fclose() commits the written size (openfpgaOS syscall.c open_nv_fd / sys_close ->
save.c nvslot_commit_size, SAVE_DT_WORD = entry*2+1 after an id scan).

When Intel's altsyncram simulation model is installed with Quartus (altera_mf.v), the staged
bridge is also run with the real datatable megafunction instead of the fork's behavioural stub,
so a read-latency assumption in the exit-save patch cannot hide behind the stub. The app's own
table reader (pocket/save_table.h, what the start screen shows) is checked with gcc against the
same layouts. No card, device or private data is touched.
"""
from pathlib import Path
import json
import os
import re
import subprocess
import sys

import toolpaths
from toolpaths import ROOT
from package_release import (slot_tables, save_entries, quit_lookup_entries, check_slot_order, check_bitstream,
                             reverse_bits, RBF_IN_RELEASE, LOG_SLOT, CACHE_SLOTS)
from pocket_loader import build_loader, loaded_slots
from pocket_save_exit_patch import patch_bridge_cmd

PIN = json.loads((ROOT / 'upstream.lock.json').read_text(encoding='utf-8'))['fetch']['openfpgaCore']['commit']
TOOLS = toolpaths.IVERILOG   # Icarus Verilog (bin/iverilog.exe, lib/ivl)
GCC = toolpaths.GCC
ALTERA_MF = toolpaths.QUARTUS / 'eda/sim_lib/altera_mf.v'   # Intel's simulation library
STAGE = ROOT / 'build/pocket-media-core/src/fpga/targets/pocket'   # tools/prepare_pocket_media_core.py
OUT = ROOT / 'build/save-exit-sim'
SEEDED = 32768                       # size of the seed saves in release/SD-first-install-saves
COMMITS = {LOG_SLOT: 1234, CACHE_SLOTS[0]: 200}   # what the app's "wb" + close() publish
NONVOLATILE = range(8, 20)


def pinned(path):
    data = subprocess.check_output(['git', '-C', str(ROOT / 'upstream/openfpgaCore'), 'show', f'{PIN}:{path}'])
    return data.decode('utf-8').replace('\r\n', '\n')


def run(args, timeout=600):
    env = toolpaths.msys_env()
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=timeout, env=env,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if result.returncode or 'FAIL' in result.stdout:
        raise SystemExit(f'Simulation failed ({result.returncode}):\n{result.stdout}\n{result.stderr}')
    return result.stdout


def simulation_order(text):
    """Icarus needs declarations before use; Quartus accepts the source order. Simulation copy only:
    the b_datatable_* declaration block moves up, unchanged, to just after the endian registers."""
    start = text.index('    wire    [31:0]  b_datatable_q;\n')
    end = text.index('\nmf_datatable idt (')
    block = text[start:end]
    text = text[:start] + text[end:]
    anchor = '    reg     [31:0]  bridge_rd_data_out;\n'
    if text.count(anchor) != 1:
        raise ValueError('core_bridge_cmd.v layout changed')
    return text.replace(anchor, anchor + '// SIMULATION ONLY: moved declarations\n' + block + '\n')


def build(name, bridge_text, ram_model=None):
    """ram_model: the fork's altsyncram stub by default, or Intel's altera_mf.v (the real megafunction)."""
    folder = OUT / name
    folder.mkdir(parents=True, exist_ok=True)
    bridge = folder / 'core_bridge_cmd.v'
    bridge.write_text(simulation_order(bridge_text), encoding='utf-8')
    vvp = folder / 'bench.vvp'
    run([TOOLS / 'bin/iverilog.exe', '-g2012', '-B', TOOLS / 'lib/ivl', '-s', 'pocket_save_exit_tb', '-o', vvp,
         ROOT / 'rtl/pocket_save_exit_tb.sv', bridge, STAGE / 'apf/mf_datatable.v', STAGE / 'apf/common.v',
         ram_model or STAGE.parents[1] / 'test/altsyncram_stub.v'])
    return vvp


def layout(packs, model):
    definitions, slots = slot_tables([f'pack{i:02d}.pjs' for i in range(packs)])
    present = {0} | {slot['id'] for slot in slots}
    ids = [d['id'] for d in definitions if model == 'dense' or d['id'] in present]
    if len(ids) > 64:
        raise ValueError('More than 64 datatable entries')
    return ids, {d['id']: d for d in definitions}


def simulate(vvp, ids, case, ack=1):
    words = [0] * 128
    for entry, slot in enumerate(ids):
        if slot is None:
            continue                   # chip32: the Pocket never wrote this entry
        words[entry * 2] = slot
        words[entry * 2 + 1] = SEEDED if slot in NONVOLATILE else 0x1000 + slot
    commits = []
    for slot, size in COMMITS.items():
        # The kernel's entry scan (file.c datatable_entry_scan_for_slot) then SAVE_DT_WORD = entry*2+1:
        # fopen(.., "wb") commits 0 (O_TRUNC), fclose() commits the bytes written.
        entry = next((e for e, s in enumerate(ids) if s == slot), None)
        if entry is not None:
            commits += [entry * 2 + 1, 0, entry * 2 + 1, size]
    commits.append(0xFFFFFFFF)
    folder = vvp.parent
    (folder / f'{case}-dt.hex').write_text(''.join(f'{w:08x}\n' for w in words))
    (folder / f'{case}-commit.hex').write_text(''.join(f'{w:08x}\n' for w in commits))
    out = folder / f'{case}-after.hex'
    log = run([TOOLS / 'bin/vvp.exe', '-M', TOOLS / 'lib/ivl', vvp, f'+DT={folder / f"{case}-dt.hex"}',
               f'+COMMIT={folder / f"{case}-commit.hex"}', f'+OUT={out}', f'+ACK={ack}'])
    lines = out.read_text().splitlines()
    after = [int(line, 16) for line in lines if not line.startswith('rr')]
    # "rr <slot> <result>": the host's 0x0080 Request Read for each save the table still names
    request_read = {int(m.group(1)): int(m.group(2)) for m in (re.match(r'rr (\d+) (\d+)', l) for l in lines) if m}
    lost, kept_readonly = [], []
    for entry, slot in enumerate(ids):
        idw, size = after[entry * 2], after[entry * 2 + 1]
        if slot is None:
            continue
        if slot in NONVOLATILE:
            want = COMMITS.get(slot, SEEDED)
            # The host sizes the .sav from the size word (0: nothing written) and only reads the save
            # window when the core answers Request Read with 0 (ready).
            if (idw & 0xFFFF) != slot or size != want or request_read.get(slot) != 0:
                lost.append(slot)
        elif entry < 8 and (idw or size):
            kept_readonly.append(slot)     # the clear's original purpose must still hold
    cycles = int(log.split('reset_enter_cycles ')[1].split()[0])
    return lost, kept_readonly, cycles


def chip32_cases(benches):
    """The table as a Chip32 load leaves it: entries only for LOADFed slots, at the declaration index.

    The quit is the Pocket's (debug log, Analogue OS 2.3): each nonvolatile slot is read at its
    declaration index; an id word that differs means "ID does not match expected value" and no
    write-back. The kernel side is save.c nvslot_commit_size: no entry for the slot, no commit.
    """
    stock = (ROOT / 'upstream/openfpgaSDK/runtime/pocket/loader.bin').read_bytes()
    definitions, slots = slot_tables(['pack00.pjs'])
    saves = [d['id'] for d in definitions if d.get('nonvolatile')]
    files = {slot['id']: b'' for slot in slots}
    files[2] = b'[os]\r\nELF=pjlive.elf\r\nARGS=\r\nVARIANT=os25\r\n'
    failures = 0
    for name, loader, expect in (('SDK loader', stock, {'pinned': saves, 'staged': saves}),
                                 ('package loader', build_loader(stock, saves),
                                  {'pinned': saves[:3], 'staged': []})):
        loaded = loaded_slots(loader, files)
        ids = [d['id'] if d['id'] in loaded else None for d in definitions]
        for bridge, vvp in benches.items():
            lost, kept, _ = simulate(vvp, ids, f'chip32-{name.split()[0].lower()}-{bridge}')
            # a save slot with no entry is never written back, whatever the bridge does
            lost = sorted(set(lost) | {slot for slot in saves if slot not in loaded})
            ok = lost == expect[bridge] and not kept
            failures += not ok
            print(f'{"OK  " if ok else "FAIL"} chip32, {name} (LOADFs {loaded or "nothing"}), {bridge} bridge: '
                  f'loses {lost or "none"}')
    return failures


def check_app_table_reader(ids):
    """pocket/save_table.h (the start screen's table line) resolves the save slots like the kernel."""
    exe = OUT / 'check_save_table.exe'
    run([GCC, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-DOF_PC', '-I', ROOT / 'pocket',
         ROOT / 'pocket/check_save_table.c', '-o', exe])
    out = run([exe, *ids])
    if 'PASS' not in out:
        raise SystemExit(f'save_table.h check failed:\n{out}')
    diag = (ROOT / 'pocket/diag_log.h').read_text(encoding='utf-8')
    store = (ROOT / 'pocket/tether_store.h').read_text(encoding='utf-8')
    if f'#define PJ_DIAG_SLOT_ID {LOG_SLOT}u' not in diag or f'PJ_STORE_STATE_SLOT PJ_TETHER_SLOT_FIRST' not in store:
        raise SystemExit('The app reads a different slot number than the package declares')


def main():
    staged = (STAGE / 'core_bridge_cmd.v').read_text(encoding='utf-8')
    original = pinned('src/fpga/targets/pocket/core_bridge_cmd.v')
    if patch_bridge_cmd(original) != staged:
        raise SystemExit('Staged core_bridge_cmd.v is not the pinned file plus pocket_save_exit_patch.py')
    print('staged core_bridge_cmd.v == pinned + exit-save patch: OK')
    benches = {'pinned': build('pinned', original), 'staged': build('staged', staged)}
    failures = 0
    for model in ('compacted', 'dense'):
        for packs in range(13):
            ids, _ = layout(packs, model)
            case = f'{model}-{packs}'
            results = {name: simulate(vvp, ids, case) for name, vvp in benches.items()}
            lost_pin, _, _ = results['pinned']
            lost_fix, kept, _ = results['staged']
            expected_pin = [s for e, s in enumerate(ids) if s in NONVOLATILE and e < 8]
            ok = not lost_fix and not kept and lost_pin == expected_pin
            failures += not ok
            mark = 'OK  ' if ok else 'FAIL'
            print(f'{mark} {case:13s} save entries {[e for e, s in enumerate(ids) if s in NONVOLATILE][:3]}...: '
                  f'pinned loses {lost_pin or "none"}, staged loses {lost_fix or "none"}'
                  + (f', read-only kept {kept}' if kept else ''))
    # A one-pack card, compacted. Pinned loses the log, the watch state and cache 1.
    ids, _ = layout(1, 'compacted')
    lost_pin, _, _ = simulate(benches['pinned'], ids, 'onepack-pinned')
    if lost_pin != [LOG_SLOT, CACHE_SLOTS[0], CACHE_SLOTS[1]]:
        print(f'FAIL one-pack layout: pinned bridge expected to lose slots 10, 11 and 12, lost {lost_pin}')
        failures += 1
    else:
        print('OK   one-pack layout (1 pack, compacted): pinned bridge drops slots 10 (pjlive_log), 11 (pjcache0), 12')
    # The quit lookup (Pocket debug log 2026-11-09): the host reads the entry at each save slot's
    # DECLARATION index and refuses it when the id word differs ("ID does not match expected
    # value"). With the old order (packs before saves) one present pack put the saves at entries
    # 6-15 while the host read 17-26: size 0, nothing written. Every pack count must agree now.
    for packs in range(13):
        definitions, slots = slot_tables([f'pack{i:02d}.pjs' for i in range(packs)])
        loaded, looked_up = save_entries(definitions, slots), quit_lookup_entries(definitions)
        if loaded != looked_up:
            failures += 1
            print(f'FAIL {packs} packs: saves load at {loaded} but the Pocket looks them up at {looked_up}')
    old = [{'id': i, 'nonvolatile': False} for i in (0, 1, 2, 3, 4, *range(20, 32))] + \
          [{'id': i, 'nonvolatile': True} for i in (LOG_SLOT, *CACHE_SLOTS)]
    try:
        check_slot_order(old)
        failures += 1
        print('FAIL the old declaration order (packs before saves) was not refused')
    except ValueError:
        print('OK   quit lookup: load entry == declaration index for 0-12 packs; the old order (packs first) is refused')
    failures += chip32_cases(benches)
    lost, kept, cycles = simulate(benches['staged'], ids, 'onepack-timeout', ack=0)
    ok = not lost and not kept and cycles >= 1 << 22
    failures += not ok
    print(f'{"OK  " if ok else "FAIL"} one-pack layout, no CPU acknowledge: Reset Enter took {cycles} cycles '
          f'({cycles / 74.25e3:.1f} ms at 74.25 MHz); staged loses {lost or "none"}')
    # The same quit against Intel's real datatable megafunction model, not the fork's stub.
    if ALTERA_MF.is_file():
        real = build('staged-altera-mf', staged, ALTERA_MF)
        for model, ack in (('compacted', 0), ('compacted', 1), ('dense', 1)):
            ids, _ = layout(1, model)
            lost, kept, cycles = simulate(real, ids, f'onepack-mf-{model}-ack{ack}', ack=ack)
            ok = not lost and not kept and (ack or cycles >= 1 << 22)
            failures += not ok
            print(f'{"OK  " if ok else "FAIL"} one-pack layout with Intel altsyncram model ({model}, ack {ack}): '
                  f'staged loses {lost or "none"}' + (f', read-only kept {kept}' if kept else ''))
    else:
        print('SKIP Intel altera_mf.v not installed: real megafunction run not available')
    for model in ('compacted', 'dense'):
        ids, _ = layout(1, model)
        check_app_table_reader([str(i) for i in ids])
        print(f'OK   app table reader (save_table.h) resolves slots {LOG_SLOT} and {CACHE_SLOTS[0]} on the {model} layout')
    # The package's own rule: the one-pack layout puts the log and watch state at entries 6 and 7, and the
    # bitstream in release/ must be the released one (built from the fixed bridge).
    entries = save_entries(*slot_tables(['pack00.pjs']))
    ok = entries[LOG_SLOT] == 5 and entries[CACHE_SLOTS[0]] == 6
    failures += not ok
    print(f'{"OK  " if ok else "FAIL"} package layout: log entry {entries[LOG_SLOT]}, watch state entry {entries[CACHE_SLOTS[0]]}')
    try:
        origin = check_bitstream(reverse_bits(RBF_IN_RELEASE.read_bytes()), STAGE.parents[3])
        print(f'OK   package check: release bitstream is {origin}')
    except (ValueError, OSError, KeyError) as error:
        failures += 1
        print(f'FAIL package check: {error}')
    print('PASS' if not failures else f'{failures} FAILED')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
