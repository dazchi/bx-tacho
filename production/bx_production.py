# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Darren Chi
"""BX Tacho production flasher.

Waits for an ESP32-C3 on USB, erases the whole chip and writes the merged
firmware image (bootloader, partition table, otadata and app), lets it start,
reads its IDs from the firmware console (`id`) and records them in the
production database (boarddb.py).
Then it asks for the next board. A local web page shows what it's doing.

    python bx_production.py                    # the latest release from GitHub
    python bx_production.py --release v2.5.0
    python bx_production.py --firmware my-merged.bin

Needs esptool and pyserial (requirements.txt; both come with ESP-IDF).
"""

import argparse
import csv
import io
import datetime
import hashlib
import json
import os
import re
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial
import serial.tools.list_ports

try:
    import certifi   # Python from python.org on macOS has no CA certificates of its own
except ImportError:
    certifi = None

import boarddb
import vbatcal

HERE = Path(__file__).resolve().parent
REPO = 'dazchi/bx-tacho'

# The ESP32-C3's built-in USB Serial/JTAG (also on a blank chip)
ESP_VID, ESP_PID = 0x303A, 0x1001

POLL_S = 0.5
# A new board's port must stay put this long before flashing: right after
# plugging in Windows may not let it be opened yet, a blank chip keeps
# resetting (no app to start), its USB port dropping out each time, and a
# board with firmware dropped out once about 2 s after plugging in (1.5 s
# wasn't enough: the first esptool run always hit that)
SETTLE_S = 3.0
# The Database tab only reads the IDs, so it looks a board up sooner; if the
# port drops out after all, the board is looked up again when it's back
DETECT_SETTLE_S = 1.0
DETECT_POLL_S = 0.2
SETTLE_GIVE_UP_S = 15   # still unsettled after this: ask for BOOT held while plugging in
GONE_S = 5              # missing this long while settling: unplugged
OPEN_RETRIES = 3        # esptool couldn't open the port: find the board again and retry
FLASH_BAUD = 921600
BOOT_WAIT_S = 20        # for the port to come back after the reset into the new firmware
ID_TIMEOUT_S = 45       # for the console to answer `id`
LOG_LINES = 300

# An update (Update tab) writes only these parts of the merged image
# (firmware/partitions.csv): the bootloader and partition table, then the
# ota_0 app slot and a blank otadata (so ota_0 boots, also on a board that
# last ran ota_1 after a Bluetooth update). nvs (settings, shot history) and
# phy_init between them, and fctry (VBAT calibration) above, aren't touched.
UPDATE_PARTS = [(0x0, 0x9000), (0x10000, 0x110000)]

# ---------------------------------------------------------------- firmware

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 16), b''):
            h.update(chunk)
    return h.hexdigest()


def version_key(tag):
    """v2.10.0 after v2.9.1: the numbers, compared as numbers"""
    return tuple(int(n) for n in re.findall(r'\d+', tag))


# A release's merged image: bx-tacho-v2.4.0-merged.bin (bx-tacho-merged.bin in v1.1.0)
RE_MERGED = re.compile(r'^bx-tacho(-v[^/]+)?-merged\.bin$')
RELEASES_CACHE_S = 120   # GitHub allows 60 unauthenticated API requests an hour
_releases = {'at': 0.0, 'list': None}


def list_releases():
    """The firmware releases, newest version first: [{tag, asset, published,
    latest, downloaded}], and an error (offline: only the ones downloaded before)"""
    if _releases['list'] is not None and time.time() - _releases['at'] < RELEASES_CACHE_S:
        out, error = [dict(r) for r in _releases['list']], None
    else:
        out, error = _ask_releases()
    for r in out:   # now, not when GitHub was asked
        r['downloaded'] = (HERE / 'firmware' / r['tag'] / r['asset']).exists()
    return out, error


# HTTPS to GitHub: with certifi's CA bundle when it's installed (python.org's
# Python on macOS can't verify certificates without it), else the system's
SSL_CONTEXT = ssl.create_default_context(cafile=certifi.where()) if certifi else None


def _urlopen(url, timeout):
    return urllib.request.urlopen(url, timeout=timeout, context=SSL_CONTEXT)


def _ask_releases():
    try:
        req = urllib.request.Request(f'https://api.github.com/repos/{REPO}/releases?per_page=100',
                                     headers={'Accept': 'application/vnd.github+json'})
        with _urlopen(req, timeout=15) as r:
            data = json.load(r)
        out = []
        for rel in data:
            merged = [a['name'] for a in rel.get('assets', []) if RE_MERGED.match(a['name'])]
            if rel.get('draft') or not merged:
                continue   # hardware releases have no firmware
            out.append({'tag': rel['tag_name'], 'asset': merged[0], 'published': (rel.get('published_at') or '')[:10],
                        'prerelease': bool(rel.get('prerelease'))})
        error = None
    except (OSError, ValueError, KeyError) as e:
        out, error = [], f'could not ask GitHub for the releases ({e}): showing the ones downloaded before'
        for folder in (HERE / 'firmware').glob('v*'):
            merged = [f.name for f in folder.glob('*.bin') if RE_MERGED.match(f.name)]
            if merged:
                out.append({'tag': folder.name, 'asset': merged[0], 'published': '', 'prerelease': False})
    out.sort(key=lambda r: version_key(r['tag']), reverse=True)
    stable = [r for r in out if not r['prerelease']]
    for r in out:
        r['latest'] = bool(stable) and r is stable[0]
    if error is None:
        _releases.update(at=time.time(), list=[dict(r) for r in out])
    return out, error


def latest_release():
    """The tag of the latest GitHub release; offline, the newest one downloaded before"""
    try:
        req = urllib.request.Request(f'https://api.github.com/repos/{REPO}/releases/latest',
                                     headers={'Accept': 'application/vnd.github+json'})
        with _urlopen(req, timeout=15) as r:
            return json.load(r)['tag_name']
    except (OSError, ValueError, KeyError) as e:
        have = sorted((p.name for p in (HERE / 'firmware').glob('v*') if p.is_dir()), key=version_key)
        if not have:
            sys.exit(f'could not ask GitHub for the latest release ({e}) and none is downloaded: '
                     'use --release vX.Y.Z or --firmware file.bin')
        print(f'could not ask GitHub for the latest release ({e}); using {have[-1]}, downloaded before')
        return have[-1]


