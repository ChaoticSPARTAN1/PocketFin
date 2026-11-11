# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""After a build: write build/pocket-media-core/media-build-receipt.json (hashes of sources, outputs and reports)."""
import hashlib
import json
from pathlib import Path

from toolpaths import ROOT


def record():
    stage=ROOT/'build/pocket-media-core'
    files=[stage/'src/fpga/common/axi_periph_slave.v',stage/'src/firmware/os/hal/regs.h',
        stage/'src/firmware/os/hal/link.c',stage/'src/firmware/os/hal/net.c',
        stage/'src/fpga/targets/pocket/core_top.v',stage/'src/fpga/targets/pocket/ap_core.qsf',
        stage/'src/fpga/targets/pocket/core_bridge_cmd.v',   # exit-save fix (pocket_save_exit_patch.py)
        stage/'src/fpga/targets/pocket/output_files/ap_core.rbf',
        stage/'src/fpga/vendor/vexriscv/configs/media.cfg',
        stage/'src/fpga/vendor/vexriscv/VexiiRiscv/VexiiRiscv_media.v',
        stage/'src/fpga/targets/pocket/firmware.mif',
        stage/'src/firmware/os/bld/pocket/firmware.elf',stage/'src/firmware/os/bld/pocket/os.bin']
    files+=sorted((stage/'src/fpga/targets/pocket/pocket_media').rglob('*.v'))
    files+=sorted((stage/'src/fpga/targets/pocket/pocket_media').glob('*.sdc'))
    report={'hardware_accessed':False,'programmer_image_assembled':False,'installed':False,
        'hardware_contract':'0x504a0001','cpu_clock_hz':90000000,'jpeg_clock_hz':24576000,
        'artifacts':{str(p.relative_to(stage)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
        'compiler':'Quartus Lite 21.1.1 Build 850','physical_acceptance':False}
    for kind in ('map','fit','sta'):
        p=stage/f'src/fpga/targets/pocket/output_files/ap_core.{kind}.summary'
        report[kind+'_summary']=p.read_text(encoding='utf-8',errors='replace') if p.exists() else None
    folder=stage/'src/fpga/targets/pocket'
    audit=folder/'media-timing-audit.json'
    report['timing_audit']=json.loads(audit.read_text(encoding='utf-8')) if audit.exists() else None
    reports=[folder/'media-gray-delays.rpt',folder/'media-metastability.rpt',
        folder/'media-setup-paths.rpt',folder/'media-hold-paths.rpt',
        folder/'media-final-fit.log',folder/'media-audit.log']
    report['report_sha256']={str(p.relative_to(stage)):hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in reports if p.exists()}
    (stage/'media-build-receipt.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('Build receipt written: build/pocket-media-core/media-build-receipt.json')


if __name__=='__main__':record()
