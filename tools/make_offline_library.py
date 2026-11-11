# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Make an offline library for the SD card from your own video files (and, optionally, your Jellyfin server).

    py -3 tools/make_offline_library.py --video "My Video.mp4" --video "Another Video.mkv"
    py -3 tools/make_offline_library.py --jellyfin --max-titles 4      # titles from your own server

The result is a folder that mirrors the card (default build/offline-library):
    Assets/pocketlive/common/library.pjl           the library: Home rows, details, pictures
    Assets/pocketlive/common/pack00.pjs ...         the videos (up to 12 packs, 2 GiB each)
    Assets/pocketlive/ChaoticSPARTAN1.Jellyfin/Jellyfin.json   tells the Pocket which packs to load
Copy its contents onto the card's root folder (docs/Offline-Library.md). Videos are converted to the
Pocket's format: 256x144 Motion-JPEG pictures at the video's own frame rate (24, 25 or 30 fps;
faster videos are halved) with IMA ADPCM sound, about 0.9 GB per hour.

Needs FFmpeg (ffmpeg and ffprobe on PATH) and PyAV (pip install av). --jellyfin reads your server
address and sign-in from private/jellyfin-connection.txt (see jellyfin-connection.example.txt); it
reads only, never changes your server, and prints counts only, never titles, addresses or tokens.
Only add videos you own or have the right to copy.
"""
import argparse
import json
from pathlib import Path
import secrets
import shutil
import subprocess
import threading

from connection_config import DEFAULT_CONFIG, load_connection
from jellyfin_artwork import ART_KINDS, ART_POSTER
from link_gateway import ART, DETAILS, HOME, Gateway
from live_av import encoded_stream
from package_release import CORE, PLATFORM, instance_json, write_json
from pocket_library import LibraryBuilder, PackWriter, finish_home, record_library
from pocket_media import tool
from stream_transport_probe import film_profile
from toolpaths import ROOT

PATTERN_HANDLE = 0x7fff0000
LOCAL_BASE = 0x7fff0100
PLAYABLE_TYPES = {'Movie', 'Episode', 'Video', 'MusicVideo'}
ESTIMATE_BYTES_PER_SECOND = 260000
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def card_from(source_args, kind):
    """One frame of a local source as card art; failure just means a placeholder."""
    _, width, height = ART_KINDS[kind]
    command = [tool('ffmpeg'), '-nostdin', '-v', 'error', *source_args, '-frames:v', '1', '-vf',
               f'scale={width}:{height}:force_original_aspect_ratio=increase,crop={width}:{height}',
               '-pix_fmt', 'rgb565le', '-f', 'rawvideo', 'pipe:1']
    result = subprocess.run(command, capture_output=True, timeout=60, creationflags=NO_WINDOW)
    return result.stdout if not result.returncode and len(result.stdout) == width * height * 2 else b''


def local_item(handle, title, overview, seconds, art_args, builder):
    builder.add_json(DETAILS, {'overview': overview, 'genres': ['On this card']}, handle=handle)
    for kind in ART_KINDS:
        data = card_from(art_args, kind)
        if data:
            builder.add(ART, data, kind=kind, handle=handle)
    return {'handle': handle, 'title': title[:160], 'subtitle': '', 'rating': '', 'type': 'Video', 'folder': False,
            'duration_ticks': int(seconds * 10000000), 'resume_ticks': 0, 'played': False, 'year': None}


def add_pattern(pack, builder, seconds=60):
    """A generated test pattern, so the card always has something to play."""
    session = secrets.randbelow(0x7ffffffe) + 1
    packets = encoded_stream(None, 'pattern', film_profile(24), session, 0, seconds, threading.Event(), lambda p: None)
    slot, offset, length, config, seek = pack.write_title(packets, seconds * ESTIMATE_BYTES_PER_SECOND)
    builder.add_title(PATTERN_HANDLE, session, slot, offset, length, config, seek)
    return local_item(PATTERN_HANDLE, 'Test Pattern',
                      f'{seconds} seconds of moving colour bars and a steady tone, decoded by the FPGA. '
                      'Press L during playback to show frame counters.', seconds,
                      ['-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=1'], builder)


def probe_seconds_and_rate(path):
    result = subprocess.run([tool('ffprobe'), '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                             'stream=r_frame_rate:format=duration', '-of', 'json', str(path)],
                            capture_output=True, timeout=60, check=True, creationflags=NO_WINDOW)
    info = json.loads(result.stdout)
    top, _, bottom = info['streams'][0]['r_frame_rate'].partition('/')
    return float(info['format']['duration']), float(top) / float(bottom or 1)


def add_local(path, index, pack, builder):
    seconds, rate = probe_seconds_and_rate(path)
    session = secrets.randbelow(0x7ffffffe) + 1
    handle = LOCAL_BASE + index
    packets = encoded_stream(None, 'local', film_profile(rate), session, 0, None, threading.Event(), lambda p: None,
                             local_path=str(path))
    slot, offset, length, config, seek = pack.write_title(packets, seconds * ESTIMATE_BYTES_PER_SECOND)
    builder.add_title(handle, session, slot, offset, length, config, seek)
    return local_item(handle, Path(path).stem, 'A video file added from a PC.', seconds,
                      ['-ss', str(seconds / 10), '-i', str(path)], builder)


def frame_rate(item):
    for stream in item.get('MediaStreams') or []:
        if stream.get('Type') == 'Video':
            return stream.get('RealFrameRate') or stream.get('AverageFrameRate')
    return None


def choose_titles(gateway, maximum, budget):
    """Continue Watching first, then newest, one copy of each, within the size budget."""
    home = gateway.dispatch(HOME, {})
    chosen, total, seen = [], 0, set()
    for row in home['rows']:
        for public in row['items']:
            if len(chosen) >= maximum:
                return chosen
            item = gateway.handles[public['handle']]
            if public['handle'] in seen or item.get('IsFolder') or item.get('Type') not in PLAYABLE_TYPES:
                continue
            seen.add(public['handle'])
            seconds = public['duration_ticks'] / 10000000
            cost = seconds * ESTIMATE_BYTES_PER_SECOND
            if not seconds or total + cost > budget:
                continue
            chosen.append(public)
            total += cost
    return chosen


def build(args):
    out = Path(args.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    common = out / 'Assets' / PLATFORM / 'common'
    common.mkdir(parents=True)
    builder, pack = LibraryBuilder(), PackWriter(common)
    on_card = [add_pattern(pack, builder)] if not args.no_test_pattern else []
    failures, synced, shown, gateway = 0, 0, 0, None
    for index, path in enumerate(args.video):
        try:
            on_card.append(add_local(Path(path), index, pack, builder))
        except Exception:
            failures += 1
            print(f'Could not convert video {index + 1}; skipped.', flush=True)
    local_count = len(on_card)
    try:
        if args.jellyfin:
            from jellyfin_client import JellyfinClient
            client = JellyfinClient(load_connection(DEFAULT_CONFIG))
            client.connect()
            gateway = Gateway(client)
            for public in choose_titles(gateway, args.max_titles, args.max_gb * 1e9):
                item = client.item(gateway.handles[public['handle']]['Id'])
                session = secrets.randbelow(0x7ffffffe) + 1
                try:
                    packets = encoded_stream(client, item['Id'], film_profile(frame_rate(item)), session, 0, None,
                                             threading.Event(), lambda p: None)
                    estimate = public['duration_ticks'] / 10000000 * ESTIMATE_BYTES_PER_SECOND
                    slot, offset, length, config, seek = pack.write_title(packets, estimate)
                except Exception:
                    failures += 1
                    continue
                builder.add_title(public['handle'], session, slot, offset, length, config, seek)
                on_card.append(public)
                synced += 1
                print(f'Copied {synced} server title(s); {failures} failed.', flush=True)
            shown = record_library(gateway, builder, on_card[local_count:])
        else:
            builder.add_json(HOME, {'rows': [{'title': 'On this Pocket', 'shape': ART_POSTER, 'items': on_card[:12]}],
                                    'truncated': False})
        # Everything on the card opens from one "On this Pocket" tile in My Media.
        finish_home(builder, on_card)
    finally:
        if gateway:
            gateway.close()
    (common / 'library.pjl').write_bytes(builder.build())
    (out / 'Assets' / PLATFORM / CORE).mkdir(parents=True)
    write_json(out / 'Assets' / PLATFORM / CORE / 'Jellyfin.json', instance_json(pack.names))
    size = sum(p.stat().st_size for p in common.iterdir())
    summary = {'titles': len(builder.titles), 'failed': failures, 'server_titles': synced, 'items_shown': shown,
               'packs': pack.names, 'total_bytes': size}
    print(json.dumps(summary))
    print(f'Done. Copy the contents of {out} onto the root of the Pocket\'s SD card '
          '(replace the files it asks about). See docs/Offline-Library.md.')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--video', action='append', default=[], help='a video file on this PC (repeatable)')
    parser.add_argument('--jellyfin', action='store_true',
                        help='also copy titles from your server (private/jellyfin-connection.txt)')
    parser.add_argument('--max-titles', type=int, default=4, help='most server titles to copy (default 4)')
    parser.add_argument('--max-gb', type=float, default=8.0, help='size budget for server titles in GB (default 8)')
    parser.add_argument('--no-test-pattern', action='store_true', help='leave out the 60-second test pattern')
    parser.add_argument('--out', default=str(ROOT / 'build/offline-library'), help='output folder')
    args = parser.parse_args()
    if not args.video and not args.jellyfin and args.no_test_pattern:
        parser.error('nothing to add: give --video and/or --jellyfin')
    try:
        build(args)
    except Exception as error:
        # No addresses, titles or tokens in the message: only the kind of failure.
        print(f'Failed: {type(error).__name__}. No connection details were printed.')
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
