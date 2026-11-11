# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Rasterize the app's Roboto Medium glyph atlas straight from Google's Roboto font file.

    py -3 tools/fetch_upstreams.py --font      # Roboto 2.138 (Apache-2.0) into upstream/roboto/
    py -3 tools/build_roboto_atlas.py          # -> pocket/fonts/roboto-medium-18.a8 and .json
    py -3 tools/build_ui_font.py               # -> pocket/ui_font.h and pocket/ui_font_large.h

The atlas holds the 95 printable ASCII characters (32..126), each in a cell of CELL_W x CELL_H 8-bit
coverage values, plus a JSON file with the cell size and each character's advance. Glyphs are drawn
1.125 times wider than tall (the proportions the app's layout was made with), on a fixed baseline,
with the baseline, x-height and cap height placed on whole pixels so horizontal strokes stay sharp.

The rasterizer is plain Python (fontTools reads the outlines), so the same font file always gives
the same atlas on any PC. The font file is checked against the SHA-256 in upstream.lock.json.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

from fontTools.pens.basePen import BasePen
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'pocket/fonts'
NAME = 'roboto-medium-18'

PX = 18             # em size in pixels
STRETCH = 1.125     # horizontal scale
CELL_W, CELL_H = 29, 25
ORIGIN_X = 1        # pen position inside the cell
BASELINE = 17       # baseline row inside the cell (rows above it are ink above the baseline)
INK_ROWS = (2, 20)  # the band the app's layout reserves; the faint overshoot outside it (under 6%) is dropped
SUBROWS = 16        # vertical samples per pixel; horizontal coverage is exact
CURVE_STEPS = 12    # line segments per quadratic or cubic curve


def lock_entry():
    lock = json.loads((ROOT / 'upstream.lock.json').read_text(encoding='utf-8'))
    return lock['download']['roboto']


class EdgePen(BasePen):
    """Collects an outline as straight edges in font units."""

    def __init__(self, glyph_set):
        super().__init__(glyph_set)
        self.edges = []
        self.start = self.last = None

    def _moveTo(self, point):
        self.start = self.last = point

    def _lineTo(self, point):
        self.edges.append((self.last, point))
        self.last = point

    def _qCurveToOne(self, control, point):
        (x0, y0), (x1, y1), (x2, y2) = self.last, control, point
        prev = self.last
        for step in range(1, CURVE_STEPS + 1):
            t = step / CURVE_STEPS
            u = 1 - t
            here = (u * u * x0 + 2 * u * t * x1 + t * t * x2, u * u * y0 + 2 * u * t * y1 + t * t * y2)
            self.edges.append((prev, here))
            prev = here
        self.last = point

    def _curveToOne(self, c1, c2, point):
        (x0, y0), (x1, y1), (x2, y2), (x3, y3) = self.last, c1, c2, point
        prev = self.last
        for step in range(1, CURVE_STEPS + 1):
            t = step / CURVE_STEPS
            u = 1 - t
            here = (u ** 3 * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t ** 3 * x3,
                    u ** 3 * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t ** 3 * y3)
            self.edges.append((prev, here))
            prev = here
        self.last = point

    def _closePath(self):
        if self.last != self.start:
            self.edges.append((self.last, self.start))
        self.last = self.start


def vertical_map(font):
    """Font units (y up) -> pixels above the baseline, with x-height and cap height on whole pixels."""
    scale = PX / font['head'].unitsPerEm
    os2 = font['OS/2']
    x_height, cap_height = os2.sxHeight, os2.sCapHeight
    x_px, cap_px = round(x_height * scale), round(cap_height * scale)

    def to_px(y):
        if y <= 0:
            return y * scale
        if y <= x_height:
            return y * x_px / x_height
        if y <= cap_height:
            return x_px + (y - x_height) * (cap_px - x_px) / (cap_height - x_height)
        return cap_px + (y - cap_height) * scale
    return to_px


def rasterize(edges):
    """Nonzero-winding coverage of pixel-space edges into a CELL_W x CELL_H byte cell."""
    cell = bytearray(CELL_W * CELL_H)
    edges = [(x0, y0, x1, y1) for (x0, y0), (x1, y1) in edges if y0 != y1]
    for row in range(CELL_H):
        acc = [0.0] * CELL_W
        for sub in range(SUBROWS):
            yc = row + (sub + 0.5) / SUBROWS
            crossings = []
            for x0, y0, x1, y1 in edges:
                if (y0 <= yc < y1) or (y1 <= yc < y0):
                    crossings.append((x0 + (yc - y0) * (x1 - x0) / (y1 - y0), 1 if y1 > y0 else -1))
            crossings.sort()
            winding, span_start = 0, 0.0
            for x, direction in crossings:
                before = winding
                winding += direction
                if before == 0 and winding != 0:
                    span_start = x
                elif before != 0 and winding == 0:
                    add_span(acc, span_start, x)
        if not INK_ROWS[0] <= row <= INK_ROWS[1]:
            continue
        for column in range(CELL_W):
            cell[row * CELL_W + column] = max(0, min(255, round(255 * acc[column] / SUBROWS)))
    return cell


def add_span(acc, a, b):
    a, b = max(0.0, a), min(float(CELL_W), b)
    if b <= a:
        return
    ia, ib = int(math.floor(a)), int(math.floor(b))
    if ia == ib:
        acc[ia] += b - a
        return
    acc[ia] += ia + 1 - a
    for column in range(ia + 1, min(ib, CELL_W)):
        acc[column] += 1.0
    if ib < CELL_W:
        acc[ib] += b - ib


def build(font_path):
    data = Path(font_path).read_bytes()
    expected = lock_entry()['ttf_sha256']
    actual = hashlib.sha256(data).hexdigest()
    if actual != expected:
        raise SystemExit(f'{font_path}: SHA-256 {actual} is not the pinned Roboto file ({expected})')
    font = TTFont(font_path)
    glyph_set, cmap = font.getGlyphSet(), font.getBestCmap()
    scale_x = PX / font['head'].unitsPerEm * STRETCH
    to_px = vertical_map(font)
    atlas, advances = bytearray(), []
    for code in range(32, 127):
        name = cmap[code]
        pen = EdgePen(glyph_set)
        glyph_set[name].draw(pen)
        edges = [((ORIGIN_X + x0 * scale_x, BASELINE - to_px(y0)), (ORIGIN_X + x1 * scale_x, BASELINE - to_px(y1)))
                 for (x0, y0), (x1, y1) in pen.edges]
        atlas += rasterize(edges)
        advances.append(round(font['hmtx'][name][0] * scale_x))
    meta = {'family': 'Roboto Medium', 'px': PX, 'cellW': CELL_W, 'cellH': CELL_H, 'ox': ORIGIN_X,
            'baseline': BASELINE, 'stretch': STRETCH, 'adv': advances,
            'source': f"Roboto-Medium.ttf {font['name'].getDebugName(5)}, sha256 {actual}",
            'license': 'Apache-2.0', 'generator': 'tools/build_roboto_atlas.py'}
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / f'{NAME}.a8').write_bytes(bytes(atlas))
    (OUT / f'{NAME}.json').write_text(json.dumps(meta, indent=1) + '\n', encoding='ascii', newline='\n')
    print(f'{NAME}: {len(atlas)} bytes, atlas sha256 {hashlib.sha256(atlas).hexdigest()}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--font', default=str(ROOT / 'upstream/roboto/Roboto-Medium.ttf'),
                        help='Roboto-Medium.ttf (default: upstream/roboto, from tools/fetch_upstreams.py --font)')
    build(parser.parse_args().font)
