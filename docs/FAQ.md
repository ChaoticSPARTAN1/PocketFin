# FAQ

[Home](Home.md) > FAQ

- [Basics](#basics)
- [Video and sound](#video-and-sound)
- [Jellyfin and the Chromatic](#jellyfin-and-the-chromatic)
- [Safety and legal](#safety-and-legal)

## Basics

**Why is the core called "Jellyfin" on the Pocket and "PocketFin" here?**
PocketFin is the project's name. The core keeps the name Jellyfin (folder `ChaoticSPARTAN1.Jellyfin`)
so existing libraries and saves on cards keep working.

**Do I need a Chromatic?** No. Videos on the SD card play with nothing else. The Chromatic is only for
live access to your server.

**Does it work in the Analogue Dock?** The core allows the Dock; it has not been tested much there.

**Can I use the Pocket's Core Settings to change things?** Not yet. Those screens show leftover template
labels, which PocketFin ignores. See [Using PocketFin](Using-PocketFin.md#settings-in-the-pockets-menu).

## Video and sound

**Why 256x144?** The FPGA decodes Motion-JPEG pictures in hardware. 256x144 at up to 30 fps keeps the
files small (about 0.9 GB per hour) and the decoding smooth.

**Will a 24 fps film play at 24 fps?** Yes. Each title keeps its own frame rate (23.976, 24, 25, 29.97 or
30 fps). Press **R** while playing for film mode, which matches the screen refresh to the film.

**Subtitles? Several audio languages?** Not yet. One audio track is used.

**Why is streaming through the Chromatic blurry?** The link cable carries a few tens of kilobytes per
second, so the server sends very small pictures. Copy titles to the card for the sharp version.

## Jellyfin and the Chromatic

**Does PocketFin need my Jellyfin password on the Pocket?** No. The Pocket never sees your server
address, password or tokens. The Chromatic signs in to your server (set up in Jellymatic), and the PC
tool reads its own file on your PC.

**Does it change my server?** The PC tool only reads. Over the Chromatic, playback positions follow
what Jellymatic does; see its documentation.

**Is there a Wi-Fi accessory or a direct PC connection?** No. The Pocket has no Wi-Fi, and cores cannot
use its USB port. The two supported ways are the Chromatic link and copying to the card.

**Which Chromatic software do I need?** Jellymatic with its Pocket link option:
https://github.com/ChaoticSPARTAN1/Jellymatic

## Safety and legal

**Can PocketFin damage my Pocket?** It runs only while the core is open and does not touch the Pocket's
firmware. The worst known case is a black screen at core start if core files are edited; switching the
Pocket off fully and restoring the files fixes it ([Troubleshooting](Troubleshooting.md)).

**Is this made by Analogue or Jellyfin?** No. PocketFin is an independent project, not affiliated with
Analogue or the Jellyfin project.

**Can I share converted videos?** Only if you have the right to. Only convert videos you own or may copy.

**Is support guaranteed?** No. Bug reports and pull requests are welcome, but answers are not
guaranteed ([CONTRIBUTING.md](../CONTRIBUTING.md)).

Next: [Building from source](Building-from-Source.md)
