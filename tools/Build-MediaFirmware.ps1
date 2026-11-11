# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
# Optional: rebuilds the core's boot ROM image (firmware.mif) and the paired OS kernel (os.bin) from
# build/pocket-media-core with the RISC-V compiler, replacing the copies from core/generated/.
# Needs MSYS2 (bash, make; $env:POCKETFIN_MSYS, default C:\msys64), riscv64-unknown-elf-gcc
# ($env:POCKETFIN_RISCV_BIN) and upstream/openfpgaSDK for musl libc.
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$msys=if($env:POCKETFIN_MSYS){$env:POCKETFIN_MSYS}else{'C:\msys64'}
$tools=if($env:POCKETFIN_RISCV_BIN){$env:POCKETFIN_RISCV_BIN}else{Join-Path $root 'local-tools/riscv/ucrt64/bin'}
$stage=Join-Path $root 'build/pocket-media-core'
$sdk=Join-Path $root 'upstream/openfpgaSDK/src/sdk'
$musl=Join-Path $stage 'src/firmware/musl'
New-Item -ItemType Directory -Force -Path "$musl/lib" | Out-Null
Copy-Item -LiteralPath "$sdk/musl/include" -Destination "$musl" -Recurse -Force
Copy-Item -LiteralPath "$sdk/musl/lib/libc.a" -Destination "$musl/lib/libc.a" -Force
$env:PATH="$tools;$msys\ucrt64\bin;$msys\usr\bin;"+$env:PATH
Push-Location $stage
try {
    # The link registers changed meaning, so FPGA and kernel carry contract 0x504a0001
    # (set by prepare_pocket_media_core.py); --bless records that deliberate change.
    & "$msys/usr/bin/bash.exe" tools/contract-check.sh --bless *> firmware-contract.log
    if($LASTEXITCODE){throw 'Contract check failed; see firmware-contract.log'}
    & "$msys/usr/bin/bash.exe" -c 'make -C src/firmware/os -j2 TARGET=pocket CROSS=riscv64-unknown-elf- install os.bin' *> firmware-build.log
    if($LASTEXITCODE){throw 'Boot ROM / kernel build failed; see firmware-build.log'}
} finally {Pop-Location}
Write-Output 'Built firmware.mif and os.bin in build/pocket-media-core. Nothing was copied to a card.'
