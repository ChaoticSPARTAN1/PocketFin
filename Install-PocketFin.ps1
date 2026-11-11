# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md

<#
.SYNOPSIS
  Copies PocketFin (the "Jellyfin" core) from release\ to an Analogue Pocket SD card, or removes it.

.DESCRIPTION
  What it does, in order:
    1. Checks every release file against release\SHA256SUMS.txt.
    2. Copies release\SD\ to the card (the core, the app, the notices). A card file that would be
       replaced is first copied to backups\<date-time>\ on this PC.
    3. Only if the card has no PocketFin library yet: copies the demo library (release\SD-sample-library\).
    4. Only if the card has none: copies the empty save files (release\SD-first-install-saves\).
       Existing saves are never replaced.
    5. Reads every copied file back from the card and compares it with the release.
  It touches nothing else on the card: other cores, games, saves and settings stay as they are.
  It needs no passwords or addresses and sends nothing anywhere.

  -Remove takes PocketFin off the card instead: its core folder, app files, platform files and instance file are
  moved to backups\<date-time>\ on this PC (not deleted). Your offline library and saves stay unless
  you also pass -RemoveLibraryAndSaves (they are backed up the same way).

.EXAMPLE
  .\Install-PocketFin.ps1 -Card E:\
.EXAMPLE
  .\Install-PocketFin.ps1 -Card E:\ -WhatIf        # show what would happen, change nothing
.EXAMPLE
  .\Install-PocketFin.ps1 -Card E:\ -Remove
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)][string]$Card,
    [switch]$ReplaceLibrary,          # put the demo library back even if the card has its own
    [switch]$Remove,
    [switch]$RemoveLibraryAndSaves,
    [switch]$NotAPocketCard           # skip the check for the Pocket's own files on the card
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$release = Join-Path $here 'release'
$core = 'ChaoticSPARTAN1.Jellyfin'

if ($Card -notmatch '^[A-Za-z]:\\?$') { throw 'Give the card as a drive letter, for example -Card E:\' }
$root = [IO.Path]::GetFullPath($Card.Substring(0,2) + '\')
if (-not (Test-Path -LiteralPath $root)) { throw "Drive $root is not there. Is the card connected?" }
if (-not $NotAPocketCard -and -not (Test-Path -LiteralPath (Join-Path $root 'Cores')) -and
    -not (Test-Path -LiteralPath (Join-Path $root 'Analogue_Pocket.json'))) {
    throw "$root does not look like an Analogue Pocket card (no Cores folder). Pass -NotAPocketCard if you are sure."
}
$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
$backup = Join-Path $here "backups\$stamp"

function Get-Sha256($path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }

function Save-Backup($cardPath) {
    $relative = $cardPath.Substring($root.Length)
    $target = Join-Path $backup $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $cardPath -Destination $target -Recurse -Force
}

if ($Remove) {
    $paths = @("Cores\$core", 'Platforms\pocketlive.json', 'Platforms\_images\pocketlive.bin', "Assets\pocketlive\$core", 'Notices\PocketLive',
                'Assets\pocketlive\common\os.bin', 'Assets\pocketlive\common\pjlive.elf', 'Assets\pocketlive\common\pjlive.ini')
    if ($RemoveLibraryAndSaves) { $paths += @('Assets\pocketlive\common', 'Saves\pocketlive') }
    foreach ($relative in $paths) {
        $path = Join-Path $root $relative
        if (-not (Test-Path -LiteralPath $path)) { continue }
        if ($PSCmdlet.ShouldProcess($path, 'Back up to this PC, then remove from the card')) {
            Save-Backup $path
            Remove-Item -LiteralPath $path -Recurse -Force
            Write-Output "Removed $relative (backup in $backup)"
        }
    }
    Write-Output 'PocketFin removed. Eject the card safely before taking it out.'
    return
}

# 1. Check the release itself.
$sums = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $release 'SHA256SUMS.txt')) {
    if ($line -match '^([0-9a-f]{64})  (.+)$') { $sums[$Matches[2]] = $Matches[1] }
}
foreach ($entry in $sums.GetEnumerator()) {
    $file = Join-Path $release $entry.Key
    if (-not (Test-Path -LiteralPath $file) -or (Get-Sha256 $file) -ne $entry.Value) {
        throw "Release file $($entry.Key) is missing or damaged. Download the release again."
    }
}

function Copy-Folder($folder, $onlyIfMissing) {
    $base = Join-Path $release $folder
    $copied = 0
    foreach ($file in Get-ChildItem -LiteralPath $base -Recurse -File) {
        $relative = $file.FullName.Substring($base.Length + 1)
        $target = Join-Path $root $relative
        $key = ($folder + '/' + $relative).Replace('\', '/')
        if ($onlyIfMissing -and (Test-Path -LiteralPath $target)) { continue }
        if ((Test-Path -LiteralPath $target) -and (Get-Sha256 $target) -eq $sums[$key]) { continue }
        if (-not $PSCmdlet.ShouldProcess($target, 'Copy from the release')) { continue }
        if (Test-Path -LiteralPath $target) { Save-Backup $target }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        $in = [IO.File]::OpenRead($file.FullName)
        try {
            $out = [IO.File]::Open($target, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try { $in.CopyTo($out, 1048576); $out.Flush($true) } finally { $out.Dispose() }
        } finally { $in.Dispose() }
        if ((Get-Sha256 $target) -ne $sums[$key]) { throw "Read-back check failed for $relative. Try another card reader or card." }
        $copied++
    }
    return $copied
}

# 2. The core and the app.
$n = Copy-Folder 'SD' $false
Write-Output "Core and app: $n file(s) copied and checked."
# 3. The demo library, only on a card without a PocketFin library (or when asked).
$library = Join-Path $root 'Assets\pocketlive\common\library.pjl'
if ($ReplaceLibrary -or -not (Test-Path -LiteralPath $library)) {
    $n = Copy-Folder 'SD-sample-library' $false
    Write-Output "Demo library: $n file(s) copied and checked."
} else {
    Write-Output 'Your card already has a PocketFin library; it was left as it is.'
}
# 4. Empty save files, never over existing ones.
$n = Copy-Folder 'SD-first-install-saves' $true
Write-Output "Save files: $n created (existing saves were kept)."
if (Test-Path -LiteralPath $backup) { Write-Output "Replaced card files were backed up to $backup" }
Write-Output 'Done. Eject the card safely, put it in the Pocket, then open Openfpga > Jellyfin.'
