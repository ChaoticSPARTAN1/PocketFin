# Offline library on the card

[Home](Home.md) > Offline library

- [How it works](#how-it-works)
- [Add your own videos](#add-your-own-videos)
- [Copy titles from your Jellyfin server](#copy-titles-from-your-jellyfin-server)
- [Where the files go on the card](#where-the-files-go-on-the-card)
- [Limits](#limits)
- [Troubleshooting](#troubleshooting)

Only add videos you own or have the right to copy.

## How it works

The Pocket cannot play ordinary video files. `tools/make_offline_library.py` converts them on your PC
into the Pocket's format: 256x144 Motion-JPEG pictures at the video's own frame rate with IMA ADPCM
sound. It writes a **library** file (Home rows, titles, descriptions, pictures) and one or more
**pack** files (the videos). The Pocket's FPGA decodes the pictures.

You need Python, FFmpeg and PyAV on the PC: see [What you need](What-You-Need.md).

## Add your own videos

1. Open a terminal (PowerShell) in the PocketFin folder.
2. Run, with your own file names:

   ```powershell
   py -3 tools\make_offline_library.py --video "C:\Videos\My Video.mp4" --video "C:\Videos\Holiday 2024.mkv"
   ```

   Any format FFmpeg can read works (MP4, MKV, AVI, MOV, ...). The title shown on the Pocket is the
   file name without its extension. A picture from each video becomes its poster.
3. Wait: conversion takes roughly as long as the video, or less on a fast PC. It prints a summary at
   the end.
4. The result is in `build\offline-library\`. Copy **everything inside it** onto the root of the
   Pocket's SD card and let it replace the files it asks about.
5. Start the Jellyfin core, press **A**: your videos are in the **On this Pocket** tile in My Media.

A 60-second **Test Pattern** is added too, so the card always has something to play; add
`--no-test-pattern` to leave it out.

Each run makes a **complete new library**: list every video you want on the card, not only new ones.
The new library replaces the old one on the card.

## Copy titles from your Jellyfin server

1. Copy `jellyfin-connection.example.txt` to `private\jellyfin-connection.txt` and fill in your
   server address (for example `http://your-server:8096`), user name and password, or an API key.
   This file stays on your PC; Git ignores the `private` folder.
2. Run:

   ```powershell
   py -3 tools\make_offline_library.py --jellyfin --max-titles 4 --max-gb 8
   ```

   It takes titles from Continue Watching first, then the newest, up to the limits, and also stores
   your Home rows, folders, posters and descriptions. Titles that are listed but not copied show
   **Not on this Pocket**. You can combine `--jellyfin` with `--video`.
3. Copy `build\offline-library\` to the card as above.

The tool only reads from your server. It never changes watched status or playback positions there,
and it prints only counts, never titles, addresses or tokens.

## Where the files go on the card

| On the card | What it is |
|---|---|
| `Assets/pocketlive/common/library.pjl` | The library (titles, rows, pictures) |
| `Assets/pocketlive/common/pack00.pjs` ... `pack11.pjs` | The videos |
| `Assets/pocketlive/ChaoticSPARTAN1.Jellyfin/Jellyfin.json` | The list of files the Pocket loads for this core |

The Pocket loads only the packs listed in `Jellyfin.json`, so always copy it together with the packs.
Old pack files that the new list does not name are ignored and can be deleted to save space.

## Limits

- Up to **256 titles** and **12 packs** of up to **2 GiB** each (about 24 hours of video in total;
  about 0.9 GB per hour).
- One video must fit in one pack (up to about 2 hours 15 minutes).
- Frame rates: 23.976, 24, 25, 29.97 and 30 fps are kept as they are; 50 and 60 fps videos are halved.
- Subtitles are not shown. One audio track is used (the one FFmpeg picks by default).

## Troubleshooting

| Problem | What to do |
|---|---|
| `ffmpeg` or `ffprobe` not found | Install FFmpeg and open a new terminal; `ffmpeg -version` must work |
| `No module named 'av'` | `py -3 -m pip install av` |
| "Could not convert video N; skipped" | FFmpeg cannot read that file, or it has no video stream. Try playing it on the PC, or convert it to MP4 first |
| "Title exceeds a pack" | The video is too long for one pack; split it into parts |
| The Pocket shows only the old titles | `Jellyfin.json` was not copied, or the card was not ejected safely. Copy the whole `build\offline-library\` again |
| The core shows a black screen after copying | See [Troubleshooting](Troubleshooting.md); put back the files from your card backup |

Next: [Troubleshooting and recovery](Troubleshooting.md)
