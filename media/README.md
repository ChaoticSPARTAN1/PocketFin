# Media

Trailers, short clips and pictures for PocketFin and Jellymatic. They are also handy sample videos for
trying the [offline library](../docs/Offline-Library.md) on your Pocket.

| Folder | What is in it |
|---|---|
| `pocketfin/` | PocketFin trailer (1080x1080 and 1920x1080), three short clips, teaser pictures |
| `jellymatic/` | Jellymatic trailer (1080x1080 and 1920x1080), four short clips, teaser pictures |
| `howto/` | How to stream Jellyfin over Wi-Fi from a Chromatic to the Pocket with a link cable (42 s, both sizes), and a picture |

All videos are MP4 (H.264, 30 fps, AAC sound).

## Try them on the Pocket

From the PocketFin folder, for example:

```powershell
py -3 tools\make_offline_library.py --video media\pocketfin\pocketfin-trailer-1920x1080.mp4 --video media\howto\howto-wifi-pocket-1920x1080.mp4
```

Then copy `build\offline-library\` to the SD card as described in
[Offline library](../docs/Offline-Library.md). The 1920x1080 versions fit the Pocket's 16:9 picture best.

## Credits

- The consoles are 3D models made for these videos; they carry no logos. The screens show PocketFin's
  and Jellymatic's own interfaces.
- Footage on the screens: *Sintel* (c) copyright Blender Foundation | durian.blender.org, licensed
  [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) (picture only, scaled and cropped).
- Jellyfin logo: CC BY-SA 4.0, Jellyfin contributors.
- Music and sounds were generated for these videos.

Unofficial fan project, not affiliated with Analogue, ModRetro or Jellyfin. Made by ChaoticSPARTAN1.
