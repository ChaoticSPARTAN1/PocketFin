# Building from source

[Home](Home.md) > Building from source

- [What is in the repository](#what-is-in-the-repository)
- [Tools](#tools)
- [1. Fetch the upstream sources](#1-fetch-the-upstream-sources)
- [2. Build the app](#2-build-the-app)
- [3. Build the FPGA core](#3-build-the-fpga-core)
- [4. Make the release](#4-make-the-release)
- [5. Run the tests](#5-run-the-tests)

You do not need any of this to use PocketFin: `release/` is ready to copy. These steps rebuild every
file in it from source.

## What is in the repository

| Folder | Contents |
|---|---|
| `pocket/` | The app (C): screens, library reader, playback, link-cable client, and PC test programs (`check_*.c`) |
| `shared/` | Headers shared with Jellymatic: wire framing, the request protocol, the A/V stream format |
| `rtl/` | PocketFin's FPGA blocks (link-cable byte engine, JPEG register interface, FIFOs) and their test benches |
| `core/` | `expected-sources.json` (the core's checked source list) and `generated/` (the CPU, boot ROM and kernel the release was built with) |
| `tools/` | Build, packaging, library and test scripts (Python and PowerShell) |
| `third_party/` | The Jellyfin logo files (CC BY-SA 4.0) |
| `release/` | The ready-to-copy SD card files |

Big upstream projects are not copied in. `tools/fetch_upstreams.py` fetches them at the exact
revisions in `upstream.lock.json`.

## Tools

Windows 10/11 is assumed. Paths can be changed with environment variables (see `tools/toolpaths.py`).

| Tool | Needed for | Where it is looked for |
|---|---|---|
| Git, Python 3.12+ | everything | PATH (`py -3`) |
| RISC-V GCC 15.1 (`riscv64-unknown-elf-gcc`, MSYS2 UCRT64 packages listed in `upstream.lock.json`) | the app, the kernel | `POCKETFIN_RISCV_BIN` (default `local-tools\riscv\ucrt64\bin`) |
| MSYS2 (gcc, make, bash, libjpeg) | PC tests, kernel build | `POCKETFIN_MSYS` (default `C:\msys64`) |
| Intel Quartus Prime Lite 21.1.1 (free) | the FPGA core | `POCKETFIN_QUARTUS` (default `C:\intelFPGA_lite\21.1\quartus`) |
| Icarus Verilog 13 | RTL tests | `POCKETFIN_IVERILOG` (folder with `bin\iverilog.exe`) |
| FFmpeg 7+ and PyAV | demo library, offline library, logo images | PATH; `py -3 -m pip install av` |
| fontTools | only to regenerate the font atlas | `py -3 -m pip install fonttools` |
| Java 17 + sbt (downloaded by the script) | only to regenerate the CPU | `POCKETFIN_JAVA_HOME` |

## 1. Fetch the upstream sources

```powershell
py -3 tools\fetch_upstreams.py          # openfpgaSDK, openfpgaCore, core_jpeg into upstream\
```

## 2. Build the app

```powershell
.\tools\Build-LivePlayer.ps1            # -> build\pocket-live-player\pjlive.elf
```

With the pinned compiler the result is byte-identical to the released app (SHA-256 starts
`e8ed291c`, see `release/SHA256SUMS.txt`).

The app's font is generated from Google's Roboto font. To regenerate it (optional; needs fontTools,
`py -3 -m pip install fonttools`):

```powershell
py -3 tools\fetch_upstreams.py --font   # Roboto 2.138, checked by SHA-256 -> upstream\roboto\
py -3 tools\build_roboto_atlas.py       # -> pocket\fonts\roboto-medium-18.a8 and .json
py -3 tools\build_ui_font.py            # -> pocket\ui_font.h and pocket\ui_font_large.h
```

## 3. Build the FPGA core

```powershell
py -3 tools\prepare_pocket_media_core.py    # pinned openfpgaCore + PocketFin's changes -> build\pocket-media-core
.\tools\Build-PocketMediaCore.ps1 -Fit      # Quartus: 20-60 minutes -> ap_core.rbf
.\tools\Audit-PocketMediaCore.ps1           # checks timing of the clock-crossing paths
```

`prepare_pocket_media_core.py` applies every change with exact-text checks, then compares the 43
files PocketFin adds or changes with `core/expected-sources.json` (the tree the released bitstream was
built from; PocketFin's own RTL files have since changed in comments only). What PocketFin changes in
the runtime:

- the 33-bit link block is replaced by `pocket_media_accel` (byte link + JPEG decoder) on a 90 MHz CPU
  clock; the JPEG decoder runs on the existing 24.576 MHz clock;
- the register window grows by one address bit; the hardware/kernel contract number becomes
  `0x504a0001`, and the old link and network drivers stand down on this core;
- an upstream AXI write-address bug is fixed (the first delayed write beat was advanced by 4 bytes);
- the save fix in `core_bridge_cmd.v` keeps save-slot entries when the Pocket resets the core at quit
  (`tools/pocket_save_exit_patch.py`);
- a smaller single-issue CPU configuration (16 KiB / 64 KiB caches) and fitter seed 9.

The CPU (`VexiiRiscv_media.v`), boot ROM (`firmware.mif`) and kernel (`os.bin`) come from
`core/generated/`. To rebuild them as well: `tools\fetch_upstreams.py --cpu`,
`prepare_pocket_media_core.py --cpu-sources`, `tools\Build-MediaCPU.ps1`, `tools\Build-MediaFirmware.ps1`.

## 4. Make the release

```powershell
py -3 tools\build_brand_assets.py       # Jellyfin logo -> core icon and platform image (build\brand)
py -3 tools\build_sample_library.py     # 20-second generated test pattern (build\sample-library)
py -3 tools\package_release.py          # -> release\SD, SD-sample-library, SD-first-install-saves, SHA256SUMS.txt
```

`package_release.py` uses your own `ap_core.rbf` if you built one, otherwise the released bitstream.
It refuses an `interact.json` other than the proven SDK template and checks the save-slot layout and
the Chip32 loader.

## 5. Run the tests

All tests run on the PC; none touch a Pocket or a card.

```powershell
py -3 tools\test_pocket_loader.py       # the loader writes saves back at quit
py -3 tools\test_link_gateway.py        # request/reply protocol, no secrets in replies
py -3 tools\test_stream_worker.py       # streaming worker
py -3 tools\test_av_stream.py           # A/V stream format
py -3 tools\test_live_av.py             # encoder (FFmpeg + PyAV)
py -3 tools\test_sd_library.py          # on-card library end to end through the C reader
py -3 tools\test_native_client.py       # the app's controller and screens; writes build\native-client\ui-*.png
py -3 tools\test_pocket_save_exit.py    # the save fix on the real bridge RTL (Icarus; Quartus sim library optional)
py -3 tools\test_rtl.py                 # link byte engine, FIFOs, JPEG decoder on the app's self-test frame (Icarus)
```

`test_sd_library.py` needs `build\sample-library` (step 4). `test_pocket_save_exit.py` and
`test_rtl.py` need `build\pocket-media-core` (step 3, prepare only).

Next: [AI prompt examples](AI-Prompt-Examples.md)
