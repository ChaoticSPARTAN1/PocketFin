# PocketFin 1.0.0 SD card release

Copy these folders onto the root of your Analogue Pocket's SD card (see ../docs/Install.md):

- `SD/` - the core and app. First install and every update.
- `SD-sample-library/` - the demo library. First install only (skip if the card already has a
  library: it replaces the card's own library).
- `SD-first-install-saves/` - empty save files. First install only (skip if the card already has
  them; never copy over existing saves).

Or run `..\Install-PocketFin.ps1 -Card E:\`.

`SHA256SUMS.txt` lists every file's checksum. Built by `tools/package_release.py`; the bitstream is
openfpgaCore 618a3eb plus PocketFin's changes (ap_core.rbf SHA-256 7c664090...), the app is
pjlive.elf SHA-256 e8ed291c... built from `pocket/` (the previous, device-tested build was a19aa677...;
this one changes only the About screen's text and the font atlas's source). Never edit `Cores/ChaoticSPARTAN1.Jellyfin/interact.json`.
