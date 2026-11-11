# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Writer for the on-card library read by pocket/sd_library.h.

The library is a recording of Gateway replies, so stored and tethered playback
share one source of truth: whatever the tether would have said, the card says.
Files contain display metadata, artwork and media only. No server address,
token, password or server-side item ID is ever written.
"""
import json
import struct
from pathlib import Path
import zlib

from jellyfin_artwork import ART_KINDS, ART_POSTER, ART_THUMB
from link_gateway import ART, BROWSE, DETAILS, HOME, json_bytes

MAGIC = b'PJLB'
VERSION = 1
HEADER = struct.Struct('<4s7I')
RECORD = struct.Struct('<BBHIIIII')
TITLE = struct.Struct('<8I')
SEEK_SECONDS = 5
FIRST_PACK_SLOT = 20
MAX_PACKS = 12
MAX_RECORDS = 32768  # must equal PJ_SD_MAX_RECORDS in pocket/sd_library.h
MAX_TITLES = 256
PACK_LIMIT = 0x7fffffff  # Offsets above 2 GiB have never been exercised on the Pocket.
MAX_REPLY = 32768
RECORD_HEADROOM = 16  # one item adds at most four records; keep Home's own record safe


class LibraryBuilder:
    def __init__(self):
        self.blobs = bytearray()
        self.records = []
        self.titles = []
        self.seek_tables = []

    @property
    def full(self):
        return len(self.records) >= MAX_RECORDS - RECORD_HEADROOM

    def add(self, command, data, kind=0, handle=0, start=0):
        if command != ART and len(data) > MAX_REPLY:
            raise ValueError('Reply exceeds the on-card limit')
        if len(self.records) >= MAX_RECORDS:
            raise ValueError('Too many library records')
        self.records.append((command, kind, handle, start, len(self.blobs), len(data)))
        self.blobs.extend(data)

    def add_json(self, command, value, handle=0, start=0):
        self.add(command, json_bytes(value), handle=handle, start=start)

    def add_title(self, handle, session, slot, offset, length, config_length, seek):
        if len(self.titles) >= MAX_TITLES:
            raise ValueError('Too many titles')
        if not seek or seek[0] != config_length or list(seek) != sorted(seek) or seek[-1] >= length:
            raise ValueError('Invalid seek table')
        self.titles.append((handle, session, slot, offset, length, config_length, len(seek)))
        self.seek_tables.append(seek)

    def build(self):
        record_at = HEADER.size
        title_at = record_at + RECORD.size * len(self.records)
        blob_at = title_at + TITLE.size * len(self.titles)
        seek_at = blob_at + len(self.blobs)
        records = b''.join(RECORD.pack(c, k, 0, h, s, blob_at + o, n, 0) for c, k, h, s, o, n in self.records)
        titles, seek_bytes = bytearray(), bytearray()
        for title, seek in zip(self.titles, self.seek_tables):
            handle, session, slot, offset, length, config, count = title
            titles += TITLE.pack(handle, session, slot, offset, length, config, seek_at + len(seek_bytes), count)
            seek_bytes += struct.pack(f'<{count}I', *seek)
        crc = zlib.crc32(bytes(titles), zlib.crc32(records))
        size = seek_at + len(seek_bytes)
        header = HEADER.pack(MAGIC, VERSION, len(self.records), record_at, len(self.titles), title_at, size, crc)
        return header + records + bytes(titles) + bytes(self.blobs) + bytes(seek_bytes)


def load_library(data):
    """Rebuild a LibraryBuilder from a written library, checking every field build() sets."""
    magic, version, records, record_at, titles, title_at, size, crc = HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION or size != len(data) or record_at != HEADER.size:
        raise ValueError('Not a library file this writer made')
    if title_at != record_at + RECORD.size * records:
        raise ValueError('Library layout differs')
    blob_at = title_at + TITLE.size * titles
    raw_records = data[record_at:title_at]
    raw_titles = data[title_at:blob_at]
    if zlib.crc32(raw_titles, zlib.crc32(raw_records)) != crc:
        raise ValueError('Library index damaged')
    builder = LibraryBuilder()
    entries, end = [], blob_at
    for c, k, _, h, st, offset, n, _ in RECORD.iter_unpack(raw_records):
        if offset < blob_at or offset + n > len(data):
            raise ValueError('Record outside the file')
        entries.append((c, k, h, st, offset, n))
        end = max(end, offset + n)
    seek_at = None
    for handle, session, slot, offset, length, config, seek_pos, count in TITLE.iter_unpack(raw_titles):
        seek_at = seek_pos if seek_at is None else min(seek_at, seek_pos)
        seek = list(struct.unpack_from(f'<{count}I', data, seek_pos))
        builder.add_title(handle, session, slot, offset, length, config, seek)
    seek_at = len(data) if seek_at is None else seek_at
    if end > seek_at:
        raise ValueError('Records overlap the seek tables')
    for c, k, h, st, offset, n in entries:
        builder.add(c, data[offset:offset + n], kind=k, handle=h, start=st)
    return builder


def retile(path):
    """Give an existing card library the My Media "On this Pocket" tile without re-encoding.

    Only for a library whose 'On this Pocket' row still lists every title on the card."""
    path = Path(path)
    builder = load_library(path.read_bytes())
    homes = [r for r in builder.records if r[0] == HOME]
    if len(homes) != 1:
        raise ValueError('Library must have exactly one Home record')
    home = json.loads(bytes(builder.blobs[homes[0][4]:homes[0][4] + homes[0][5]]))
    row = next((r for r in home['rows'] if r.get('title') == ON_DEVICE_TITLE), None)
    if row is None:
        raise ValueError('Library has no On this Pocket row; it may already have the tile')
    stored = {t[0] for t in builder.titles}
    listed = {item['handle'] for item in row['items']}
    if not stored <= listed:
        raise ValueError('The row does not list every title on the card; run a full sync instead')
    if any(r[0] == BROWSE and r[2] == ON_DEVICE_HANDLE for r in builder.records):
        raise ValueError('Library already has the tile')
    count = finish_home(builder, row['items'])
    data = builder.build()
    load_library(data)  # the result must read back
    temp = path.with_suffix('.pjl.new')
    temp.write_bytes(data)
    temp.replace(path)
    return count


class PackWriter:
    """Appends titles to numbered pack files, one per APF data slot."""

    def __init__(self, folder):
        self.folder = Path(folder)
        self.index = 0
        self.used = 0
        self.names = []

    def write_title(self, packets, estimate):
        """Stream encoded PJAV packets into a pack. Returns placement and seek table.

        A failed title is cut back out, so one bad file cannot poison a pack.
        """
        if self.used and self.used + estimate > PACK_LIMIT:
            self.index += 1
            self.used = 0
        if self.index >= MAX_PACKS:
            raise ValueError('All pack slots are full')
        name = f'pack{self.index:02d}.pjs'
        path = self.folder / name
        start = self.used
        seek, config, length = [], 0, 0
        with path.open('ab') as file:
            try:
                for data in packets:
                    kind = data[5]
                    if kind == 1:
                        config = len(data)
                    elif kind == 2 and int.from_bytes(data[16:24], 'little') >= len(seek) * SEEK_SECONDS * 90000:
                        seek.append(length)
                    file.write(data)
                    length += len(data)
                    if start + length > PACK_LIMIT:
                        raise ValueError('Title exceeds a pack')
                if not config or not seek:
                    raise ValueError('Incomplete title')
            except BaseException:
                file.truncate(start)
                raise
        seek[0] = config  # the first picture may follow early audio; start right after CONFIG
        if name not in self.names:
            self.names.append(name)
        self.used += length
        return FIRST_PACK_SLOT + self.index, start, length, config, seek


def annotate(value, playable):
    """Mark items that exist in the snapshot but have no video on this card."""
    if isinstance(value, dict):
        if 'handle' in value and 'title' in value and not value.get('folder'):
            if value['handle'] not in playable:
                value['unavailable'] = True
        for child in value.values():
            annotate(child, playable)
    elif isinstance(value, list):
        for child in value:
            annotate(child, playable)
    return value


def record_item(gateway, builder, item):
    handle = item['handle']
    # One unreadable image must not cost the whole library; the app has placeholders.
    if not item.get('folder'):
        try:
            builder.add_json(DETAILS, gateway.dispatch(DETAILS, {'handle': handle}), handle=handle)
        except Exception:
            pass
    for kind in ART_KINDS:
        try:
            header = gateway.dispatch(ART, {'handle': handle, 'kind': kind})
            total = struct.unpack('<I', header[:4])[0]
            if not total:
                continue
            data = bytearray(header[8:])
            while len(data) < total:
                data += gateway.dispatch(ART, {'handle': handle, 'kind': kind, 'offset': len(data)})[8:]
        except Exception:
            continue
        builder.add(ART, bytes(data), kind=kind, handle=handle)


ON_DEVICE_HANDLE = 0x7ffe0000  # outside gateway handles (1..16384) and local files (0x7fff....)
ON_DEVICE_TITLE = 'On this Pocket'
PAGE = 12  # items per BROWSE reply, as the gateway pages


def on_device_tile():
    """The My Media tile that opens everything stored on this card (like Downloads)."""
    return {'handle': ON_DEVICE_HANDLE, 'title': ON_DEVICE_TITLE, 'subtitle': '', 'rating': '',
            'type': 'CollectionFolder', 'folder': True, 'duration_ticks': 0, 'resume_ticks': 0,
            'played': False, 'year': None}


def finish_home(builder, on_card):
    """Show the card's titles as an "On this Pocket" tile in My Media, next to Movies and
    Shows, instead of a separate row the user has to scroll down to find.

    Replaces the Home record, removes any 'On this Pocket' row, and records the tile's
    folder pages. on_card is every title stored on the card, in display order."""
    seen, items = set(), []
    for item in on_card:
        if item['handle'] not in seen:
            seen.add(item['handle'])
            items.append(item)
    home = None
    for index, record in enumerate(builder.records):
        if record[0] == HOME:
            offset, size = record[4], record[5]
            home = json.loads(bytes(builder.blobs[offset:offset + size]))
            del builder.records[index]
            break
    if home is None:
        home = {'rows': [], 'truncated': False}
    home['rows'] = [row for row in home['rows'] if row.get('title') != ON_DEVICE_TITLE]
    media = next((row for row in home['rows'] if row.get('title') == 'My Media'), None)
    if media is None:
        media = {'title': 'My Media', 'shape': ART_THUMB, 'items': []}
        home['rows'].insert(0, media)
    media['items'] = [item for item in media['items'] if item.get('handle') != ON_DEVICE_HANDLE]
    # Keep the tile even in a full row: it takes the last place rather than a library's.
    media['items'] = media['items'][:11] + [on_device_tile()]
    home['rows'] = home['rows'][:12]
    builder.add_json(HOME, home)
    total = len(items)
    for start in range(0, max(total, 1), PAGE):
        page = items[start:start + PAGE]
        builder.add_json(BROWSE, {'items': page, 'shape': ART_POSTER, 'start': start,
                                  'next': start + len(page), 'total': total},
                         handle=ON_DEVICE_HANDLE, start=start)
    return total


def record_library(gateway, builder, on_card, *, folder_limit=400, pages_per_folder=100):
    """Record Home, folders, details and art for everything the card will show.

    on_card is the public items with video on this card, in display order.
    """
    home = gateway.dispatch(HOME, {})
    playable = {item['handle'] for item in on_card}
    seen, queue = set(), []

    def visit(item):
        if item['handle'] in seen or builder.full:
            return
        seen.add(item['handle'])
        if item.get('folder'):
            queue.append(item['handle'])
        record_item(gateway, builder, item)

    for item in on_card:
        visit(item)
    for row in home['rows']:
        for item in row['items']:
            visit(item)
    folders = 0
    while queue and folders < folder_limit and not builder.full:
        handle = queue.pop(0)
        folders += 1
        start = 0
        for _ in range(pages_per_folder):
            page = gateway.dispatch(BROWSE, {'handle': handle, 'start': start})
            for item in page['items']:
                visit(item)
            builder.add_json(BROWSE, annotate(page, playable), handle=handle, start=start)
            if page['next'] >= page['total'] or not page['items']:
                break
            start = page['next']
    if on_card:
        home['rows'].insert(0, {'title': 'On this Pocket', 'shape': ART_POSTER, 'items': on_card[:12]})
    home['rows'] = home['rows'][:12]
    builder.add_json(HOME, annotate(home, playable))
    return len(seen)