def fetch_release(tag, name=None):
    """The merged image of a GitHub release (`name`, by default
    bx-tacho-<tag>-merged.bin), downloaded once into firmware/<tag>/ and
    checked against the release's SHA256SUMS.txt. Raises RuntimeError."""
    if not re.fullmatch(r'[\w.+-]+', tag) or (name and not RE_MERGED.match(name)):
        raise RuntimeError(f'not a release: {tag} {name or ""}')
    name = name or f'bx-tacho-{tag}-merged.bin'
    folder = HERE / 'firmware' / tag
    folder.mkdir(parents=True, exist_ok=True)
    base = f'https://github.com/{REPO}/releases/download/{tag}'
    sums_path = folder / 'SHA256SUMS.txt'
    image = folder / name
    for file, path in (('SHA256SUMS.txt', sums_path), (name, image)):
        if not path.exists():
            print(f'downloading {base}/{file}')
            try:
                with _urlopen(f'{base}/{file}', timeout=60) as r:
                    data = r.read()
            except OSError as e:
                raise RuntimeError(f'could not download {file} of {tag}: {e}') from None
            tmp = path.with_name(path.name + '.tmp')
            tmp.write_bytes(data)
            os.replace(tmp, path)   # never a half-downloaded file under the real name
    sums = {}
    for line in sums_path.read_text().splitlines():
        parts = line.split()
        if len(parts) == 2:
            sums[parts[1].lstrip('*')] = parts[0].lower()
    if name not in sums:
        raise RuntimeError(f'{name} is not listed in the release\'s SHA256SUMS.txt')
    actual = sha256_file(image)
    if actual != sums[name]:
        image.unlink()
        raise RuntimeError(f'{name}: checksum mismatch (deleted: choose it again to download it again)')
    return image, tag


def image_version(data):
    """The app version in a merged image (the ESP-IDF app description at
    0x10020, after the image and first segment headers), e.g. 'v2.4.0', or None"""
    at = 0x10000 + 24 + 8
    if len(data) < at + 48 or struct.unpack_from('<I', data, at)[0] != 0xABCD5432:
        return None
    return 'v' + data[at + 16:at + 48].split(b'\0')[0].decode('ascii', 'replace')


def check_merged(data):
    """Raises ValueError unless `data` looks like a merged image (bootloader
    at 0x0, partition table at 0x8000, an app with its description at 0x10000)"""
    if len(data) < 0x10100 or data[0] != 0xE9 or data[0x8000:0x8002] != b'\xaa\x50' or data[0x10000] != 0xE9:
        raise ValueError('not a merged image (bootloader at 0x0, partition table at 0x8000, app at 0x10000): '
                         'use bx-tacho-vX.Y.Z-merged.bin')
    if image_version(data) is None:
        raise ValueError('no ESP-IDF app description at 0x10000: not a BX Tacho firmware image')


def load_firmware(image, version, source, label):
    """Everything the worker needs about an image: {image, version, sha,
    source (release | local | file), label, updates, update_error}"""
    fw = {'image': Path(image), 'version': version, 'sha': sha256_file(image), 'source': source, 'label': label}
    try:
        fw['updates'], fw['update_error'] = update_parts(image), None
    except ValueError as e:
        fw['updates'], fw['update_error'] = None, str(e)
    return fw


def update_parts(image):
    """The merged image cut into the parts an update writes, as temporary
    files: [(address, path)]. Raises ValueError if the image isn't laid out as
    expected (then updating is unavailable)."""
    data = Path(image).read_bytes()
    if len(data) > UPDATE_PARTS[-1][1]:
        raise ValueError(f'the image reaches 0x{len(data):x}, into fctry (the calibration)')
    if data[0x8000:0x8002] != b'\xaa\x50' or len(data) <= 0x10000 or data[0x10000] != 0xe9:
        raise ValueError('not a merged image with the partition table at 0x8000 and the app at 0x10000')
    folder = Path(tempfile.mkdtemp(prefix='bx-tacho-update-'))
    parts = []
    for start, end in UPDATE_PARTS:
        part = data[start:end]
        if start == 0x10000:
            part = part.ljust(end - start, b'\xff')   # through otadata, blank
        path = folder / f'update-0x{start:x}.bin'
        path.write_bytes(part)
        parts.append((start, path))
    return parts


# ---------------------------------------------------------------- state shared with the web page

def fw_public(fw):
    return {'file': str(fw['image']), 'version': fw['version'], 'sha256': fw['sha'], 'source': fw['source'],
            'label': fw['label']}


