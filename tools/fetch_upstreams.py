# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Fetch the upstream projects PocketFin builds against, at the exact revisions in upstream.lock.json.

Each one is cloned into upstream/<name> (Git-ignored) and checked out at its pinned commit. Nothing is
uploaded and nothing outside upstream/ is changed. Needs Git and an internet connection.

    py -3 tools/fetch_upstreams.py                 # the app and core: openfpgaSDK, openfpgaCore, core_jpeg
    py -3 tools/fetch_upstreams.py --cpu           # also VexiiRiscv + SpinalHDL + rvls (only to regenerate the CPU)
    py -3 tools/fetch_upstreams.py --font          # also Roboto Medium (only to regenerate the font atlas)
    py -3 tools/fetch_upstreams.py --from NAME=DIR # clone NAME from a local Git repository instead (offline)
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import subprocess
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / 'upstream'


def git(*args, cwd=None):
    subprocess.run(['git', *args], cwd=cwd, check=True)


def fetch(name, url, commit, folder, local=None):
    folder.parent.mkdir(parents=True, exist_ok=True)
    if not (folder / '.git').exists():
        source = str(local) if local else url
        git('clone', '--no-checkout', *([] if local else ['--filter=blob:none']), source, str(folder))
    have = subprocess.run(['git', '-C', str(folder), 'cat-file', '-e', commit + '^{commit}'],
                          capture_output=True).returncode == 0
    if not have:
        git('-C', str(folder), 'fetch', 'origin', commit)
    git('-C', str(folder), '-c', 'advice.detachedHead=false', 'checkout', '--quiet', commit)
    actual = subprocess.check_output(['git', '-C', str(folder), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != commit:
        raise SystemExit(f'{name}: checked out {actual}, expected {commit}')
    print(f'{name}: {commit[:12]} in {folder.relative_to(ROOT)}')


def download(name, entry):
    """One file from a release archive, checked against both pinned SHA-256 values."""
    target = ROOT / entry['to']
    if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == entry['ttf_sha256']:
        print(f'{name}: already in {target.relative_to(ROOT)}')
        return
    with urllib.request.urlopen(entry['url'], timeout=120) as response:
        archive = response.read()
    if hashlib.sha256(archive).hexdigest() != entry['zip_sha256']:
        raise SystemExit(f'{name}: {entry["url"]} does not match the pinned SHA-256')
    member = zipfile.ZipFile(io.BytesIO(archive)).read(entry['member'])
    if hashlib.sha256(member).hexdigest() != entry['ttf_sha256']:
        raise SystemExit(f'{name}: {entry["member"]} does not match the pinned SHA-256')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(member)
    print(f'{name}: {entry["tag"]} {entry["member"]} in {target.relative_to(ROOT)}')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cpu', action='store_true', help='also fetch VexiiRiscv and its two submodules')
    parser.add_argument('--font', action='store_true', help='also download Roboto Medium (for tools/build_roboto_atlas.py)')
    parser.add_argument('--from', dest='local', action='append', default=[], metavar='NAME=DIR',
                        help='clone NAME from a local repository (repeatable)')
    args = parser.parse_args()
    local = dict(item.split('=', 1) for item in args.local)
    full = json.loads((ROOT / 'upstream.lock.json').read_text(encoding='utf-8'))
    lock = full['fetch']
    names = ['openfpgaSDK', 'openfpgaCore', 'core_jpeg'] + (['VexiiRiscv'] if args.cpu else [])
    for name in names:
        entry = lock[name]
        fetch(name, entry['repository'], entry['commit'], UPSTREAM / name, local.get(name))
        for sub, subentry in entry.get('submodules', {}).items():
            key = f'{name}/{sub}'
            fetch(key, subentry['repository'], subentry['commit'], UPSTREAM / name / sub, local.get(key))
    if args.font:
        download('roboto', full['download']['roboto'])
    print('Done. Nothing was uploaded.')


if __name__ == '__main__':
    main()
