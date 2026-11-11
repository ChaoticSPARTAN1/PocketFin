# Troubleshooting and recovery

[Home](Home.md) > Troubleshooting and recovery

- [Golden rules](#golden-rules)
- [Black screen with a flashing backlight](#black-screen-with-a-flashing-backlight)
- [Go back to the previous files](#go-back-to-the-previous-files)
- [Other problems](#other-problems)
- [Remove the core cleanly](#remove-the-core-cleanly)
- [Reporting a bug](#reporting-a-bug)

PocketFin runs only while you have the core open. It cannot change the Pocket's firmware, and quitting
the core or switching the Pocket off always returns it to normal.

## Golden rules

- **Back up the card** before installing or updating.
- **Never edit `interact.json`** (or other files in `Cores/ChaoticSPARTAN1.Jellyfin/`). Changed
  versions of that file made the Pocket stop at a black screen during core start.
- Eject the card safely before taking it out of the computer.

## Black screen with a flashing backlight

If the core stops at a black screen and the backlight flashes:

1. **Switch the Pocket off fully**: hold the power button until it turns off (about 10 seconds). A
   short press may not be enough.
2. Switch it on again. The Pocket starts normally; other cores are not affected.
3. Put back the PocketFin files you had before (next section), or reinstall `release/SD/` from a
   fresh download. Check that `Cores/ChaoticSPARTAN1.Jellyfin/interact.json` is the file from the
   release (`release/SHA256SUMS.txt` lists its checksum, which starts `4b53bc27`).

## Go back to the previous files

- From your own card backup: copy back `Cores/ChaoticSPARTAN1.Jellyfin/`,
  `Assets/pocketlive/` and `Platforms/pocketlive.json`.
- If you used `Install-PocketFin.ps1`, the files it replaced are in `backups\<date-time>\` next to the
  script, in the same folder layout as the card. Copy them back onto the card.

## Other problems

| What you see | What to do |
|---|---|
| **Jellyfin** is not in the core list | `Cores/ChaoticSPARTAN1.Jellyfin/` or `Platforms/pocketlive.json` is missing; copy `release/SD/` again |
| The start screen says the JPEG decoder failed | The bitstream is damaged or from another version: copy `release/SD/` again |
| "Library on card: none" | Copy `release/SD-sample-library/` or your own library (see [Offline library](Offline-Library.md)) |
| Your videos do not appear | Copy the whole library folder again, including `Assets/pocketlive/ChaoticSPARTAN1.Jellyfin/Jellyfin.json` |
| Resume points or the saved Chromatic library are lost | Quit the core from the Pocket's menu instead of switching off; check `Saves/pocketlive/common/` has the `pjcache*.sav` and `pjlive_log.sav` files (from `release/SD-first-install-saves/`) |
| The Chromatic is not found | Cable firmly in both ports; on the Chromatic choose **Pocket link**; press **X** on the Pocket's start screen |
| "Link fault" or titles do not start over the cable | Unplug and replug the cable, choose Pocket link again. Try another link cable |
| You cannot see what the Chromatic is doing while you hold the Pocket | Use [Remote Pilot](https://github.com/ChaoticSPARTAN1/RemotePilot) to watch the Chromatic's screen and log on your PC ([more](Wireless-Jellyfin.md#optional-remote-pilot)) |
| Playback over the cable pauses | Normal for the link's speed; copy the title to the card for smooth playback |
| Sound but no picture, or picture but no sound | Note what the start screen says and report it (below) |

## Remove the core cleanly

See [Install > Remove PocketFin](Install.md#remove-pocketfin). Removing PocketFin leaves the rest of
the card untouched.

## Reporting a bug

The app keeps a short log in `Saves/pocketlive/common/pjlive_log.sav` (written when you quit the core).
It is meant to hold timings and error messages only. Open an issue as described in
[CONTRIBUTING.md](../CONTRIBUTING.md) and attach the log if you can. Before posting anything, check it
contains no Wi-Fi names, server addresses or tokens ([SECURITY.md](../SECURITY.md)).

Next: [FAQ](FAQ.md)