class State:
    def __init__(self, fw, db_path):
        self.lock = threading.Condition()   # an RLock: update() may be called with it held
        self.seq = 0
        self.cal_start = threading.Event()
        self.identifying = False   # the Database tab is reading a board's IDs (its console is open)
        self.identify_lock = threading.Lock()
        self.identified = {}       # board key -> IDs read for the Database tab, for adding it
        self.fw = fw            # load_firmware(): what's flashed; changed with set_firmware
        self.fw_lock = threading.Lock()   # one firmware download or upload at a time
        self.data = {
            # flash: every ESP32-C3 plugged in is erased and flashed; update: its
            # firmware is updated, settings and calibration kept; cal: VBAT
            # calibration, nothing is flashed. Changed only while all are idle.
            'mode': 'flash',
            # Off: boards plugged in are left alone, even in flash or update mode,
            # so a board already calibrated (or a customer's) isn't erased or
            # updated by accident. Off at start and whenever a tab is opened.
            'auto_flash': False,
            'job': 'flash',          # flash | update: what happens to the current (last) board
            'update_error': fw['update_error'],   # why updating is unavailable, or None
            'fw_busy': None,         # what's being done to change the firmware (downloading ...)
            'cal': {
                'phase': 'idle',     # idle | running | done | failed
                'message': '',
                'progress': 0,
                'ppk2': None,        # its port, while one is connected
                'result': None,
                'row': None,
                'error': None,
                'session_count': 0,
                'total_count': 0,
                'recent': [],
            },
            'phase': 'waiting',      # waiting | flashing | identifying | done | failed
            'message': 'Plug in a board',
            'progress': 0,
            'port': None,
            'record': None,
            'error': None,
            'firmware': fw_public(fw),
            'db': str(db_path),
            'db_version': 0,         # changes on every save, for the Database tab to reload
            'session_count': 0,
            'total_count': 0,
            'recent': [],
            'update_session_count': 0,
            'update_total_count': 0,
            'update_recent': [],
            'usb': [],               # boards on USB, settled: [{key, port}] (for the Database tab)
            'log': [],
        }

    def update(self, **kw):
        with self.lock:
            self.data.update(kw)
            self.seq += 1
            self.lock.notify_all()

    def update_cal(self, **kw):
        with self.lock:
            self.data['cal'].update(kw)
            self.seq += 1
            self.lock.notify_all()

    def log(self, text):
        lines = [line.rstrip() for line in text.splitlines() if line.strip()]
        if not lines:
            return
        with self.lock:
            log = self.data['log']
            stamp = time.strftime("%H:%M:%S")
            log.extend(f'{stamp} {line}' for line in lines)
            del log[:-LOG_LINES]
            self.seq += 1
            self.lock.notify_all()

    def snapshot(self, after):
        """Wait (up to 15 s) for a change after `after`; returns (seq, json)."""
        with self.lock:
            self.lock.wait_for(lambda: self.seq != after, timeout=15)
            return self.seq, json.dumps(self.data)


# ---------------------------------------------------------------- records

def refresh_counts(state, db_path):
    """The counts and latest boards on the Flash and VBAT tabs, after a save"""
    sm = boarddb.summary(db_path)
    with state.lock:
        state.data['cal'].update(total_count=sm['calibrated'], recent=sm['recent_cal'])
        state.update(total_count=sm['flashed'], recent=sm['recent_flash'], update_total_count=sm['updated'],
                     update_recent=sm['recent_update'], db_version=state.data['db_version'] + 1)


# ---------------------------------------------------------------- boards

def esp_ports():
    return [p for p in serial.tools.list_ports.comports() if p.vid == ESP_VID and p.pid == ESP_PID]


def board_key(p):
    # The USB Serial/JTAG reports the chip's MAC as its USB serial number, so the
    # same board is recognised across resets even if the port name changes
    return p.serial_number or p.device


def find_board(key):
    for p in esp_ports():
        if board_key(p) == key:
            return p
    return None


class PortGone(RuntimeError):
    """esptool couldn't open the port (it dropped out); worth another try"""


def wait_settled(state, key):
    """Wait until the board's port has been there, unchanged, for SETTLE_S.
    Returns the port, or None if it was unplugged."""
    deadline = time.time() + SETTLE_GIVE_UP_S
    since, seen, gone_since = None, None, None
    hinted = False
    while True:
        p = find_board(key)
        now = time.time()
        if p is None:
            since, seen = None, None
            gone_since = gone_since or now
            if now - gone_since >= GONE_S:
                return None   # away for good: unplugged
        elif p.device != seen:
            gone_since = None
            since, seen = now, p.device
        elif now - since >= SETTLE_S:
            return p.device
        else:
            gone_since = None
        if now > deadline and not hinted:
            hinted = True
            state.log('the port keeps dropping out (a blank chip resetting?)')
            state.update(message='The USB port keeps dropping out: unplug the board and plug it in '
                                 'again while holding BOOT')
        time.sleep(0.2)


def flash(state, port, parts, erase_all=True):
    """Write `parts` ([(address, file)]), erasing the whole chip first if
    `erase_all` (else only the sectors written). Returns the MAC esptool read
    (lower case, colon separated)."""
    cmd = [sys.executable, '-m', 'esptool', '--chip', 'esp32c3', '--port', port, '--baud', str(FLASH_BAUD),
           '--before', 'default-reset', '--after', 'hard-reset', 'write-flash']
    if erase_all:
        cmd.append('--erase-all')
    for address, file in parts:
        cmd += [f'0x{address:x}', str(file)]
    part, nparts = 0, len(parts)
    state.log('$ ' + ' '.join(cmd[1:]))
    mac = None
    port_gone = False
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
                            encoding='utf-8', errors='replace')
    buf = ''
    last_pct = -1
    # esptool redraws its progress bar with \r: split on both
    while True:
        ch = proc.stdout.read(1)
        if ch == '':
            break
        if ch not in '\r\n':
            buf += ch
            continue
        line, buf = buf.strip(), ''
        if not line:
            continue
        if 'Could not open' in line and 'port is busy or doesn' in line:
            port_gone = True
        m = re.search(r'MAC:\s*([0-9a-fA-F:]{17})', line)
        if m:
            mac = m.group(1).lower()
        if erase_all and ('Erasing flash' in line or 'Chip erase' in line):
            state.update(message='Erasing the whole chip')
        if re.search(r'Flash will be erased from', line):
            part = min(nparts, part + 1)   # the next file
        m = re.search(r'Writing at 0x[0-9a-f]+.*?(\d+(?:\.\d+)?)\s*%', line)
        if m:
            pct = int(float(m.group(1)))
            if pct != last_pct:
                last_pct = pct
                which = f' (part {max(part, 1)} of {nparts})' if nparts > 1 else ''
                state.update(message=f'Writing firmware{which} {pct}%', progress=pct)
            continue
        state.log(line)
    if proc.wait() != 0:
        if port_gone:
            raise PortGone(f'could not open {port}')
        raise RuntimeError(f'esptool failed (exit code {proc.returncode}); see the log')
    if mac is None:
        raise RuntimeError('esptool did not report the chip\'s MAC')
    return mac


