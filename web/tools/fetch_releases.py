# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Darren Chi
"""Copy the firmware of the GitHub releases into the web app, as the Pages
workflow (.github/workflows/pages.yml) does before publishing it, or to try
the web app locally:

- web/firmware/releases/<tag>/: every release since 2.0.0 (older ones have
  another settings layout), with releases.json listing them newest first,
  for the web app's "Or another release";
- web/firmware/latest/: the newest of them, with latest.json, for "Use the
  latest release".

Each file is checked against its release's SHA256SUMS.txt (a mismatch stops
it). Needs the GitHub CLI (gh), logged in. Both folders are git-ignored.

    python web/tools/fetch_releases.py [--repo owner/name]
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

WEB = Path(__file__).resolve().parent.parent
RELEASES = WEB / 'firmware' / 'releases'
LATEST = WEB / 'firmware' / 'latest'
FIRST = (2, 0, 0)
RE_TAG = re.compile(r'v(\d+)\.(\d+)\.(\d+)')
RE_BIN = re.compile(r'bx-tacho-v[\w.-]+\.bin')   # the update file, not the merged image


def gh(*args):
    # UTF-8 whatever the console's code page (release notes aren't ASCII)
    return subprocess.check_output(['gh', *args], encoding='utf-8')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY'),
                    help='owner/name (default: $GITHUB_REPOSITORY, or this checkout\'s GitHub repository)')
    repo = ap.parse_args().repo or gh('repo', 'view', '--json', 'nameWithOwner', '-q', '.nameWithOwner').strip()

    index = []
    for r in json.loads(gh('api', f'repos/{repo}/releases?per_page=100')):
        m = RE_TAG.fullmatch(r['tag_name'])
        if r['draft'] or r['prerelease'] or not m or tuple(map(int, m.groups())) < FIRST:
            continue
        names = [a['name'] for a in r['assets']]
        bins = [n for n in names if RE_BIN.fullmatch(n) and 'merged' not in n]
        if not bins or 'SHA256SUMS.txt' not in names:
            print(f"{r['tag_name']}: no firmware or no checksums, left out")
            continue
        d = RELEASES / r['tag_name']
        d.mkdir(parents=True, exist_ok=True)
        subprocess.check_call(['gh', 'release', 'download', r['tag_name'], '--repo', repo, '--dir', str(d),
                               '--pattern', bins[0], '--pattern', 'SHA256SUMS.txt', '--clobber'])
        sums = dict(reversed(line.split()) for line in (d / 'SHA256SUMS.txt').read_text().splitlines() if line.strip())
        data = (d / bins[0]).read_bytes()
        if hashlib.sha256(data).hexdigest() != sums.get(bins[0]):
            raise SystemExit(f"{r['tag_name']}: {bins[0]} doesn't match its SHA256SUMS.txt")
        index.append(({'tag': r['tag_name'], 'version': r['tag_name'][1:], 'url': r['html_url'],
                       'published': r['published_at'], 'file': bins[0], 'size': len(data),
                       'sha256': sums[bins[0]]}, tuple(map(int, m.groups()))))
    index = [e for e, _ in sorted(index, key=lambda x: x[1], reverse=True)]
    RELEASES.mkdir(parents=True, exist_ok=True)
    (RELEASES / 'releases.json').write_text(json.dumps(index, indent=2))
    print(f"{len(index)} releases:", ' '.join(e['tag'] for e in index))

    if not index:
        print('No release with firmware yet: no latest')
        return
    newest = index[0]
    if LATEST.exists():
        shutil.rmtree(LATEST)
    LATEST.mkdir(parents=True)
    src = RELEASES / newest['tag']
    for name in (newest['file'], 'SHA256SUMS.txt'):
        shutil.copy2(src / name, LATEST / name)
    (LATEST / 'latest.json').write_text(json.dumps(newest, indent=2))
    print(f"latest: {newest['tag']}")


if __name__ == '__main__':
    main()
