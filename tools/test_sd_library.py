# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""End to end on-card library: real Gateway replies, real encoder, real C reader.

Uses FakeClient metadata and FFmpeg's synthetic source. No library media, no
server, no hardware.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from unittest.mock import patch

import toolpaths
from toolpaths import ROOT, GCC
from live_av import encoded_stream
from pocket_library import LibraryBuilder, PackWriter, load_library, record_library, retile
from stream_transport_probe import PROFILES, film_profile
from test_link_gateway import FakeClient
from link_gateway import Gateway, HOME

SECONDS = 12
SEEK = 7


def art(client, item, kind):
    from jellyfin_artwork import ART_KINDS
    _, width, height = ART_KINDS[kind]
    return bytes([kind + 1]) * (width * height * 2)


def main():
    assert film_profile(23.976).key == 'pocket-card-film-23976'
    assert film_profile(24).key == 'pocket-card-film-24'
    assert film_profile(25).key == 'pocket-card-pal-25'
    assert film_profile(29.97).key == 'pocket-card-video-2997'
    assert film_profile(59.94).key == 'pocket-card-video-2997'
    assert film_profile(None).key == 'pocket-card-film-23976'
    env = toolpaths.msys_env()
    out = ROOT / 'build/native-client'
    out.mkdir(parents=True, exist_ok=True)
    exe = out / 'check_sd_library.exe'
    subprocess.run([str(GCC), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'pocket'), str(ROOT / 'pocket/check_sd_library.c'), '-ljpeg', '-o', str(exe)],
                   check=True, env=env)
    # Device read path against a model of the runtime's interrupt-driven completion.
    race = out / 'check_sd_async.exe'
    subprocess.run([str(GCC), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'pocket/check_sd_async_include'), str(ROOT / 'pocket/check_sd_async.c'),
                    '-o', str(race)], check=True, env=env)
    subprocess.run([str(race)], check=True, env=env, timeout=60)
    profile = next(p for p in PROFILES if p.key == 'pocket-card-film-24')
    client = FakeClient()
    other = dict(client.video, Id='other', Name='Not synced')
    client.home = lambda: [{'title': 'Latest Movies', 'items': [client.video, other], 'shape': 'poster'}]
    gateway = Gateway(client)
    try:
        with tempfile.TemporaryDirectory(dir=out) as temp:
            folder = Path(temp)
            home = gateway.dispatch(HOME, {})
            title = home['rows'][0]['items'][0]
            pack = PackWriter(folder)
            session = 77
            packets = encoded_stream(None, 'synthetic', profile, session, 0, SECONDS, threading.Event(), lambda p: None)
            slot, offset, length, config, seek = pack.write_title(packets, 0)
            builder = LibraryBuilder()
            builder.add_title(title['handle'], session, slot, offset, length, config, seek)
            with patch('link_gateway.fetch_card', art):
                record_library(gateway, builder, [title])
            # Written the old way (an 'On this Pocket' row), then given the My Media tile in
            # place, which also proves the loader round-trips a library exactly.
            legacy = builder.build()
            assert load_library(legacy).build() == legacy
            (folder / 'library.pjl').write_bytes(legacy)
            assert retile(folder / 'library.pjl') == 1
            try:
                retile(folder / 'library.pjl')
                raise AssertionError('a second retile must be refused')
            except ValueError:
                pass
            rate = length / SECONDS
            result = subprocess.run([str(exe), str(folder / 'library.pjl'), str(folder / pack.names[0]),
                                     str(profile.fps * SECONDS), str(profile.fps), str(SEEK)],
                                    capture_output=True, text=True, env=env, timeout=180)
            print(result.stdout.strip(), result.stderr.strip())
            if result.returncode:
                raise SystemExit(1)
            print(json.dumps({'synthetic_bytes_per_second': round(rate), 'seek_entries': len(seek)}))
    finally:
        gateway.close()
    check_sample(exe, env, out)
    print('PASS: on-card library end to end, synthetic source. No hardware accessed.')


def check_sample(exe, env, out):
    """The core's built-in sample (tools/build_sample_library.py) through the same C reader."""
    from build_sample_library import SAMPLE, SECONDS as SAMPLE_SECONDS
    if not (SAMPLE / 'library.pjl').is_file():
        raise SystemExit('Build the sample first: tools/build_sample_library.py')
    result = subprocess.run([str(exe), str(SAMPLE / 'library.pjl'), str(SAMPLE / 'pack00.pjs'),
                             str(24 * SAMPLE_SECONDS), '24', '7', 'sample', str(out)],
                            capture_output=True, text=True, env=env, timeout=180)
    print('sample:', result.stdout.strip(), result.stderr.strip())
    if result.returncode:
        raise SystemExit(1)
    for ppm in out.glob('ui-*-sample.ppm'):
        subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', str(ppm), '-frames:v', '1',
                        '-vf', 'scale=960:720:flags=neighbor', str(ppm.with_suffix('.png'))], check=True, env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
