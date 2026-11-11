# Contributing to PocketFin

Thank you for helping. Bug reports, bug fixes, documentation fixes and pull requests are welcome.
PocketFin is a hobby project: there is no guarantee of a response or a fix, or of when.

## Reporting a bug

Open an issue on GitHub with:

- what you did, what you expected and what happened;
- the PocketFin version (core.json, or the release you downloaded) and your Pocket firmware version;
- whether you used the card on its own or a Chromatic (and its Jellymatic version);
- what the start screen shows (the self-test lines);
- if you can, `Saves/pocketlive/common/pjlive_log.sav` from your card.

**Never post** Wi-Fi network names or passwords, server addresses, user names, API keys or tokens,
or logs and screenshots that show them. Remove them first. Do not post copyrighted videos.
Security problems: see [SECURITY.md](SECURITY.md), not a public issue.

## Pull requests

1. Keep changes small and focused, with a short description of why.
2. Run the tests in [docs/Building-from-Source.md](docs/Building-from-Source.md#5-run-the-tests) and
   say which you ran.
3. Do not change `release/` in the same pull request as source changes unless you rebuilt it with
   `tools/package_release.py`; say so if you did.
4. Never change `interact.json` in the release without a tested reason: other versions hung the Pocket.
5. New third-party code or assets must have a licence compatible with GPL-3.0-or-later; add them to
   [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
6. Keep the "ChaoticSPARTAN1" attribution (see [ADDITIONAL-TERMS.md](ADDITIONAL-TERMS.md)).
7. New source files start with `SPDX-License-Identifier: GPL-3.0-or-later` and the line
   `Additional terms and permissions: ADDITIONAL-TERMS.md`, like the existing ones.
8. After editing any documentation page, run `py -3 tools\build_docs_html.py` (Open-Documentation.html
   and docs/index.html) and `py -3 tools\build_docs_pdf.py` (the PDF guide); `tools\test_docs_html.py`
   checks all three. When the repository is public, GitHub Pages can serve the docs: Settings > Pages,
   branch `main`, folder `/docs`; the site is `https://chaoticspartan1.github.io/PocketFin/`.

By contributing you agree that your contribution is accepted under the same licence
(GPL-3.0-or-later), with the same additional terms and the same additional permission as the rest of
PocketFin ([ADDITIONAL-TERMS.md](ADDITIONAL-TERMS.md)). Changes to third-party files keep that
file's own licence (for example, patches to openfpgaCore's Apache-2.0 files are Apache-2.0).
