# What you need

[Home](Home.md) > What you need

- [To watch videos from the card](#to-watch-videos-from-the-card)
- [To add your own videos](#to-add-your-own-videos)
- [To use your Jellyfin server live](#to-use-your-jellyfin-server-live)

## To watch videos from the card

- An **Analogue Pocket** with current firmware (openFPGA support; firmware 2.2 or newer).
- Its **SD card**, and a way to copy files to it: a card reader on your computer, or the Pocket's own
  USB card access if your firmware offers it.
- About **5 MB** free for the core and demo, plus room for your videos (about 0.9 GB per hour).
- The PocketFin card files: **`PocketFin-SD-<version>.zip`** from the
  [Releases page](https://github.com/ChaoticSPARTAN1/PocketFin/releases/latest) (the same files as
  `release/` in this repository; see [Install](Install.md)).

That is all. No network, no Chromatic, no server.

## To add your own videos

A Windows PC (other systems work for the tools, but the steps here are written for Windows) with:

- **Python 3.12 or newer**: https://www.python.org/downloads/ (tick "Add python.exe to PATH" when
  installing). The commands below use `py -3`.
- **FFmpeg** (version 7 or newer) with `ffmpeg.exe` and `ffprobe.exe` on your PATH. One easy way:
  `winget install Gyan.FFmpeg`, then open a new terminal. FFmpeg is separate software under its own
  licence (LGPL or GPL depending on the build); PocketFin does not include it.
- **PyAV**: `py -3 -m pip install av`.

See [Offline library](Offline-Library.md).

## To use your Jellyfin server live

- A **Jellyfin server** at home (free; see [Connecting to Jellyfin wirelessly](Wireless-Jellyfin.md)).
- A **ModRetro Chromatic** with **Jellymatic** installed: https://github.com/ChaoticSPARTAN1/Jellymatic
- A **link cable** that fits the link ports of both consoles.
- A 2.4 GHz Wi-Fi network for the Chromatic.

Next: [Install](Install.md)
