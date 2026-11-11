# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Assemble the ready-to-copy SD card release in release/ from the build outputs.

    py -3 tools/package_release.py

Inputs (each can be replaced with an option):
  bitstream   build/pocket-media-core/src/fpga/targets/pocket/output_files/ap_core.rbf if it exists,
              otherwise the bitstream already in release/ (so the release can be repackaged without Quartus)
  kernel      core/generated/os.bin (the OS kernel paired with the bitstream)
  app         build/pocket-live-player/pjlive.elf (tools/Build-LivePlayer.ps1)
  sample      build/sample-library (tools/build_sample_library.py: one generated test clip)
  brand       build/brand (tools/build_brand_assets.py: the Jellyfin logo in the Pocket's formats)
  templates   upstream/openfpgaSDK (tools/fetch_upstreams.py)

Output, three folders that mirror the card's root (see docs/Install.md):
  release/SD/                 the core and app; copy for a first install and for every update
  release/SD-sample-library/  the demo library; copy only on a first install (or to go back to the demo)
  release/SD-first-install-saves/  empty save files the Pocket needs to keep the app's log and offline
                              cache; copy only if your card has none (never over existing saves)
plus release/SHA256SUMS.txt. Nothing is written to a card.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct

from pocket_loader import build_loader, check_loader
from validate_pocket_package import check_core
from toolpaths import ROOT

# The author is the project's public handle; the Pocket names the core folder author.shortname.
# Saves (Saves/pocketlive/) and media (Assets/pocketlive/common/) are keyed by the platform id.
AUTHOR, SHORTNAME, PLATFORM = 'ChaoticSPARTAN1', 'Jellyfin', 'pocketlive'
CORE = f'{AUTHOR}.{SHORTNAME}'
CREDIT = f'Created by {AUTHOR}'
DESCRIPTION = f'Jellyfin library and film playback. {CREDIT}'  # APF limit: 63 bytes
VERSION, RELEASE_DATE = '1.0.0', '2026-11-10'
URL = 'https://github.com/ChaoticSPARTAN1/PocketFin'
PACK_SLOTS = range(20, 32)
# The app opens the log as "slot:10" (PJ_DIAG_SLOT in pocket/diag_log.h).
LOG_SLOT, LOG_FILE = 10, 'pjlive_log.sav'
# The Chromatic tether's saved library and the watch state (pocket/tether_store.h): slots 11-19.
CACHE_SLOTS = range(11, 20)
CACHE_FILES = [f'pjcache{index}.sav' for index in range(len(CACHE_SLOTS))]
SAVE_BASE, SAVE_STRIDE, SAVE_SIZE = 0x20100000, 0x40000, 0x40000  # SDK template: slot 10 at 0x20100000
SEED_SIZE = 32768
LOG_SEED = b'PJLIVE-LOG 1\nboot 0 dropped 0 save 0\n'
RPC_ART = 5  # PJ_RPC_ART in shared/bridge_rpc.h
RELEASE = ROOT / 'release'
TEMPLATE = 'dist/sdk/Cores/ThinkElastic.openfpgaOS'
# The only interact.json proven to start on a real Pocket: the SDK template, with CRLF line ends
# (as Git for Windows checks it out). Generated "Core Settings" files hung the Pocket at core start.
TEMPLATE_INTERACT_SHA256 = '4b53bc27ef162c68b3878ab8a203816852c19289bf09758389f6ed98e06a2093'
RBF_IN_RELEASE = RELEASE / 'SD/Cores' / CORE / 'os25.rbf_r'


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as file:
        while chunk := file.read(1 << 22):
            digest.update(chunk)
    return digest.hexdigest()


def reverse_bits(data):
    """The Pocket loads bitstreams with every byte bit-mirrored (the SDK's tools/reverse_bits.c)."""
    table = bytes(int(f'{value:08b}'[::-1], 2) for value in range(256))
    return data.translate(table)


def crlf(data):
    return data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')


def write_json(path, value):
    # CRLF, byte for byte like the files this core has started with on a Pocket.
    path.write_bytes(crlf(json.dumps(value, indent=2).encode('ascii')))


ALWAYS_PRESENT = (0, 1, 2, 3, 4)


def slot_tables(packs):
    """data.json slot definitions and the instance's slot files, in the order the Pocket reads them.

    core.json names a Chip32 loader, so the Pocket loads a slot (and writes its datatable entry) only
    when the loader LOADFs it; tools/pocket_loader.py LOADFs the save slots 10-19. At quit the Pocket
    looks each save slot up by its DECLARATION index, so the save slots come right after the runtime
    slots 0-4 and before the optional packs (check_slot_order); with packs first, no .sav was written.
    """
    slots = [{'id': 1, 'filename': 'os.bin'}, {'id': 2, 'filename': 'pjlive.ini'},
             {'id': 3, 'filename': 'pjlive.elf'}, {'id': 4, 'filename': 'library.pjl'}]
    slots.append({'id': LOG_SLOT, 'filename': LOG_FILE})
    slots += [{'id': slot, 'filename': name} for slot, name in zip(CACHE_SLOTS, CACHE_FILES)]
    slots += [{'id': slot, 'filename': name} for slot, name in zip(PACK_SLOTS, packs)]
    definitions = [{'id': 0, 'name': 'Media library', 'required': True, 'parameters': 275, 'extensions': ['json']}]
    for slot, extension in ((1, 'bin'), (2, 'ini'), (3, 'elf'), (4, 'pjl')):
        definitions.append({'id': slot, 'name': f'Runtime {slot}', 'required': False, 'parameters': 0,
                            'extensions': [extension], 'deferload': slot != 2})
    definitions.append({'id': LOG_SLOT, 'name': 'Diagnostics log', 'required': False, 'parameters': '0x84',
                        'nonvolatile': True, 'address': '0x20100000', 'size_maximum': '0x40000',
                        'extensions': ['sav']})
    definitions += [{'id': slot, 'name': f'Offline cache {index}', 'required': False, 'parameters': '0x84',
                     'nonvolatile': True, 'address': f'0x{SAVE_BASE + (slot - LOG_SLOT) * SAVE_STRIDE:08X}',
                     'size_maximum': f'0x{SAVE_SIZE:X}', 'extensions': ['sav']}
                    for index, slot in enumerate(CACHE_SLOTS)]
    for index, slot in enumerate(PACK_SLOTS):
        definitions.append({'id': slot, 'name': f'Pack {index + 1}', 'required': False, 'parameters': 0,
                            'extensions': ['pjs'], 'deferload': True})
    check_slot_order(definitions)
    return definitions, slots


def check_slot_order(definitions):
    """No optional slot may be declared before a nonvolatile one (see slot_tables)."""
    for index, definition in enumerate(definitions):
        if definition.get('nonvolatile'):
            earlier = [d['id'] for d in definitions[:index] if not d.get('nonvolatile') and d['id'] not in ALWAYS_PRESENT]
            if earlier:
                raise ValueError(f'Optional slots {earlier} are declared before save slot {definition["id"]}')


def save_entries(definitions, slots):
    """Datatable entries the Pocket gives each save slot: one per slot with a file, data.json order."""
    present = {0} | {slot['id'] for slot in slots}
    ids = [d['id'] for d in definitions if d['id'] in present]
    return {slot: ids.index(slot) for slot in (LOG_SLOT, *CACHE_SLOTS)}


def quit_lookup_entries(definitions):
    """Where the Pocket looks each save slot up at quit: its declaration index."""
    return {d['id']: index for index, d in enumerate(definitions) if d.get('nonvolatile')}


def instance_json(packs):
    """Assets/pocketlive/ChaoticSPARTAN1.Jellyfin/Jellyfin.json: the files the Pocket loads (it loads only listed packs)."""
    _, slots = slot_tables(packs)
    return {'instance': {'magic': 'APF_VER_1', 'variant_select': {'id': 666, 'select': False}, 'data_slots': slots}}


def released_rbf():
    expected = json.loads((ROOT / 'core/expected-sources.json').read_text(encoding='utf-8'))
    return expected['released_ap_core_rbf_sha256']


def check_bitstream(rbf, built_tree):
    """A bitstream is accepted if it is the released one, or if it was built from a tree that passes the
    source check (so it has the exit-save fix in core_bridge_cmd.v)."""
    if hashlib.sha256(rbf).hexdigest() == released_rbf():
        return 'released'
    if built_tree is None:
        raise ValueError('Unknown bitstream and no build tree to check it against')
    from prepare_pocket_media_core import check
    check(built_tree)
    return 'built from checked sources'


def package(rbf_path=None, os_bin=None, elf=None, sample=None, brand=None, sdk=None):
    sdk = Path(sdk or ROOT / 'upstream/openfpgaSDK')
    built_tree = ROOT / 'build/pocket-media-core'
    default_rbf = built_tree / 'src/fpga/targets/pocket/output_files/ap_core.rbf'
    if rbf_path:
        rbf = Path(rbf_path).read_bytes()
    elif default_rbf.is_file():
        rbf = default_rbf.read_bytes()
    elif RBF_IN_RELEASE.is_file():
        rbf = reverse_bits(RBF_IN_RELEASE.read_bytes())
    else:
        raise SystemExit('No bitstream: build one (tools/Build-PocketMediaCore.ps1 -Fit) or pass --rbf')
    origin = check_bitstream(rbf, built_tree if built_tree.is_dir() else None)
    os_bin = Path(os_bin or ROOT / 'core/generated/os.bin')
    elf = Path(elf or ROOT / 'build/pocket-live-player/pjlive.elf')
    sample = Path(sample or ROOT / 'build/sample-library')
    brand = Path(brand or ROOT / 'build/brand')
    for need, hint in ((elf, 'tools/Build-LivePlayer.ps1'), (sample / 'library.pjl', 'tools/build_sample_library.py'),
                       (brand / 'icon.bin', 'tools/build_brand_assets.py'), (sdk / TEMPLATE / 'interact.json',
                       'tools/fetch_upstreams.py')):
        if not need.is_file():
            raise SystemExit(f'Missing {need.relative_to(ROOT) if need.is_relative_to(ROOT) else need}: run {hint}')
    for name in ('SD', 'SD-sample-library', 'SD-first-install-saves'):
        if (RELEASE / name).exists():
            shutil.rmtree(RELEASE / name)
    bundle = RELEASE / 'SD'
    core = bundle / 'Cores' / CORE
    common = bundle / 'Assets' / PLATFORM / 'common'
    # The app's About screen points at Notices/PocketLive on the card, so the notices stay there.
    notices = bundle / 'Notices' / 'PocketLive'
    for folder in (core, common, bundle / 'Platforms/_images', notices):
        folder.mkdir(parents=True, exist_ok=True)
    template = sdk / TEMPLATE
    for name in ('audio.json', 'input.json', 'video.json', 'variants.json', 'interact.json'):
        (core / name).write_bytes(crlf((template / name).read_bytes()))
    if sha256(core / 'interact.json') != TEMPLATE_INTERACT_SHA256:
        raise ValueError('interact.json is not the SDK template (sha256 4b53bc27...); only that file is known to start')
    metadata = json.loads((template / 'core.json').read_text())
    metadata['core']['metadata'].update(platform_ids=[PLATFORM], shortname=SHORTNAME, description=DESCRIPTION,
                                        author=AUTHOR, url=URL, version=VERSION, date_release=RELEASE_DATE)
    metadata['core']['framework']['chip32_vm'] = 'loader.bin'
    for bitstream in metadata['core']['cores']:
        bitstream.pop('chip32_vm', None)
    metadata['core']['cores'] = [c for c in metadata['core']['cores'] if c.get('id') == 0]
    write_json(core / 'core.json', metadata)
    shutil.copyfile(brand / 'icon.bin', core / 'icon.bin')
    shutil.copyfile(brand / 'platform.bin', bundle / 'Platforms/_images' / f'{PLATFORM}.bin')
    # The loader picks core 0 unless the .ini says os30, and core 0 is this file.
    (core / 'os25.rbf_r').write_bytes(reverse_bits(rbf))
    # Under a Chip32 loader the Pocket loads no slot by itself: the SDK loader plus LOADF for every save slot.
    (core / 'loader.bin').write_bytes(build_loader((sdk / 'runtime/pocket/loader.bin').read_bytes(),
                                                   [LOG_SLOT, *CACHE_SLOTS]))
    shutil.copyfile(os_bin, common / 'os.bin')
    shutil.copyfile(elf, common / 'pjlive.elf')
    (common / 'pjlive.ini').write_bytes(b'[os]\r\nELF=pjlive.elf\r\nARGS=\r\nVARIANT=os25\r\n')
    definitions, _ = slot_tables([])
    write_json(core / 'data.json', {'data': {'magic': 'APF_VER_1', 'data_slots': definitions}})
    platform = json.loads((sdk / 'dist/sdk/Platforms/openfpgaos.json').read_text())
    platform['platform'].update(name='Jellyfin', manufacturer=AUTHOR, year=2026)
    write_json(bundle / 'Platforms' / f'{PLATFORM}.json', platform)
    write_notices(notices, sdk)

    # The demo library: library.pjl + packs and the instance file that lists them.
    library_root = RELEASE / 'SD-sample-library'
    library_common = library_root / 'Assets' / PLATFORM / 'common'
    library_common.mkdir(parents=True)
    summary = json.loads((sample / 'sync.json').read_text(encoding='utf-8'))
    if summary.get('sample') is not True:
        raise ValueError('The release may carry only the generated sample library')
    shutil.copyfile(sample / 'library.pjl', library_common / 'library.pjl')
    for name in summary['packs']:
        shutil.copyfile(sample / name, library_common / name)
    (library_root / 'Assets' / PLATFORM / CORE).mkdir(parents=True)
    write_json(library_root / 'Assets' / PLATFORM / CORE / 'Jellyfin.json', instance_json(summary['packs']))
    check_sample(library_common)

    saves = RELEASE / 'SD-first-install-saves/Saves' / PLATFORM / 'common'
    saves.mkdir(parents=True)
    (saves / LOG_FILE).write_bytes(LOG_SEED + bytes(SEED_SIZE - len(LOG_SEED)))
    for name in CACHE_FILES:
        (saves / name).write_bytes(bytes(SEED_SIZE))

    validate(bundle, library_root, rbf)
    lines = [f'{sha256(p)}  {p.relative_to(RELEASE).as_posix()}' for p in sorted(RELEASE.rglob('*'))
             if p.is_file() and p.name not in ('SHA256SUMS.txt', 'README.md')]
    (RELEASE / 'SHA256SUMS.txt').write_text('\n'.join(lines) + '\n', encoding='ascii', newline='\n')
    print(f'Release written to release/ ({len(lines)} files; bitstream: {origin}). Nothing was written to a card.')


# Files copied to Notices/PocketLive on the card, under these names (repository path -> card name).
CARD_NOTICES = {
    'LICENSE': 'PocketFin-LICENSE-GPL-3.0.txt',
    'ADDITIONAL-TERMS.md': 'PocketFin-ADDITIONAL-TERMS.md',
    'THIRD-PARTY-NOTICES.md': 'THIRD-PARTY-NOTICES.md',
    'third_party/jellyfin-ux-branding/LICENSE': 'Jellyfin-logo-CC-BY-SA-4.0.txt',
    'pocket/fonts/NOTICE.txt': 'Roboto-font-NOTICE.txt',
}
CARD_LICENCES = ('Apache-2.0.txt', 'ISC-lib9660.txt', 'MIT-0-dlmalloc.txt', 'MIT-musl-COPYRIGHT.txt',
                 'MIT-cram0_phy.txt', 'MIT-VexiiRiscv.txt', 'LicenseRef-Analogue-Pocket-Framework.txt',
                 'LicenseRef-Intel-FPGA-IP.txt', 'openfpgaSDK-NOTICE.txt', 'openfpgaCore-NOTICE.txt')


def card_links(text):
    """Markdown links as they work on the card: notices by their card names, anything else on GitHub."""
    def target(match):
        link = match.group(1)
        if link.startswith(('http://', 'https://', '#')):
            return match.group(0)
        if link in CARD_NOTICES:
            return f']({CARD_NOTICES[link]})'
        if link.startswith('LICENSES/') and link[len('LICENSES/'):] in CARD_LICENCES:
            return f']({link[len("LICENSES/"):]})'
        kind = 'tree' if (ROOT / link).is_dir() else 'blob'
        return f']({URL}/{kind}/main/{link})'
    return re.sub(r'\]\(([^)\s]+)\)', target, text)


def write_notices(notices, sdk):
    licences = ROOT / 'LICENSES'
    for source, name in CARD_NOTICES.items():
        data = (ROOT / source).read_bytes()
        if source.endswith('.md'):
            data = card_links(data.decode('utf-8')).encode('utf-8')
        (notices / name).write_bytes(data)
    if crlf((sdk / 'NOTICE').read_bytes()) != crlf((licences / 'openfpgaSDK-NOTICE.txt').read_bytes()):
        raise ValueError('LICENSES/openfpgaSDK-NOTICE.txt is not the pinned SDK NOTICE')
    for name in CARD_LICENCES:
        shutil.copyfile(licences / name, notices / name)
    (notices / 'SOURCE.txt').write_bytes(crlf(
        f'PocketFin {VERSION}. {CREDIT}.\n'
        f'Source code for everything in this core, and how to rebuild it: {URL}\n'
        'Licence: GPL-3.0-or-later (PocketFin-LICENSE-GPL-3.0.txt) with the additional terms and permission in\n'
        'PocketFin-ADDITIONAL-TERMS.md. Third-party parts: THIRD-PARTY-NOTICES.md and the other files here.\n'
        'Exact upstream revisions: upstream.lock.json in that repository.\n'
        'Not affiliated with, endorsed by or supported by Analogue or the Jellyfin project.\n'.encode('ascii')))


def check_sample(common):
    """The release library is exactly the generated sample: one title, no artwork."""
    data = (common / 'library.pjl').read_bytes()
    magic, _version, records, record_at, titles, _title_at, _size, _crc = struct.unpack_from('<4s7I', data)
    commands = [data[record_at + 24 * i] for i in range(records)]
    if magic != b'PJLB' or titles != 1 or RPC_ART in commands:
        raise ValueError('Release library must be one title without artwork')


def validate(bundle, library_root, rbf):
    directory = bundle / 'Cores' / CORE
    core = json.loads((directory / 'core.json').read_text())['core']
    check_core(core, directory)
    if core['metadata']['author'] != AUTHOR or CREDIT not in core['metadata']['description']:
        raise ValueError('Core metadata must name the author and carry the credit')
    if len(core['cores']) != 1 or core['cores'][0]['filename'] != 'os25.rbf_r':
        raise ValueError('Loader expects a single core 0 named os25.rbf_r')
    if reverse_bits((directory / 'os25.rbf_r').read_bytes()) != rbf:
        raise ValueError('Bitstream does not reverse back to the assembled image')
    data = json.loads((directory / 'data.json').read_text())['data']['data_slots']
    if not 1 <= len(data) <= 32 or any(len(slot['name']) > 15 for slot in data):
        raise ValueError('Invalid data slot layout')
    check_slot_order(data)
    saves = {slot['id'] for slot in data if slot.get('nonvolatile')}
    if saves != {LOG_SLOT, *CACHE_SLOTS}:
        raise ValueError('Only the diagnostics log and the offline cache may be save slots')
    check_loader((directory / core['framework']['chip32_vm']).read_bytes(), saves)
    ids = {slot['id'] for slot in data}
    instance = json.loads((library_root / 'Assets' / PLATFORM / CORE / 'Jellyfin.json').read_text())['instance']
    named = {reference['id']: reference['filename'] for reference in instance['data_slots']}
    if any(named.get(slot) != name for slot, name in zip(CACHE_SLOTS, CACHE_FILES)) or named.get(LOG_SLOT) != LOG_FILE:
        raise ValueError('Every save slot needs its file name in the instance, or the Pocket keeps nothing')
    for reference in instance['data_slots']:
        if reference['id'] not in ids or len(reference['filename']) > 31:
            raise ValueError('Instance references an invalid slot or file')
    if 'os30' in (bundle / 'Assets' / PLATFORM / 'common/pjlive.ini').read_text():
        raise ValueError('The loader would select a core this package does not have')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--rbf', help='ap_core.rbf to package (default: the build tree, else the released one)')
    parser.add_argument('--os-bin', help='paired kernel (default core/generated/os.bin)')
    parser.add_argument('--elf', help='app (default build/pocket-live-player/pjlive.elf)')
    parser.add_argument('--sample', help='sample library folder (default build/sample-library)')
    parser.add_argument('--sdk', help='openfpgaSDK checkout (default upstream/openfpgaSDK)')
    a = parser.parse_args()
    package(a.rbf, a.os_bin, a.elf, a.sample, sdk=a.sdk)