ID_PATTERNS = {
    'serial_mac': re.compile(r'serial\s*:\s*([0-9A-F]{12})'),
    'bt_mac': re.compile(r'bluetooth\s*:\s*([0-9A-F]{2}(?::[0-9A-F]{2}){5})'),
    'unique_id': re.compile(r'unique id\s*:\s*([0-9A-F]{32}|not programmed)'),
    'chip': re.compile(r'chip\s*:\s*(ESP32-C3 revision v[\d.]+)'),
}
RE_FIRMWARE = re.compile(r'firmware\s*:\s*\S+ (v[^\s,]+)')   # in `status`
# Also in `status`: the battery percentage curve, and per launcher the load
# calibration and RPM threshold. The saved empty level is taken, not the one
# re-learned since boot ("(tracking N mV)"; firmware up to 2.4.0 prints only
# the re-learned one, as "load empty N mV")
RE_BATT_CURVE = re.compile(r'^batt curve\s*:\s*(.+?)\s*$', re.M)
RE_VBAT_CAL = re.compile(r'^vbat cal\s*:\s*(?:gain (\d+\.\d+), offset (-?\d+) mV, (\d+) points over [-\d]+ mV, '
                         r'max fit error (\d+) mV|(none))', re.M)
RE_LAUNCHER_CAL = re.compile(r'^cal (black|white)\s*:\s*(?:load empty (-?\d+) mV(?: \(tracking -?\d+ mV\))?, '
                             r'loaded (-?\d+) mV|load not calibrated); rpm threshold (\d+) mV', re.M)


def parse_board_state(text):
    """The boarddb.BOARD_COLUMNS that `status` shows (an older firmware shows
    fewer: those left out stay as they are in the database)"""
    st = {}
    m = RE_VBAT_CAL.search(text)
    if m:
        st['vbat'] = None if m.group(5) else {'gain': m.group(1), 'offset_mv': int(m.group(2)),
                                              'points': int(m.group(3)), 'max_fit_error_mv': int(m.group(4))}
    m = RE_BATT_CURVE.search(text)
    if m:
        curve = re.sub(r"\s*\('cal batt [^)]*\)", '', m.group(1))   # without the hint
        st['batt_cal'] = 1 if curve.startswith('measured') else 0
        st['batt_curve'] = curve
    for launcher, empty, loaded, thr in RE_LAUNCHER_CAL.findall(text):
        st[f'{launcher}_load_empty_mv'] = int(empty) if empty else None
        st[f'{launcher}_load_loaded_mv'] = int(loaded) if loaded else None
        st[f'{launcher}_rpm_thr_mv'] = int(thr)
    return st


def _read_for(s, seconds, until=None):
    """Read for up to `seconds` (until `until` appears), answering the line
    editor's cursor query (ESC[6n) as a terminal would."""
    text = ''
    end = time.time() + seconds
    while time.time() < end:
        chunk = s.read(512)
        if b'\x1b[6n' in chunk:
            s.write(b'\x1b[1;200R')
        text += chunk.decode('utf-8', 'replace')
        if until and until(text):
            break
    return text


def _read_until_quiet(s, quiet_s=0.4, max_s=3.0):
    """Read (answering cursor queries) until nothing has come for `quiet_s`."""
    text = ''
    end = time.time() + max_s
    last = time.time()
    while time.time() < end and time.time() - last < quiet_s:
        chunk = s.read(512)
        if chunk:
            last = time.time()
            if b'\x1b[6n' in chunk:
                s.write(b'\x1b[1;200R')
            text += chunk.decode('utf-8', 'replace')
    return text


def read_ids(state, port):
    """Ask the firmware console for `id` and parse it. On each new connection
    the console first asks the terminal where its cursor is (ESC[6n) and takes
    whatever comes next as the answer, so nothing may be typed until that's
    over: wait, answer it, send Enter and wait for the prompt, then type `id`.
    Retries stay on one connection: closing and reopening the port left the
    console silent."""
    deadline = time.time() + ID_TIMEOUT_S
    text = ''
    attempt = 0
    s = None
    try:
        while time.time() < deadline:
            attempt += 1
            state.update(message=f'Asking the firmware for its IDs (try {attempt})')
            if s is None:
                try:
                    # DTR/RTS low: opening the port must not reset the chip. A write
                    # timeout so a console that isn't reading can't hang the tool.
                    s = vbatcal.exclusive(serial.Serial())
                    s.port, s.baudrate, s.timeout, s.write_timeout = port, 115200, 0.1, 1.0
                    s.dtr = s.rts = False
                    s.open()
                except serial.SerialException:
                    s = None
                    time.sleep(0.5)
                    continue
                _read_until_quiet(s, 1.0)                  # whatever the console sends on connecting
                # A connection closed right after a reply leaves the line editor waiting
                # for the answer to its cursor query, taking keystrokes for it: answer
                # it now. If nothing was waiting, this is just a line it rejects.
                s.write(b'\x1b[1;200R\r')
                _read_until_quiet(s)
            try:
                s.write(b'\r')
                text = _read_for(s, 1.5, lambda t: 'bxt>' in t)
                # Around each prompt the line editor asks for the cursor position and
                # the terminal width, taking what comes next as the answer: type only
                # once it has gone quiet
                text += _read_until_quiet(s)
                for ch in b'id\r':
                    s.write(bytes([ch]))
                    time.sleep(0.02)
                text = _read_for(s, 3.0, lambda t: ID_PATTERNS['chip'].search(t) is not None)
            except serial.SerialException as e:
                state.log(f'console: {e}')
                s.close()
                s = None
                time.sleep(1.0)
                continue
            found = {k: (p.search(text).group(1) if p.search(text) else None) for k, p in ID_PATTERNS.items()}
            if found['serial_mac'] and found['bt_mac'] and found['unique_id']:
                if found['unique_id'] == 'not programmed':
                    found['unique_id'] = ''
                # Let the next prompt's cursor queries be answered before closing
                _read_until_quiet(s)
                return found
            time.sleep(0.5)
    finally:
        if s is not None:
            s.close()
    last = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', text).strip().replace('\r', ' ').replace('\n', ' ')
    state.log(f'last console reply: {last[-200:] or "(nothing)"}')
    raise RuntimeError('the firmware did not answer `id` (did it start?)')


