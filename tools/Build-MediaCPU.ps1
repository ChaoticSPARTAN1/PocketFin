# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
# Optional: regenerates the CPU (VexiiRiscv_media.v) from the pinned VexiiRiscv/SpinalHDL sources.
# Only needed if you change the CPU; core/generated/VexiiRiscv_media.v is the released one.
# First: py -3 tools/fetch_upstreams.py --cpu ; py -3 tools/prepare_pocket_media_core.py --cpu-sources
# Needs Git for Windows (bash), a Java 17 JDK in $env:POCKETFIN_JAVA_HOME (default local-tools\java\jdk-*)
# and internet access the first time (sbt downloads its own launcher and Scala libraries into local-tools\sbt).
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$bin=Join-Path $root 'local-tools\bin'
New-Item -ItemType Directory -Force -Path $bin | Out-Null
$python=(Get-Command py).Source.Replace('\','/')
$runner=(Join-Path $root 'tools/run_private_sbt.py').Replace('\','/')
$shim='#!/bin/bash'+"`n"+'exec "'+$python+'" -3 "'+$runner+'" "$@"'+"`n"
[System.IO.File]::WriteAllText((Join-Path $bin 'sbt'),$shim,[System.Text.UTF8Encoding]::new($false))
$env:PATH="$bin;C:\Program Files\Git\usr\bin;C:\Program Files\Git\bin;"+$env:PATH
$stage=Join-Path $root 'build/pocket-media-core/src/fpga/vendor/vexriscv'
Push-Location $stage
try {
    & 'C:/Program Files/Git/bin/bash.exe' ./generate_vexii.sh media *> cpu-build.log
    if($LASTEXITCODE){throw 'CPU generation failed; see cpu-build.log'}
} finally {Pop-Location}
Write-Output 'Generated VexiiRiscv_media.v. Compare it with core/generated/VexiiRiscv_media.v before building.'
