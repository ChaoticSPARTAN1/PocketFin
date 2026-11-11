# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""RTL tests for PocketFin's own FPGA blocks (Icarus Verilog), PC only.

1. byte FIFO and link-cable byte engine (rtl/*_tb.sv);
2. the CPU-visible byte link (pocket_media_link_tb);
3. the JPEG register interface with the ported core_jpeg decoder (pocket_jpeg_mmio_tb), fed the
   app's own self-test frame (pocket/selftest_jpeg.h: a 256x144 FFmpeg testsrc2 picture). The RGB565
   output must have the CRC the app checks at every start.

Needs Icarus Verilog (POCKETFIN_IVERILOG), MSYS2 gcc, and build/pocket-media-core from
tools/prepare_pocket_media_core.py (for the ported decoder).
"""
import re
import subprocess
import sys
import zlib

import toolpaths
from toolpaths import ROOT, GCC, IVERILOG

OUT = toolpaths.BUILD / 'rtl-tests'
JPEG = ROOT / 'build/pocket-media-core/src/fpga/targets/pocket/pocket_media/jpeg'


def run(args, timeout=600):
    result = subprocess.run([str(a) for a in args], capture_output=True, env=toolpaths.msys_env(), timeout=timeout,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    text = (result.stdout + result.stderr).decode('utf-8', errors='replace')
    if result.returncode or 'FAIL' in text:
        raise SystemExit(f'FAIL: {" ".join(str(a) for a in args[:1])}\n{text[-3000:]}')
    return text


def bench(top, sources, extra=()):
    vvp = OUT / f'{top}.vvp'
    run([IVERILOG / 'bin/iverilog.exe', '-g2012', '-B', IVERILOG / 'lib/ivl', '-s', top, '-o', vvp,
         *[ROOT / 'rtl' / s for s in sources], *extra])
    return vvp


def simulate(vvp, *plus):
    return run([IVERILOG / 'bin/vvp.exe', '-M', IVERILOG / 'lib/ivl', vvp, *plus])


def selftest_frame():
    text = (ROOT / 'pocket/selftest_jpeg.h').read_text(encoding='ascii')
    width = int(re.search(r'PJ_SELFTEST_W (\d+)u', text).group(1))
    height = int(re.search(r'PJ_SELFTEST_H (\d+)u', text).group(1))
    crc = int(re.search(r'PJ_SELFTEST_CRC (0x[0-9a-f]+)u', text).group(1), 16)
    body = text[text.index('{') + 1:text.index('}')]
    data = bytes(int(v) for v in body.replace('\n', '').split(',') if v.strip())
    return data, width, height, crc


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    failures = 0
    for top, sources in (('byte_fifo_tb', ['byte_fifo.v', 'byte_fifo_tb.sv']),
                         ('gb_link_byte_tb', ['gb_link_byte.v', 'gb_link_byte_tb.sv']),
                         ('pocket_media_link_tb', ['byte_fifo.v', 'gb_link_byte.v', 'gb_link_stream.v',
                                                   'pocket_media_link.v', 'pocket_media_link_tb.sv'])):
        lines = simulate(bench(top, sources)).strip().splitlines()
        print(f'OK   {top}: {next((l for l in lines if "PASS" in l), lines[0])[:120]}', flush=True)
    if not JPEG.is_dir():
        raise SystemExit('Run tools/prepare_pocket_media_core.py first (it makes the ported JPEG decoder)')
    vvp = bench('pocket_jpeg_mmio_tb', ['async_word_fifo.v', 'pocket_jpeg_mmio.v', 'pocket_jpeg_mmio_tb.sv'],
                sorted(JPEG.glob('*.v')))
    data, width, height, crc = selftest_frame()
    hexfile, raw = OUT / 'selftest.hex', OUT / 'selftest-rgb565.raw'
    hexfile.write_text(''.join(f'{b:02x}\n' for b in data), encoding='ascii')
    jpg = OUT / 'selftest.jpg'
    jpg.write_bytes(data)
    result = simulate(vvp, f'+INPUT={hexfile}', f'+OUTPUT={raw}', f'+LENGTH={len(data)}', f'+WIDTH={width}',
                      f'+HEIGHT={height}', '+TRYPOP=1')
    got = zlib.crc32(raw.read_bytes())
    if got != crc:
        failures += 1
        print(f'FAIL pocket_jpeg_mmio_tb: CRC {got:08x}, the app expects {crc:08x}')
    else:
        print(f'OK   pocket_jpeg_mmio_tb: {width}x{height} self-test frame, CRC {crc:08x} as the app checks at start')
    print('PASS' if not failures else f'{failures} FAILED')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
