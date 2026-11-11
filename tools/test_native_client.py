# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Compile real portable native model; exercise replies from actual Gateway."""
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import hashlib
from datetime import datetime, timezone
from unittest.mock import patch
import toolpaths
from toolpaths import ROOT, GCC
from link_gateway import Gateway, HOME, ART, encode_packet
from test_link_gateway import FakeClient
from test_stream_worker import raw_packets


def main():
    env = toolpaths.msys_env()
    out = ROOT / 'build/native-client'
    out.mkdir(parents=True, exist_ok=True)
    exe = out / 'check_native_client.exe'
    subprocess.run([str(GCC), '-std=c11', '-O2', '-Wall',
        '-Wextra', '-Werror',
        str(ROOT / 'pocket/check_native_client.c'), '-o', str(exe)], check=True, env=env)
    controller = out / 'check_live_controller.exe'
    subprocess.run([str(GCC), '-std=c11', '-O2', '-Wall',
        '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
        '-I' + str(ROOT / 'upstream/openfpgaSDK/src/sdk/include'),
        str(ROOT / 'pocket/check_live_controller.c'), '-o', str(controller)], check=True, env=env)
    client = FakeClient()
    client.home = lambda: [
        {'title': 'Continue Watching', 'items': [client.video,
            dict(client.video, Id='folder', Name='Library', IsFolder=True)]},
        {'title': 'Latest', 'items': [dict(client.video, Id='unicode', Name='Café 🚀',
            UserData={'Played': True, 'PlaybackPositionTicks': 0})]}]
    gateway = Gateway(client)
    try:
        data = io.BytesIO()
        requests = [encode_packet(HOME, 1)]
        requests += [encode_packet(ART, i+2, json.dumps({'handle': 1, 'offset': off}).encode())
                     for i, off in enumerate((0, 4096, 8192))]
        art = bytes(i % 256 for i in range(64 * 96 * 2))
        with patch('link_gateway.fetch_card', return_value=art):
            gateway.serve(io.BytesIO(b''.join(requests)), data)
        frames = data.getvalue()
        home_size = 16 + int.from_bytes(frames[6:8], 'little')
        # Only synthetic metadata/media in these temporary fixture files.
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            home, artwork, stream = (folder / n for n in ('home.dat', 'art.dat', 'stream.dat'))
            home.write_bytes(frames[:home_size]); artwork.write_bytes(frames[home_size:])
            stream.write_bytes(b''.join(raw_packets(3, 3)))
            subprocess.run([str(exe), str(home), str(artwork), str(stream)], check=True, env=env)
            subprocess.run([str(controller), str(home), str(out)], check=True, env=env)
            for ppm in out.glob('ui-*.ppm'):
                subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', str(ppm), '-frames:v', '1',
                    '-vf', 'scale=960:720:flags=neighbor', str(ppm.with_suffix('.png'))], check=True, env=env,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        sources = [ROOT / p for p in ('pocket/live_player.c', 'pocket/live_ui.h',
            'pocket/live_library.h', 'pocket/rpc_client.h', 'pocket/stream_reader.h',
            'shared/bridge_rpc.h', 'shared/bounded_json.h', 'pocket/check_native_client.c',
            'pocket/check_live_controller.c', 'pocket/tether_cache.h', 'pocket/tether_sync.h',
            'pocket/tether_store.h', 'pocket/watch_state.h', 'pocket/link_hw.h',
            'pocket/live_playback.h', 'shared/pocket_tether.h')]
        (out / 'test-receipt.json').write_text(json.dumps({
            'at_utc': datetime.now(timezone.utc).isoformat(), 'passed': True,
            'source': 'synthetic fixtures through actual Gateway',
            'physical_link': False, 'sdk_audio_video_io': 'modeled',
            'sanitizer': False,
            'sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                       for p in sources}}, indent=2), encoding='utf-8')
    finally:
        gateway.close()


if __name__ == '__main__':
    main()