def worker(state, db_path, identify_only):
    """Flash or update each board plugged in, with the firmware chosen when
    it was plugged in (state.fw)"""
    session = {'flash': 0, 'update': 0}
    while True:
        # ---- wait for a board
        state.update(phase='waiting', message='Plug in a board', progress=0, port=None, error=None)
        board = None
        while board is None:
            # Taken under the lock, so the mode can't change to calibration meanwhile:
            # boards being calibrated are on USB too and must not be erased
            with state.lock:
                job = state.data['mode']
                fw = state.fw
                image, version, sha, updates = fw['image'], fw['version'], fw['sha'], fw['updates']
                if state.data['auto_flash'] and (job == 'flash' or (job == 'update' and updates)):
                    ports = esp_ports()
                    board = ports[0] if ports else None
                    if board is not None:
                        state.update(phase='flashing', message='Waiting for the USB port to settle', progress=0,
                                     port=board.device, record=None, job=job)
            if board is None:
                time.sleep(POLL_S)
        key, port = board_key(board), board.device
        state.log(f'board on {port} (USB serial {board.serial_number})')
        port = wait_settled(state, key)
        if port is None:
            state.log('board removed before it settled')
            continue

        try:
            if identify_only:
                mac = None
            else:
                for attempt in range(1, OPEN_RETRIES + 1):
                    state.update(phase='flashing', message='Connecting', progress=0, port=port)
                    try:
                        if job == 'update':
                            mac = flash(state, port, updates, erase_all=False)
                        else:
                            mac = flash(state, port, [(0x0, image)])
                        break
                    except PortGone:
                        if attempt == OPEN_RETRIES:
                            raise RuntimeError(f'could not open the port after {OPEN_RETRIES} tries; unplug the '
                                               'board and plug it in again while holding BOOT') from None
                        state.log(f'port dropped out, trying again ({attempt}/{OPEN_RETRIES})')
                        port = wait_settled(state, key)
                        if port is None:
                            raise RuntimeError('the board was unplugged') from None
                state.log(f'{"updated" if job == "update" else "flashed"}, MAC {mac}')

            # ---- the board restarts into the new firmware: find it again and ask for its IDs
            state.update(phase='identifying', message='Starting the firmware, reading the IDs', progress=100)
            end = time.time() + BOOT_WAIT_S
            found = None
            while found is None and time.time() < end:
                time.sleep(1.0)
                found = find_board(key)
            if found is None:
                raise RuntimeError('the board did not come back on USB after '
                                   + ('the update' if job == 'update' else 'flashing'))
            port = found.device
            ids = read_ids(state, port)
            if mac and ids['serial_mac'] != mac.replace(':', '').upper():
                raise RuntimeError(f'MAC mismatch: esptool read {mac}, the firmware reports {ids["serial_mac"]}')

            row = dict(ids, firmware='not flashed (identify only)' if identify_only else version, note='')
            if identify_only:
                boarddb.save_ids(db_path, ids)
            elif job == 'update':
                boarddb.save_update(db_path, ids, version, sha, port)
                # What it says now about its calibrations (a VBAT calibration made
                # elsewhere shows as CAL)
                try:
                    board_state = read_board_state(port)
                    boarddb.save_board_state(db_path, ids['unique_id'], board_state)
                    state.log(describe_board_state(board_state))
                except Exception as e:   # noqa: BLE001 - only extra information: the update is done
                    state.log(f'could not read its calibrations from `status` ({e})')
                rec = boarddb.get(db_path, ids['unique_id'])
                row['note'] = 'settings and VBAT calibration kept' + (
                    f' (calibration {rec["cal_result"]}, {rec["cal_at"].replace("T", " ")})' if rec['cal_at']
                    else f' (calibrated elsewhere, gain {rec["cal_gain"]:.6f})' if rec['cal_result'] == 'CAL'
                    else '')
                state.log(f'updated to {version}: {row["note"]}')
            else:
                count = boarddb.save_flash(db_path, ids, version, sha, port)
                if count > 1:
                    row['note'] = f'flashed again ({count} times in all): its VBAT calibration was erased'
                    state.log(f'this board was flashed before: {row["note"]}')
            session[job] += 1
            state.log(f'recorded {ids["serial_mac"]}, BT {ids["bt_mac"]}, unique id {ids["unique_id"]}')
            refresh_counts(state, db_path)
            state.update(phase='done', message='Then plug in the next one', record=row,
                         session_count=session['flash'], update_session_count=session['update'])
        except Exception as e:   # noqa: BLE001 - every failure goes to the operator
            state.log(f'FAILED: {e}')
            state.update(phase='failed', message='Unplug it; plugging it in again starts over', error=str(e))

        # ---- wait for it to be unplugged, so the same board isn't flashed twice
        while find_board(key) is not None:
            time.sleep(POLL_S)
        state.log('board removed')


# ---------------------------------------------------------------- VBAT calibration

