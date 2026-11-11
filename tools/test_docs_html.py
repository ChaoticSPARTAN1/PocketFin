# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""The documentation copies: Open-Documentation.html, docs/index.html and the PDF guide. PC only.

  current    Open-Documentation.html and docs/index.html (GitHub Pages copy) equal a fresh build from the
             .md files (tools/build_docs_html.py); the PDF guide's receipt (docs/.guide-source.sha256)
             matches the pages and build scripts of today and the PDF's own bytes (tools/build_docs_pdf.py)
  offline    a strict Content-Security-Policy; no <script src>, <link>, <iframe>, <base> or similar; every
             picture and srcset is a file in the repository (or inline data); the script has no
             fetch/XMLHttpRequest/WebSocket; http(s) addresses appear only as links people click (or as
             text), and every such link opens in a new tab without a referrer
  links      every page link and #anchor points at an embedded page and heading; the GitHub links to the
             sibling projects are there
  private    only README.md, docs/*.md and the four notices are embedded, all tracked by Git (when Git is
             available); no home-folder paths in the pages or the PDF; the PDF names only the project
No network, no device, standard library only (the PDF is checked, not printed again).
"""
import hashlib
import json
import re
import subprocess
import sys
import unittest
from html.parser import HTMLParser

import build_docs_html as docs
from toolpaths import ROOT

PDF = ROOT / 'docs' / f'{docs.PROJECT}-Guide.pdf'
RECEIPT = ROOT / 'docs/.guide-source.sha256'
SIBLINGS = ('https://github.com/ChaoticSPARTAN1/Jellymatic', 'https://github.com/ChaoticSPARTAN1/PocketFin')
LOADS = {'src', 'srcset', 'poster', 'data', 'action', 'formaction', 'background', 'manifest', 'ping', 'xlink:href'}
FORBIDDEN_TAGS = {'link', 'iframe', 'frame', 'object', 'embed', 'base', 'form', 'audio', 'video', 'track', 'portal'}
LOCAL_PATHS = re.compile(rb'(?i)\b[A-Z]:\\{1,2}Users\\|/Users/|/home/|file:///')


class Tags(HTMLParser):
    def __init__(self, text):
        super().__init__(convert_charrefs=True)
        self.tags, self.ids = [], set()
        self.feed(text)
        self.close()

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        self.tags.append((tag, attrs))
        if attrs.get('id'):
            self.ids.add(attrs['id'])

    handle_startendtag = handle_starttag


class Copy:
    """One built HTML copy, parsed."""

    def __init__(self, variant, out):
        self.variant, self.out, self.base = variant, out, out.parent
        self.page = out.read_text(encoding='utf-8').replace('\r\n', '\n')
        blob = re.search(r'<script type="application/json" id="docs-data">\n(.*?)\n</script>', self.page, re.S)
        self.shell = self.page.replace(blob.group(0), '')
        data = json.loads(blob.group(1))
        self.pages = {p['path']: p for g in data['groups'] for p in g['pages']}
        self.parsed = {path: Tags(p['html']) for path, p in self.pages.items()}

    def local(self, url):
        return url.startswith('data:image/') or (self.base / url.split('#')[0].split('?')[0]).is_file()


class OfflineDocumentation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.copies = [Copy(variant, out) for variant, out in docs.OUTPUTS.items()]

    def test_pages_are_current(self):
        for copy in self.copies:
            with self.subTest(copy.out.name):
                self.assertTrue(copy.page == docs.build(copy.variant), f'{copy.out} is out of date: run py -3 {docs.SCRIPT_NAME}')
        self.assertTrue((ROOT / 'docs/.nojekyll').is_file(), 'docs/.nojekyll (GitHub Pages serves files as they are)')

    def test_strict_content_security_policy(self):
        for copy in self.copies:
            with self.subTest(copy.out.name):
                outer = Tags(copy.page)
                csp = [a.get('content') for t, a in outer.tags
                       if t == 'meta' and (a.get('http-equiv') or '').lower() == 'content-security-policy']
                self.assertEqual(csp, [docs.CSP])
                self.assertIn("default-src 'none'", csp[0])
                self.assertNotIn('http', csp[0])
                self.assertFalse([t for t, a in outer.tags if t in FORBIDDEN_TAGS])
                self.assertFalse([a for t, a in outer.tags if t == 'script' and 'src' in a])

    def test_nothing_is_loaded_from_the_internet(self):
        for copy in self.copies:
            with self.subTest(copy.out.name):
                code = re.findall(r'<script>(.*?)</script>', copy.page, re.S)[-1]
                self.assertIsNone(re.search(r'\b(fetch|XMLHttpRequest|WebSocket|EventSource|sendBeacon|importScripts|import\s*\()', code))
                style = ''.join(re.findall(r'<style>(.*?)</style>', copy.page, re.S))
                self.assertNotIn('@import', style)
                for url in re.findall(r'url\(\s*["\']?([^"\')]+)', style):
                    self.assertFalse(re.match(r'(?i)[a-z]+:|//', url), url)
                    self.assertTrue(copy.local(url), url)
                self.assertIsNone(re.search(r'(?i)https?:', copy.shell), 'an address outside the embedded pages')
                bad, pictures = [], 0
                for path, doc in copy.parsed.items():
                    for tag, attrs in doc.tags:
                        if tag in FORBIDDEN_TAGS or tag in ('script', 'style'):
                            bad.append(f'{path}: <{tag}>')
                        for key, value in attrs.items():
                            if key.startswith('on') or (key == 'style' and value and 'url(' in value):
                                bad.append(f'{path}: {key}')
                            if value and re.search(r'(?i)https?:|//', value) and not (tag == 'a' and key == 'href') \
                                    and not value.startswith('data:'):
                                bad.append(f'{path}: <{tag} {key}={value}>')
                            if key in LOADS and value is not None:
                                for part in ([value] if value.startswith('data:') else value.split(',') if key == 'srcset' else [value]):
                                    url = part.strip().split(' ')[0]
                                    pictures += 1
                                    if (re.match(r'(?i)[a-z][a-z0-9+.-]*:|//', url) and not url.startswith('data:image/')) \
                                            or not copy.local(url):
                                        bad.append(f'{path}: {key}={url[:60]}')
                self.assertFalse(bad, bad[:10])
                self.assertGreater(pictures, 10)

    def test_web_links_open_in_a_new_tab_and_reach_the_siblings(self):
        for copy in self.copies:
            with self.subTest(copy.out.name):
                external, unsafe = [], []
                for path, doc in copy.parsed.items():
                    for tag, attrs in doc.tags:
                        if tag == 'a' and re.match(r'(?i)https?:', attrs.get('href') or ''):
                            external.append(attrs['href'])
                            rel = attrs.get('rel') or ''
                            if attrs.get('target') != '_blank' or 'noopener' not in rel or 'noreferrer' not in rel:
                                unsafe.append(f'{path}: {attrs["href"]}')
                self.assertFalse(unsafe, unsafe[:5])
                for sibling in SIBLINGS:
                    self.assertTrue(any(h.rstrip('/').startswith(sibling) for h in external), sibling)

    def test_page_links_and_anchors_resolve(self):
        for copy in self.copies:
            with self.subTest(copy.out.name):
                broken = []
                for path, doc in copy.parsed.items():
                    for tag, attrs in doc.tags:
                        href = attrs.get('href') or ''
                        if tag != 'a' or not href:
                            continue
                        if href.startswith('#'):
                            target, _, anchor = href[1:].partition(':')
                            if target not in copy.pages or (anchor and 'h-' + anchor.lower() not in copy.parsed[target].ids):
                                broken.append(f'{path} -> {href}')
                        elif not re.match(r'(?i)[a-z][a-z0-9+.-]*:', href) and not (copy.base / href.split('#')[0]).exists():
                            broken.append(f'{path} -> {href} (file)')
                self.assertFalse(broken, broken[:10])

    def test_only_tracked_documentation_is_embedded(self):
        try:
            tracked = set(subprocess.run(['git', '-C', str(ROOT), 'ls-files'], capture_output=True, text=True,
                                         check=True, timeout=60).stdout.split('\n'))
        except (OSError, subprocess.SubprocessError):
            tracked = set()
        for copy in self.copies:
            with self.subTest(copy.out.name):
                self.assertEqual(set(copy.pages), set(docs.page_paths()))
                self.assertIn('README.md', copy.pages)
                for path in copy.pages:
                    self.assertTrue(path == 'README.md' or re.fullmatch(r'docs/[^/]+\.md', path) or path in docs.EXTRA, path)
                if len(tracked) > 1:
                    self.assertFalse([p for p in copy.pages if p not in tracked])
                self.assertIsNone(LOCAL_PATHS.search(copy.page.encode('utf-8')))

    def test_pdf_guide_is_current_and_clean(self):
        receipt = dict(line.split(' ', 1) for line in RECEIPT.read_text(encoding='ascii').split('\n') if ' ' in line)
        pdf = PDF.read_bytes()
        self.assertEqual(receipt.get('sources'), docs.source_hash(),
                         f'{PDF.name} is out of date: run py -3 {docs.SCRIPT_NAME.replace("html", "pdf")}')
        self.assertTrue(pdf.startswith(b'%PDF-'))
        self.assertEqual(receipt.get('pdf'), hashlib.sha256(pdf).hexdigest(), 'the PDF is not the one the receipt names')
        info = re.findall(rb'/Info\s+(\d+)\s+0\s+R', pdf)
        last = re.search(rb'\n' + info[-1] + rb' 0 obj\s*(<<.*?>>)', pdf, re.S).group(1)
        self.assertIn(b'/Author (ChaoticSPARTAN1)', last)
        for word in (b'Chromium', b'Skia', b'Date'):
            self.assertNotIn(word, last)
        self.assertIsNone(LOCAL_PATHS.search(pdf))
        self.assertLess(len(pdf), 15 * 1048576)


if __name__ == '__main__':
    sys.exit(not unittest.main(exit=False).result.wasSuccessful())
