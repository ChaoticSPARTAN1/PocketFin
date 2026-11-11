# Install on the Pocket

[Home](Home.md) > Install

- [Download the release](#download-the-release)
- [What is in release/](#what-is-in-release)
- [Install by copying files](#install-by-copying-files)
- [Install with the Windows script](#install-with-the-windows-script)
- [Update to a new version](#update-to-a-new-version)
- [Remove PocketFin](#remove-pocketfin)

PocketFin adds its own folders to the card. It does not change other cores, games, saves or settings.

## Download the release

1. Open the [Releases page](https://github.com/ChaoticSPARTAN1/PocketFin/releases/latest) (the latest
   PocketFin release).
2. Download **`PocketFin-SD-<version>.zip`** and unzip it anywhere on your computer.
3. Inside are the folders `SD/`, `SD-sample-library/` and `SD-first-install-saves/`, a `README.txt` and
   `SHA256SUMS.txt`. They are the same files as `release/` in this repository, so the steps below work
   with either: where they say `release/SD/`, use the unzipped `SD/` folder, and so on.

To use the Windows script with the download, unzip it into a folder named `release` next to
`Install-PocketFin.ps1` (in a downloaded copy of this repository), replacing the files there.

## What is in release/

| Folder | What it holds | When to copy it |
|---|---|---|
| `release/SD/` | The core (`Cores/ChaoticSPARTAN1.Jellyfin/`), the app and OS (`Assets/pocketlive/common/`), the platform entry and the licence notices | First install and every update |
| `release/SD-sample-library/` | The demo library (a 20-second test pattern) and the file that tells the Pocket which library files to load | First install only, or to go back to the demo |
| `release/SD-first-install-saves/` | Empty save files for the app's log and offline cache (`Saves/pocketlive/common/`) | Only if your card does not have them yet |

`release/SHA256SUMS.txt` lists a checksum for every file.

## Install by copying files

1. Turn the Pocket off and take out the SD card (or use the Pocket's USB card access). Put the card in
   your computer.
2. **Back up the card** (copy everything on it to a folder on your computer). This is your way back
   if anything goes wrong.
3. Open `release/SD/` and copy everything inside it (the folders `Assets`, `Cores`, `Notices`,
   `Platforms`) onto the root of the card. Say yes if asked to merge folders.
4. **First install only (skip if the card already has them):** do the same with everything inside
   `release/SD-sample-library/`.
5. **First install only (skip if the card already has them):** do the same with everything inside
   `release/SD-first-install-saves/`, but **skip any file that already exists** (choose "Skip these
   files"). Never replace existing saves.
6. Eject the card safely, put it back in the Pocket and see [First run](First-Run.md).

Do not change any file in `Cores/ChaoticSPARTAN1.Jellyfin/`. In particular, never edit
`interact.json`: other versions of that file made the Pocket hang at a black screen.

## Install with the Windows script

`Install-PocketFin.ps1` does the same steps and checks every file:

```powershell
.\Install-PocketFin.ps1 -Card E:\            # use your card's drive letter
.\Install-PocketFin.ps1 -Card E:\ -WhatIf    # only show what it would do
```

It checks the release against `SHA256SUMS.txt`, copies `release/SD/`, adds the demo library only if
the card has no PocketFin library, creates save files only where none exist, and reads every copied
file back to check it. Any card file it replaces is first copied to `backups\<date-time>\` next to
the script. It never needs a password or address and sends nothing anywhere.

If Windows refuses to run the script, run this once in the same window first:
`Set-ExecutionPolicy -Scope Process Bypass`.

## Update to a new version

Download the new `PocketFin-SD-<version>.zip` from the
[Releases page](https://github.com/ChaoticSPARTAN1/PocketFin/releases/latest), then copy only its `SD/`
folder (or run the script again). Your library, your saves and the list of library
files stay as they are. Do not copy `SD-sample-library` over your own library unless you want the
demo back.

## Remove PocketFin

By hand, delete these from the card:

- `Cores/ChaoticSPARTAN1.Jellyfin/`
- `Assets/pocketlive/ChaoticSPARTAN1.Jellyfin/`
- `os.bin`, `pjlive.elf` and `pjlive.ini` in `Assets/pocketlive/common/`
- `Platforms/pocketlive.json` and `Platforms/_images/pocketlive.bin`
- `Notices/PocketLive/`
- Optional, your library and saves: the rest of `Assets/pocketlive/common/` and `Saves/pocketlive/`

Or run `.\Install-PocketFin.ps1 -Card E:\ -Remove` (add `-RemoveLibraryAndSaves` to remove those too).
The script moves everything to `backups\` on your PC instead of deleting it.

Next: [First run](First-Run.md)