class CalUI:
    """What vbatcal.run_calibration reports progress to"""

    def __init__(self, state, total):
        self.state, self.total, self.done = state, total, 0

    def step(self, text):
        self.done += 1
        self.state.log(f'[cal] {text}')
        self.state.update_cal(message=text, progress=min(99, 100 * self.done // self.total))

    def log(self, text):
        self.state.log(text)

    def extend(self, steps):
        self.total += steps   # a calibration try is repeated

    def result(self, r):
        self.state.update_cal(result=json.loads(json.dumps(r)))   # a copy: the page reads it in another thread


def usb_watcher(state):
    """Keep the page told which boards are on USB (once their port has settled:
    right after plugging in it may drop out once), for the Database tab"""
    seen = {}   # key -> (port, since)
    while True:
        now = time.time()
        current = {}
        for p in esp_ports():
            k = board_key(p)
            prev = seen.get(k)
            current[k] = prev if prev and prev[0] == p.device else (p.device, now)
        seen = current
        usb = [{'key': k, 'port': port} for k, (port, since) in seen.items() if now - since >= DETECT_SETTLE_S]
        if usb != state.data['usb']:
            with state.lock:
                for k in [k for k in state.identified if k not in seen]:
                    del state.identified[k]
                state.update(usb=usb)
        time.sleep(DETECT_POLL_S)


def identify_board(state, db_path, key):
    """Read the IDs of a board on USB from its console, for the Database tab.
    Returns (ids, known) or raises ValueError with the reason. Not while the
    board may be flashed, updated or calibrated."""
    with state.lock:
        if state.data['auto_flash']:
            raise ValueError('automatic ' + ('update' if state.data['mode'] == 'update' else 'flashing')
                             + ' is on: the board is being written, not read')
        if state.data['cal']['phase'] == 'running':
            raise ValueError('a calibration is running')
        if state.data['phase'] in ('flashing', 'identifying'):
            raise ValueError('a board is being flashed')
    # One board at a time (several plugged in at once wait their turn)
    if not state.identify_lock.acquire(timeout=60):
        raise ValueError('busy reading another board: try again')
    with state.lock:
        state.identifying = True
    try:
        # One try; the page tries again until it works or the board is unplugged
        # (right after plugging in the console may not be ready, or the port may
        # drop out once)
        p = find_board(key)
        if p is None:
            raise ValueError('unplugged')
        text = ''
        con = None
        try:
            con = vbatcal.Console(p.device)
            if not con.wait_prompt(3.0):
                raise ValueError('no prompt from its console (still starting, or no firmware on it)')
            text = con.cmd('id')
            # The version it runs ("firmware    : bx-tacho v2.4.0, ESP-IDF ..."), for adding it,
            # and its battery curve and launcher calibrations, for its row
            status = con.cmd('status') if ID_PATTERNS['serial_mac'].search(text) else ''
            version = RE_FIRMWARE.search(status)
        except serial.SerialException as e:
            raise ValueError(f'its port could not be opened ({e})') from None
        finally:
            if con:
                con.close()
        ids = {k: (pat.search(text).group(1) if pat.search(text) else None) for k, pat in ID_PATTERNS.items()}
        if not ids['serial_mac']:
            raise ValueError('it did not answer `id`')
        ids['firmware'] = version.group(1) if version else None
        if ids['unique_id'] in (None, 'not programmed'):
            ids['unique_id'] = ''
        board_state = parse_board_state(status)
        with state.lock:
            state.identified[key] = (ids, board_state)
        # A board in the database gets what it says now (written on adding it otherwise)
        known = bool(ids['unique_id']) and boarddb.save_board_state(db_path, ids['unique_id'], board_state)
        if known:
            refresh_counts(state, db_path)
        state.log(f'database: board {ids["serial_mac"]} on {p.device} '
                  + (f'is recorded; {describe_board_state(board_state)}' if known else 'is not in the database'))
        return ids, known
    finally:
        with state.lock:
            state.identifying = False
        state.identify_lock.release()


def read_board_state(port):
    """parse_board_state() of `status`, from a board running the firmware"""
    con = vbatcal.Console(port)
    try:
        if not con.wait_prompt(5.0):
            raise RuntimeError('no prompt from its console')
        return parse_board_state(con.cmd('status'))
    finally:
        con.close()


def describe_board_state(st):
    """For the log: 'batt curve measured, ...; black load 120/860 mV, rpm 1400 mV; ...'"""
    parts = [f'batt curve {st["batt_curve"]}'] if 'batt_curve' in st else []
    if 'vbat' in st:
        parts.insert(0, f'VBAT calibration gain {st["vbat"]["gain"]}' if st['vbat'] else 'no VBAT calibration')
    for l in ('black', 'white'):
        if f'{l}_rpm_thr_mv' in st:
            load = (f'{st[l + "_load_empty_mv"]}/{st[l + "_load_loaded_mv"]} mV'
                    if st[l + '_load_empty_mv'] is not None else 'not calibrated')
            parts.append(f'{l} load {load}, rpm threshold {st[l + "_rpm_thr_mv"]} mV')
    return '; '.join(parts) or 'no calibration shown by its firmware'


def ppk2_watcher(state):
    """Keep the page told whether a PPK2 is connected (not while it's in use)"""
    while True:
        with state.lock:
            busy = state.data['cal']['phase'] == 'running'
        if not busy:
            port = vbatcal.find_ppk2()
            if port != state.data['cal']['ppk2']:
                state.update_cal(ppk2=port)
                state.log(f'PPK2 on {port}' if port else 'PPK2 disconnected')
        time.sleep(1.0)


def cal_worker(state, db_path, ppk2_offset_mv, verify_per_segment):
    session = 0
    while True:
        state.cal_start.wait()
        state.cal_start.clear()
        ui = CalUI(state, vbatcal.steps_total(verify_per_segment))
        try:
            result, row = vbatcal.run_calibration(ui, sys.modules[__name__], state.data['cal']['ppk2'], db_path,
                                                  ppk2_offset_mv, verify_per_segment)
            refresh_counts(state, db_path)
            session += 1
            failed = [v for v in result['verify'] if not v['pass']]
            state.log(f'[cal] {row["serial_mac"]}: gain {row["gain"]}, offset {row["offset_mv"]} mV, '
                      f'max fit error {row["max_fit_error_mv"]} mV, verify {row["result"]}')
            state.update_cal(phase='failed' if failed else 'done', progress=100, row=row,
                             message='Remove the board (clip and USB cable)',
                             error=('verification failed at ' + ', '.join(f'{v["actual"]} mV ({v["err"]:+d} mV)'
                                                                         for v in failed)) if failed else None,
                             session_count=session)
        except Exception as e:   # noqa: BLE001 - every failure goes to the operator
            state.log(f'[cal] FAILED: {e}')
            state.update_cal(phase='failed', message='Remove the board (clip and USB cable)', error=str(e))


def set_firmware(state, load):
    """Change the firmware to load() (it downloads or checks the image: it may
    take a while). Returns None, or why it can't change now."""
    def busy():
        if state.data['auto_flash']:
            return 'turn automatic flashing or update off first'
        if state.data['phase'] in ('flashing', 'identifying'):
            return 'wait for the board being written'
        return None
    with state.lock:
        why = busy()
    if why:
        return why
    if not state.fw_lock.acquire(blocking=False):
        return 'another firmware is being loaded'
    try:
        try:
            fw = load()
        except (RuntimeError, ValueError, OSError) as e:
            return str(e)
        with state.lock:
            why = busy()   # turned on meanwhile
            if why:
                return why
            state.fw = fw
            state.update(firmware=fw_public(fw), update_error=fw['update_error'])
        state.log(f'firmware: {fw["version"]} ({fw["label"]}), SHA-256 {fw["sha"]}'
                  + (f'; updating unavailable: {fw["update_error"]}' if fw['update_error'] else ''))
        return None
    finally:
        state.update(fw_busy=None)
        state.fw_lock.release()


def select_release(state, tag):
    releases, _ = list_releases()
    rel = next((r for r in releases if r['tag'] == tag), None)
    if rel is None:
        return f'no firmware release {tag}'

    def load():
        state.update(fw_busy=f'Downloading {tag}' if not rel['downloaded'] else f'Checking {tag}')
        image, _ = fetch_release(tag, rel['asset'])
        return load_firmware(image, tag, 'release', f'GitHub release {tag}')
    return set_firmware(state, load)


MAX_UPLOAD = 4 << 20


def select_local(state, filename, data):
    """A merged image chosen on the PC, kept in firmware/local/"""
    def load():
        state.update(fw_busy=f'Checking {filename}')
        check_merged(data)
        version = image_version(data)
        name = re.sub(r'[^\w.+-]', '_', Path(filename).name)[-80:] or 'image.bin'
        folder = HERE / 'firmware' / 'local'
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / f'{hashlib.sha256(data).hexdigest()[:12]}-{name}'
        path.write_bytes(data)
        return load_firmware(path, f'{version} (local)', 'local', f'local file {Path(filename).name}')
    return set_firmware(state, load)


MODE_NAMES = {'flash': 'flash', 'update': 'firmware update', 'cal': 'VBAT calibration'}


def set_mode(state, mode):
    """Returns None, or why the mode can't change now"""
    if mode not in MODE_NAMES:
        return 'unknown mode'
    with state.lock:
        if state.data['mode'] == mode:
            # Back to the Flash or Update tab from the Database tab: automatic off too
            if state.data['auto_flash']:
                state.update(auto_flash=False)
                state.log(f'automatic {"update" if mode == "update" else "flashing"} off')
            return None
        if state.data['phase'] != 'waiting':
            return 'finish the board being flashed first (unplug it)'
        if state.data['cal']['phase'] == 'running':
            return 'a calibration is running'
        state.data['mode'] = mode
        state.update(mode=mode, auto_flash=False)
    state.log(f'mode: {MODE_NAMES[mode]}')
    return None


def set_auto_flash(state, on):
    """Returns None, or why automatic flashing can't be turned on now"""
    with state.lock:
        if on and state.data['fw_busy']:
            return 'the firmware is being changed: try again in a moment'
        if on and state.identifying:
            return 'the Database tab is reading a board: try again in a moment'
        if on and state.data['mode'] not in ('flash', 'update'):
            return 'switch to the Flash or Update tab first'
        if on and state.data['mode'] == 'update' and state.data['update_error']:
            return f'updating is unavailable: {state.data["update_error"]}'
        if on and not state.data['auto_flash'] and esp_ports():
            return ('a board is on USB: unplug it first, so it isn\'t '
                    + ('updated' if state.data['mode'] == 'update' else 'erased')
                    + ' by accident, then turn it on and plug in the boards')
        changed = state.data['auto_flash'] != on
        state.update(auto_flash=on)
    if changed:
        state.log(f'automatic {"update" if state.data["mode"] == "update" else "flashing"} {"on" if on else "off"}')
    return None


def start_cal(state):
    """Returns None, or why a calibration can't start now"""
    with state.lock:
        cal = state.data['cal']
        if state.data['mode'] != 'cal':
            return 'switch to the VBAT calibration tab first'
        if cal['phase'] == 'running':
            return 'a calibration is running'
        if not cal['ppk2']:
            return 'no PPK2 found: connect it over USB'
        if esp_ports():
            return ('a board is already powered on USB (a battery, or a USB cable with power?): the PPK2 must be '
                    'its only supply')
        state.update_cal(phase='running', progress=0, message='Starting', result=None, row=None, error=None)
    state.cal_start.set()
    return None


# ---------------------------------------------------------------- web page

def make_handler(state, db_path):
    page = (HERE / 'index.html').read_bytes()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, body, content_type, extra=()):
            self.send_response(200)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            for k, v in extra:
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = urllib.parse.urlsplit(self.path).path
            if path in ('/', '/index.html'):
                self.reply(page, 'text/html; charset=utf-8')
            elif path == '/firmware/releases':
                releases, error = list_releases()
                body = json.dumps({'releases': releases, 'error': error})
                self.reply(body.encode(), 'application/json', [('Cache-Control', 'no-cache')])
            elif path == '/db':
                # The Database tab: every board
                body = json.dumps({'db': str(db_path), 'columns': boarddb.COLUMNS, 'rows': boarddb.rows(db_path)})
                self.reply(body.encode(), 'application/json', [('Cache-Control', 'no-cache')])
            elif path == '/db.csv':
                out = io.StringIO()
                w = csv.DictWriter(out, fieldnames=boarddb.COLUMNS)
                w.writeheader()
                w.writerows(boarddb.rows(db_path))
                stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
                self.reply(out.getvalue().encode('utf-8-sig'), 'text/csv; charset=utf-8',
                           [('Content-Disposition', f'attachment; filename="bx-tacho-boards-{stamp}.csv"')])
            elif path == '/events':
                # Server-sent events: the whole state on every change
                self.send_response(200)
                self.send_header('Content-Type', 'text/event-stream')
                self.send_header('Cache-Control', 'no-cache')
                self.end_headers()
                seq = -1
                try:
                    while True:
                        seq, data = state.snapshot(seq)
                        self.wfile.write(f'data: {data}\n\n'.encode())
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass
            else:
                self.send_error(404)

        def do_POST(self):
            if urllib.parse.urlsplit(self.path).path == '/firmware/upload':
                # A local merged image: the file itself is the body
                q = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
                length = int(self.headers.get('Content-Length') or 0)
                if length > MAX_UPLOAD:
                    self.rfile.read(length)
                    why = f'too big for a merged image ({length} bytes)'
                else:
                    why = select_local(state, (q.get('name') or ['image.bin'])[0], self.rfile.read(length))
                return self.answer(why, {})
            try:
                body = json.loads(self.rfile.read(int(self.headers.get('Content-Length') or 0)) or b'{}')
            except ValueError:
                body = {}
            extra = {}
            if self.path == '/mode':
                why = set_mode(state, body.get('mode'))
            elif self.path == '/firmware/select':
                why = select_release(state, str(body.get('tag')))
            elif self.path == '/flash/auto':
                why = set_auto_flash(state, bool(body.get('on')))
            elif self.path == '/cal/start':
                why = start_cal(state)
            elif self.path == '/db/board':
                # The customer columns, edited on the Database tab
                try:
                    boarddb.save_customer(db_path, body.get('unique_id'), body)
                    refresh_counts(state, db_path)
                    why = None
                except ValueError as e:
                    why = str(e)
            elif self.path == '/db/delete':
                # A board's row, deleted on the Database tab (after the page asked to confirm)
                try:
                    row = boarddb.delete(db_path, body.get('unique_id'))
                    refresh_counts(state, db_path)
                    # Everything it held goes to the log, as the only trace left
                    state.log(f'database: deleted board {row["serial_mac"]}: '
                              + json.dumps({k: v for k, v in row.items() if v not in (None, '') and k != 'cal_detail'}))
                    why = None
                except ValueError as e:
                    why = str(e)
            elif self.path == '/db/identify':
                # A board plugged in while the Database tab is open: which is it?
                try:
                    ids, known = identify_board(state, db_path, body.get('key'))
                    extra = {'ids': ids, 'known': known}
                    why = None
                except ValueError as e:
                    why = str(e)
            elif self.path == '/db/add':
                # ... and it isn't in the database: add it (the IDs read just now)
                with state.lock:
                    ids, board_state = state.identified.get(body.get('key')) or (None, None)
                try:
                    if ids is None:
                        raise ValueError('read the board again (it was unplugged?)')
                    boarddb.save_ids(db_path, ids)
                    boarddb.save_board_state(db_path, ids['unique_id'], board_state)
                    refresh_counts(state, db_path)
                    state.log(f'database: board {ids["serial_mac"]} added')
                    extra = {'unique_id': ids['unique_id']}
                    why = None
                except (ValueError, boarddb.NoUniqueId) as e:
                    why = str(e)
            else:
                self.send_error(404)
                return
            self.answer(why, extra)

        def answer(self, why, extra):
            reply = json.dumps({'ok': why is None, 'error': why, **extra}).encode()
            self.send_response(200 if why is None else 409)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(reply)))
            self.end_headers()
            self.wfile.write(reply)

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--release', help='GitHub release to flash, e.g. v2.4.0 (default: the latest)')
    ap.add_argument('--firmware', help='a local merged image instead of a release')
    ap.add_argument('--db', default='production.db',
                    help='production database (default production.db in the current folder)')
    ap.add_argument('--port', type=int, default=8765, help='web page port (default 8765)')
    ap.add_argument('--no-browser', action='store_true', help="don't open the page")
    ap.add_argument('--identify-only', action='store_true',
                    help='read and record the IDs without erasing or flashing (for testing)')
    ap.add_argument('--ppk2-offset-mv', type=int, default=0,
                    help='voltage at the board minus the PPK2 setting, if a multimeter shows one (default 0)')
    ap.add_argument('--verify-per-segment', type=int, default=vbatcal.VERIFY_PER_SEGMENT,
                    help=f'random check voltages between each two calibration points '
                         f'(default {vbatcal.VERIFY_PER_SEGMENT})')
    args = ap.parse_args()

    if args.firmware:
        image = Path(args.firmware).resolve()
        if not image.exists():
            sys.exit(f'{image}: not found')
        version = image_version(image.read_bytes())
        fw = load_firmware(image, f'{version} (local)' if version else image.name, 'file', f'file {image.name}')
    else:
        try:
            tag = args.release or latest_release()
            image, version = fetch_release(tag)
        except RuntimeError as e:
            sys.exit(str(e))
        fw = load_firmware(image, version, 'release', f'GitHub release {tag}')
    db_path = Path(args.db).resolve()
    print(f'firmware {fw["version"]}: {image} (SHA-256 {fw["sha"]})')
    if fw['update_error']:
        print(f'firmware updates unavailable: {fw["update_error"]}')
    boarddb.init(db_path)
    print(f'records: {db_path}')

    state = State(fw, db_path)
    refresh_counts(state, db_path)
    threading.Thread(target=worker, args=(state, db_path, args.identify_only), daemon=True).start()
    threading.Thread(target=cal_worker, args=(state, db_path, args.ppk2_offset_mv, args.verify_per_segment),
                     daemon=True).start()
    threading.Thread(target=ppk2_watcher, args=(state,), daemon=True).start()
    threading.Thread(target=usb_watcher, args=(state,), daemon=True).start()

    server = ThreadingHTTPServer(('127.0.0.1', args.port), make_handler(state, db_path))
    url = f'http://127.0.0.1:{args.port}/'
    print(f'open {url}  (Ctrl+C to quit)')
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
