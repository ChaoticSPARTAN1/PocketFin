# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
# Builds the Pocket app (pjlive.elf) from pocket/ and shared/ with the RISC-V compiler.
# Needs: upstream/openfpgaSDK (py -3 tools/fetch_upstreams.py) and riscv64-unknown-elf-gcc 15.1
# (folder in $env:POCKETFIN_RISCV_BIN, default local-tools\riscv\ucrt64\bin).
# Output: build/pocket-live-player/pjlive.elf plus build-receipt.json (hashes of every source used).
# A diagnostic build names its own folder and defines, e.g. -OutDir build/pjlive-ack256 -Defines 'PJ_LINK_ACKS=1'
param([string]$OutDir='build/pocket-live-player',[string]$Defines='')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$tool=if($env:POCKETFIN_RISCV_BIN){$env:POCKETFIN_RISCV_BIN}else{Join-Path $root 'local-tools/riscv/ucrt64/bin'}
$msys=if($env:POCKETFIN_MSYS){$env:POCKETFIN_MSYS}else{'C:\msys64'}
$sdk=Join-Path $root 'upstream/openfpgaSDK/src/sdk'
if(!(Test-Path -LiteralPath "$sdk/app.ld")){throw 'upstream/openfpgaSDK is missing: run py -3 tools/fetch_upstreams.py'}
$gcc=Join-Path $tool 'riscv64-unknown-elf-gcc.exe'
if(!(Test-Path -LiteralPath $gcc)){throw "RISC-V gcc not found in $tool (set POCKETFIN_RISCV_BIN)"}
$out=Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:PATH="$tool;$msys\ucrt64\bin;"+$env:PATH
$defineList=@($Defines -split ';' | Where-Object {$_} | ForEach-Object {"-D$_"})
$flags=@('-march=rv32imafc','-mabi=ilp32f','-O2','-Wall','-Wextra','-Werror',
    '-ffunction-sections','-fdata-sections','-nostdinc',"-I$sdk/include",'-isystem',"$sdk/musl/include")+$defineList
foreach($name in @('live_player','live_session')) {
    & $gcc @flags -c "$root/pocket/$name.c" -o "$out/$name.o"
    if($LASTEXITCODE){throw 'App compile failed'}
}
& $gcc @flags -c "$sdk/of_init.c" -o "$out/of_init.o"
if($LASTEXITCODE){throw 'SDK start-up compile failed'}
& $gcc -march=rv32imafc -mabi=ilp32f -nostdlib -static -T "$sdk/app.ld" "-L$sdk/musl/lib" '-Wl,--gc-sections' "$sdk/musl/lib/crt1.o" "$sdk/musl/lib/crti.o" "$out/live_player.o" "$out/live_session.o" "$out/of_init.o" "$sdk/musl/lib/crtn.o" '-Wl,--start-group' -lc -lgcc '-Wl,--end-group' -o "$out/pjlive.elf"
if($LASTEXITCODE){throw 'App link failed'}
& "$tool/riscv64-unknown-elf-size.exe" "$out/pjlive.elf"
$sources=@("$root/pocket/live_player.c","$root/pocket/live_session.c","$sdk/of_init.c","$sdk/app.ld")
$sources+=@(Get-ChildItem "$root/pocket/*.h","$root/shared/*.h" -File | Select-Object -ExpandProperty FullName)
$hashes=[ordered]@{}
foreach($path in $sources){$hashes[$path.Substring($root.Length+1).Replace('\','/')]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
$receipt=[ordered]@{app_sha256=(Get-FileHash -LiteralPath "$out/pjlive.elf" -Algorithm SHA256).Hash.ToLowerInvariant();source_sha256=$hashes}
if($defineList.Count){$receipt['defines']=$defineList;$receipt['diagnostic_build']=$true}
$receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$out/build-receipt.json" -Encoding utf8
Write-Output "Built $out\pjlive.elf (sha256 $($receipt.app_sha256)). Nothing was copied to a card."
