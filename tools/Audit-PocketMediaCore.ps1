# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
# After a -Fit build: writes timing reports for PocketFin's clock-crossing paths and checks them
# (tools/check_media_timing.py). Quartus folder: $env:POCKETFIN_QUARTUS.
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$quartus=if($env:POCKETFIN_QUARTUS){$env:POCKETFIN_QUARTUS}else{'C:/intelFPGA_lite/21.1/quartus'}
$target=Join-Path $root 'build/pocket-media-core/src/fpga/targets/pocket'
$script=@(
    'package require ::quartus::project',
    'package require ::quartus::sta',
    'package require ::quartus::sdc_ext',
    'project_open ap_core -revision ap_core',
    'create_timing_netlist',
    'read_sdc',
    'update_timing_netlist',
    'report_net_delay -nworst 100 -file media-gray-delays.rpt',
    'report_metastability -nchains 2000 -file media-metastability.rpt',
    'report_timing -setup -npaths 10 -detail full_path -file media-setup-paths.rpt',
    'report_timing -hold -npaths 10 -detail full_path -file media-hold-paths.rpt',
    'project_close'
)
[System.IO.File]::WriteAllLines((Join-Path $target 'media_audit.tcl'),$script,[System.Text.UTF8Encoding]::new($false))
Push-Location $target
try {
    cmd /c "`"$quartus/bin64/quartus_sta.exe`" -t media_audit.tcl > media-audit.log 2>&1"
    if($LASTEXITCODE){throw 'Timing audit failed; see media-audit.log'}
} finally {Pop-Location}
& py -3 "$root/tools/check_media_timing.py"
if($LASTEXITCODE){throw 'Timing check failed'}
