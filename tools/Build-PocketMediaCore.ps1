# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
# Builds the PocketFin FPGA core (ap_core.rbf) with Intel Quartus Prime Lite 21.1.1.
# First run: py -3 tools/fetch_upstreams.py ; py -3 tools/prepare_pocket_media_core.py
# Quartus folder: $env:POCKETFIN_QUARTUS (default C:\intelFPGA_lite\21.1\quartus).
# -Fit also runs place-and-route, timing analysis and the assembler (about 20-60 minutes).
# Output: build/pocket-media-core/src/fpga/targets/pocket/output_files/ap_core.rbf
# Nothing is copied to a card; tools/package_release.py turns the rbf into the Pocket's os25.rbf_r.
param([switch]$Fit)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$quartus=if($env:POCKETFIN_QUARTUS){$env:POCKETFIN_QUARTUS}else{'C:/intelFPGA_lite/21.1/quartus'}
$stage=Join-Path $root 'build/pocket-media-core'
$target=Join-Path $stage 'src/fpga/targets/pocket'
foreach($required in @("$stage/src/fpga/vendor/vexriscv/VexiiRiscv/VexiiRiscv_media.v","$target/firmware.mif","$stage/src/firmware/os/bld/pocket/os.bin")) {
    if(!(Test-Path -LiteralPath $required)){throw 'Run py -3 tools/prepare_pocket_media_core.py first'}
}
$script=@(
    'package require ::quartus::project',
    'package require ::quartus::flow',
    'project_open ap_core -revision ap_core',
    'execute_module -tool map'
)
if($Fit){$script+=@('execute_module -tool fit','execute_module -tool sta','execute_module -tool asm')}
$script+='project_close'
[System.IO.File]::WriteAllLines((Join-Path $target 'media_build.tcl'),$script,[System.Text.UTF8Encoding]::new($false))
Push-Location $target
try {
    # cmd owns the redirect: Windows PowerShell 5.1 would turn Quartus' harmless stderr notices into errors.
    cmd /c "`"$quartus/bin64/quartus_sh.exe`" -t media_build.tcl > media-build.log 2>&1"
    if($LASTEXITCODE){throw 'Quartus build failed; see media-build.log in the target folder'}
} finally {Pop-Location}
if($Fit){
    $rbf="$target/output_files/ap_core.rbf"
    Write-Output "Built $rbf (sha256 $((Get-FileHash -LiteralPath $rbf -Algorithm SHA256).Hash.ToLowerInvariant()))."
    Write-Output 'Check timing with tools/Audit-PocketMediaCore.ps1 before using it.'
} else { Write-Output 'Synthesis (map) finished. Add -Fit for a bitstream.' }
