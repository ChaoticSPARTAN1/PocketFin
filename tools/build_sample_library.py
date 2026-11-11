# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Build the core's built-in sample library: one generated test clip, nothing personal.

The released core ships this instead of anyone's Jellyfin library. The clip is our
own FFmpeg output (testsrc2: moving colour pattern with a running time counter, plus a
440 Hz tone), so it carries no copyrighted video and no server data. No artwork is
stored, so the app shows Jellyfin-style placeholder cards and its own logo.

Output: build/sample-library/ (library.pjl, pack00.pjs, sync.json), in exactly the
format and card location the app reads for any library (see LIBRARY_FORMAT below). A
user's own library, written by tools/make_offline_library.py or by the Chromatic tether, replaces
these files in the same place.
"""
import json
import secrets
import shutil
import threading

from connection_config import ROOT
from link_gateway import DETAILS, HOME
from live_av import encoded_stream
from pocket_library import ART_POSTER, LibraryBuilder, PackWriter, finish_home
from stream_transport_probe import film_profile

SAMPLE = ROOT / 'build/sample-library'
SAMPLE_HANDLE = 0x7fff0000  # same handle the PC sync gives its test pattern
SECONDS = 20
LIBRARY_FORMAT = (
    'Card location: Assets/pocketlive/common/library.pjl (data slot 4) and packNN.pjs '
    '(slots 20-31). Format: PJLB version 1, written by tools/pocket_library.py and read by '
    'pocket/sd_library.h.')


def sample_item():
    return {'handle': SAMPLE_HANDLE, 'title': 'Test Pattern', 'subtitle': '', 'rating': '',
            'type': 'Video', 'folder': False, 'duration_ticks': SECONDS * 10000000, 'resume_ticks': 0,
            'played': False, 'year': None}


def build():
    if SAMPLE.exists():
        shutil.rmtree(SAMPLE)
    SAMPLE.mkdir(parents=True)
    builder, pack = LibraryBuilder(), PackWriter(SAMPLE)
    session = secrets.randbelow(0x7ffffffe) + 1
    # client=None and no local path: the encoder's own synthetic source (testsrc2 + sine).
    packets = encoded_stream(None, 'pattern', film_profile(24), session, 0, SECONDS,
                             threading.Event(), lambda process: None)
    slot, offset, length, config, seek = pack.write_title(packets, SECONDS * 260000)
    builder.add_title(SAMPLE_HANDLE, session, slot, offset, length, config, seek)
    item = sample_item()
    builder.add_json(DETAILS, {'overview': f'{SECONDS} seconds of a moving colour pattern with a running '
                               'counter and a steady 440 Hz tone, generated for this core so picture and '
                               'sound can be checked out of the box. Press L during playback for frame '
                               'counters. Your own library replaces this sample.',
                               'genres': ['Sample']}, handle=SAMPLE_HANDLE)
    builder.add_json(HOME, {'rows': [{'title': 'Latest Videos', 'shape': ART_POSTER, 'items': [item]}],
                            'truncated': False})
    finish_home(builder, [item])
    (SAMPLE / 'library.pjl').write_bytes(builder.build())
    summary = {'titles': len(builder.titles), 'records': len(builder.records), 'packs': pack.names,
               'sample': True, 'seconds': SECONDS, 'format': LIBRARY_FORMAT,
               'library_bytes': (SAMPLE / 'library.pjl').stat().st_size,
               'pack_bytes': sum((SAMPLE / name).stat().st_size for name in pack.names)}
    (SAMPLE / 'sync.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary))


if __name__ == '__main__':
    build()
