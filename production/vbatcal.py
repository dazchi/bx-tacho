# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Darren Chi
"""Automatic VBAT calibration with a Nordic Power Profiler Kit II as the supply.

The board sits on the PPK2's output (VBAT+/VBAT- clip) and on USB through a
data-only cable (no VBUS, so the charger stays out of it). The PPK2 powers it
up, Bluetooth goes off for a steady current, the firmware's `cal vbat` takes a
point at each voltage, the fit is checked at random voltages in between, and
Bluetooth goes back on.
"""

import datetime
import os
import random
import re
import time

import serial
import serial.tools.list_ports

import boarddb

# ---------------------------------------------------------------- PPK2

# Command bytes, as used by Nordic's tools (and the ppk2-api Python library)
PPK2_REGULATOR_SET = 0x0D
PPK2_DEVICE_RUNNING_SET = 0x0C
PPK2_SET_POWER_MODE = 0x11
PPK2_SOURCE_MODE = 0x02
PPK2_GET_META_DATA = 0x19
PPK2_VID = 0x1915
PPK2_MIN_MV, PPK2_MAX_MV = 800, 5000


def exclusive(s):
    """Open serial ports for this process only, as Windows always does: on
    macOS and Linux another program (or a second thread here) could otherwise
    open the same port at the same time. Call before s.open()."""
    if os.name == 'posix':
        s.exclusive = True
    return s


PPK2_PROBE_RETRY_S = 10   # ports that didn't answer are asked again after this
_ppk2_probed = {}         # (port, ...) -> (command port or None, when)


def _answers_metadata(port):
    """Whether this port answers the PPK2's metadata request (the command
    interface does; it ends with "END")"""
    try:
        with exclusive(serial.Serial()) as s:
            s.port, s.baudrate, s.timeout, s.write_timeout = port, 9600, 0.2, 1
            s.open()
            s.reset_input_buffer()
            s.write(bytes([PPK2_GET_META_DATA]))
            end, data = time.time() + 1.5, b''
            while time.time() < end and b'END' not in data:
                data += s.read(256)
            return b'END' in data
    except (serial.SerialException, OSError):
        return False


def find_ppk2():
    """The PPK2's command port, or None. It shows up as "nRF Connect USB CDC
    ACM" (product "PPK2"). On Windows the command interface's location ends in
    1; elsewhere (macOS reports the same location for both of its ports) the
    port that answers a metadata request, asked once per set of ports."""
    candidates = [p for p in serial.tools.list_ports.comports()
                  if p.vid == PPK2_VID and ((p.product or '') == 'PPK2'
                                            or (p.description or '').startswith('nRF Connect USB CDC ACM'))]
    if len(candidates) == 1:
        return candidates[0].device
    if os.name == 'nt':
        for p in candidates:
            if (p.location or '').endswith('1'):
                return p.device
        return None
    if not candidates:
        return None
    key = tuple(sorted(p.device for p in candidates))
    found, when = _ppk2_probed.get(key, (None, 0.0))
    if found is None and time.time() - when >= PPK2_PROBE_RETRY_S:
        found = next((dev for dev in key if _answers_metadata(dev)), None)
        _ppk2_probed[key] = (found, time.time())
    return found


class PPK2:
    """Just enough of the PPK2 to use it as an adjustable supply"""

    def __init__(self, port):
        self.ser = exclusive(serial.Serial())
        self.ser.port, self.ser.baudrate, self.ser.timeout, self.ser.write_timeout = port, 9600, 1, 1
        self.ser.open()
        # Ask for the metadata first, as Nordic's tools do; the answer isn't needed
        self.ser.write(bytes([PPK2_GET_META_DATA]))
        end = time.time() + 2
        data = b''
        while time.time() < end and b'END' not in data:
            data += self.ser.read(256)
        self.ser.write(bytes([PPK2_SET_POWER_MODE, PPK2_SOURCE_MODE]))
        self.power(False)

    def set_mv(self, mv):
        mv = max(PPK2_MIN_MV, min(PPK2_MAX_MV, int(mv)))
        # 800 mV is [3, 32], then linear: 256 mV per step of the first byte
        diff = mv - PPK2_MIN_MV + 32
        self.ser.write(bytes([PPK2_REGULATOR_SET, 3 + diff // 256, diff % 256]))

    def power(self, on):
        self.ser.write(bytes([PPK2_DEVICE_RUNNING_SET, 1 if on else 0]))

    def close(self):
        try:
            self.power(False)
        finally:
            self.ser.close()


# ---------------------------------------------------------------- firmware console

ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')
CONTROL = re.compile(r'[\x00-\x09\x0b-\x1f\x7f]')   # all but \n (left over from the line editor)


class Console:
    """One connection to the firmware console, kept open while the firmware
    runs (closing and reopening the port can leave the console silent). The
    line editor asks the terminal for the cursor position and width around each
    prompt and takes what comes next as the answer, so this answers those and
    only types once the console has gone quiet."""

    def __init__(self, port):
        self.s = exclusive(serial.Serial())
        self.s.port, self.s.baudrate, self.s.timeout, self.s.write_timeout = port, 115200, 0.1, 1.0
        self.s.dtr = self.s.rts = False   # opening the port must not reset the chip
        self.s.open()
        self.quiet(0.3)
        # Answer a cursor query an earlier connection may have left waiting
        self.s.write(b'\x1b[1;200R\r')
        self.quiet(0.3)

    def _read(self):
        chunk = self.s.read(512)
        if b'\x1b[6n' in chunk:
            self.s.write(b'\x1b[1;200R')
        return chunk.decode('utf-8', 'replace')

    def quiet(self, quiet_s=0.4, max_s=3.0):
        text, end, last = '', time.time() + max_s, time.time()
        while time.time() < end and time.time() - last < quiet_s:
            chunk = self._read()
            if chunk:
                last = time.time()
                text += chunk
        return text

    def wait_prompt(self, timeout=20.0):
        """Press Enter until the prompt answers (the firmware may still be starting)"""
        end = time.time() + timeout
        while time.time() < end:
            self.s.write(b'\r')
            text, until = '', time.time() + 0.5
            while time.time() < until and 'bxt>' not in text:
                text += self._read()
            if 'bxt>' in text:
                self.quiet()
                return True
        return False

    def cmd(self, line, timeout=10.0, done=None):
        """Type `line` and return its output (escape codes and the echo removed).
        Finishes at the next prompt, or when done(text) is true (for commands
        that restart the chip, after which the port may vanish). A line the
        console garbled ("Unrecognized command": nothing ran) is typed again."""
        for attempt in range(3):
            out = self._cmd(line, timeout, done)
            if 'Unrecognized command' not in out:
                return out
            time.sleep(0.3)
        return out

    def _cmd(self, line, timeout, done):
        # The line editor's cursor queries around a prompt can come late; typing
        # into one makes it take the keystrokes as the answer. So first an Enter
        # and its prompt, answered and quiet, then the line.
        self.quiet(0.3)
        self.s.write(b'\r')
        text, until = '', time.time() + 1.5
        while time.time() < until and 'bxt>' not in text:
            text += self._read()
        self.quiet()
        for ch in line.encode() + b'\r':
            self.s.write(bytes([ch]))
            time.sleep(0.02)
        text, end = '', time.time() + timeout
        while time.time() < end:
            text += self._read()
            plain = ANSI.sub('', text)
            nl = plain.find('\n')
            if done and done(plain):
                break
            if nl >= 0 and 'bxt>' in plain[nl:]:
                break
        try:
            self.quiet()
        except serial.SerialException:
            pass   # restarting
        plain = CONTROL.sub('', ANSI.sub('', text))
        nl = plain.find('\n')
        out = plain[nl + 1:] if nl >= 0 else plain
        return out.split('bxt>')[0].strip()

    def close(self):
        try:
            self.s.close()
        except serial.SerialException:
            pass


# ---------------------------------------------------------------- calibration

CAL_POINTS_MV = [3200, 3400, 3600, 3800, 4000, 4200]
START_MV = 3600
VERIFY_PER_SEGMENT = 1      # random verification voltages inside each gap between points
VERIFY_MARGIN_MV = 30       # keep them this far from the calibration points
SETTLE_S = 0.7              # after changing the PPK2 voltage (the firmware waits 300 ms more)
CAL_ATTEMPTS = 3            # calibrations tried when a check fails (the first failed check ends a try)
POWER_UP_S = 15             # for the board to appear on USB once the PPK2 is on
CONNECT_TIMEOUT_S = 20      # for its console to answer, after power-up or a restart
RESTART_DELAY_S = 0.4       # `ble on|off` restarts 200 ms after its reply
POLL_S = 0.1

RE_POINT = re.compile(r'point (\d+): VBAT (\d+) mV, pin (\d+) mV \(nominal reading (\d+) mV, ([+-]\d+) mV\)')
RE_SAVED = re.compile(r'Saved: gain ([\d.]+), offset (-?\d+) mV, (\d+) points over (\d+)-(\d+) mV, '
                      r'max fit error (\d+) mV')
RE_FIT_ROW = re.compile(r'^\s*(\d+)\s+(\d+)\s+([+-]\s*\d+) mV', re.M)
RE_USB = re.compile(r'usb\s*:\s*VBUS (yes|no), host \w+')
RE_VERIFY = re.compile(r'VBAT (-?\d+) mV measured vs (\d+) mV actual: ([+-]\d+) mV .*-> (PASS|FAIL)')


class CalFailed(RuntimeError):
    pass


def steps_total(verify_per_segment):
    """ui.step calls in a run without retries, for the progress bar"""
    return 9 + len(CAL_POINTS_MV) + (len(CAL_POINTS_MV) - 1) * verify_per_segment


def run_calibration(ui, bp, ppk_port, db_path, ppk2_offset_mv=0, verify_per_segment=VERIFY_PER_SEGMENT):
    """One board, start to finish. `ui` shows progress (ui.step, ui.log,
    ui.result); `bp` is the
    main module (board detection, ID parsing). Returns (result, row); raises
    CalFailed (or a serial error) when it couldn't finish."""
    ppk = PPK2(ppk_port)
    con = None
    key = None
    in_cal_mode = False
    result = {'points': [], 'fit': None, 'verify': [], 'ids': None}

    def actual(mv):
        return mv + ppk2_offset_mv

    def connect(what):
        """Poll until the board is on USB and its console answers: the port may
        drop out and come back (or not) while the firmware starts"""
        nonlocal con
        if con:
            con.close()
            con = None
        ui.step(what)
        end = time.time() + CONNECT_TIMEOUT_S
        while time.time() < end:
            p = bp.find_board(key)
            if p is not None:
                c = None
                try:
                    c = Console(p.device)
                    if c.wait_prompt(min(2.0, end - time.time())):
                        con = c
                        return
                except serial.SerialException:
                    pass   # gone again (resetting) or not ready to open yet
                if c:
                    c.close()
            time.sleep(POLL_S)
        raise CalFailed(f'the board did not answer within {CONNECT_TIMEOUT_S} s '
                        '(check the cable and the clip)')

    def restart_cmd(line):
        """`ble on|off`: saved, then the firmware restarts"""
        nonlocal con
        out = con.cmd(line, done=lambda t: 'Restarting' in t or 'already' in t)
        ui.log(out)
        if 'Restarting' in out:
            con.close()
            con = None
            time.sleep(RESTART_DELAY_S)   # don't catch the old console before it resets
            connect('Waiting for the restart')
        elif 'already' not in out:
            raise CalFailed(f'`{line}` failed: {out[-160:]!r}')

    def measure(line, pattern):
        """`cal vbat point|verify`, again if the reading was unstable"""
        for _ in range(3):
            out = con.cmd(line, timeout=8)
            m = pattern.search(out)
            if 'unstable' not in out:
                break
            ui.log(out)
            time.sleep(0.5)
        if m:
            return out, m
        raise CalFailed(f'`{line}`: {out[-200:]}')

    def restore():
        """Out of calibration mode and Bluetooth on, also after a failure"""
        if key is None:
            return   # the board never came up
        if con is None:
            connect('Reconnecting')
        ppk.set_mv(START_MV)
        time.sleep(SETTLE_S)
        if in_cal_mode:
            con.cmd('cal vbat stop')
        ui.step('Turning Bluetooth back on')
        restart_cmd('ble on')
        for _ in range(5):
            out = con.cmd('ble')
            if 'BLE: on' in out:
                ui.log(out)
                return
            time.sleep(1.0)
        raise CalFailed('Bluetooth did not come back on: run `ble on` on this board')

    error = None
    try:
        # ---- power up from the PPK2 and find the board on USB
        ui.step(f'Powering the board at {START_MV} mV')
        ppk.set_mv(START_MV)
        ppk.power(True)
        end = time.time() + POWER_UP_S
        board = None
        while board is None and time.time() < end:
            ports = bp.esp_ports()
            board = ports[0] if ports else None
            time.sleep(POLL_S)
        if board is None:
            raise CalFailed('no board appeared on USB: check the clip on VBAT+/VBAT-, the power switch '
                            'and the data cable')
        key = bp.board_key(board)
        connect('Waiting for the board to start')

        ids_text = con.cmd('id')
        ids = {k: (p.search(ids_text).group(1) if p.search(ids_text) else None)
               for k, p in bp.ID_PATTERNS.items()}
        if not ids['serial_mac']:
            raise CalFailed(f'the firmware did not answer `id`: {ids_text[-120:]!r}')
        if ids['unique_id'] in (None, 'not programmed'):
            raise CalFailed(f'{ids["serial_mac"]}: no eFuse unique ID on this chip, so it can\'t be recorded')
        result['ids'] = ids
        ui.result(result)
        ui.log(f'board {ids["serial_mac"]}, unique id {ids["unique_id"] or "none"}')

        # `cal vbat` refuses with VBUS present: check before the Bluetooth restarts
        status = con.cmd('status')
        usb = RE_USB.search(status)
        ui.log(usb.group(0) if usb else 'status: no usb line')
        if usb and usb.group(1) == 'yes':
            raise CalFailed('the board reads VBUS present (VUSB_DET high) although the PPK2 powers it: '
                            'measure VUSB at C2 / D1 anode (should be 0 V) and check the clip is on J1 '
                            'VBAT+/VBAT-, not the USB side')

        # ---- Bluetooth off, for a steady current (saved; the firmware restarts)
        ui.step('Turning Bluetooth off')
        restart_cmd('ble off')

        for attempt in range(1, CAL_ATTEMPTS + 1):
            result.update(points=[], fit=None, verify=[], attempt=attempt)
            # ---- calibration points (`cal vbat start` begins a new set)
            ui.step('Starting VBAT calibration mode' if attempt == 1 else
                    f'Calibrating again (try {attempt} of {CAL_ATTEMPTS})')
            out = con.cmd('cal vbat start')
            # The refusal: "USB power (VBUS) detected..." (the setup text printed on
            # success also mentions "USB power", so match the refusal itself)
            if 'VBUS) detected' in out:
                raise CalFailed('the board reads VBUS present (VUSB_DET high): measure VUSB at C2 / D1 anode')
            if 'VBAT calibration' not in out:
                raise CalFailed(f'`cal vbat start` failed: {out[-160:]!r}')
            in_cal_mode = True

            for mv in CAL_POINTS_MV:
                ui.step(f'Calibration point {mv} mV')
                ppk.set_mv(mv)
                time.sleep(SETTLE_S)
                out, m = measure(f'cal vbat point {actual(mv)}', RE_POINT)
                result['points'].append({'vbat': int(m.group(2)), 'pin': int(m.group(3)),
                                         'nominal_err': int(m.group(5)), 'fit_err': None})
                ui.result(result)
                ui.log(out)

            ui.step('Fitting and saving')
            out = con.cmd('cal vbat save', timeout=8)
            ui.log(out)
            m = RE_SAVED.search(out)
            if not m:
                # The firmware rejected the fit (a point off the line, gain or
                # offset out of range): nothing saved, take the points again
                why = re.search(r'Not saved: (.*)', out)
                why = why.group(1).rstrip('.') if why else out[-200:]
                if attempt == CAL_ATTEMPTS:
                    raise CalFailed(f'not saved after {CAL_ATTEMPTS} tries: {why}')
                ui.log(f'fit rejected on try {attempt} of {CAL_ATTEMPTS} ({why}): calibrating again')
                ui.extend(2 + len(CAL_POINTS_MV))   # the steps the new try repeats
                continue
            residuals = {int(r.group(1)): int(r.group(3).replace(' ', '')) for r in RE_FIT_ROW.finditer(out)}
            for p in result['points']:
                p['fit_err'] = residuals.get(p['vbat'])
            result['fit'] = {'gain': m.group(1), 'offset_mv': int(m.group(2)), 'points': int(m.group(3)),
                             'max_err_mv': int(m.group(6))}
            ui.result(result)

            # ---- check at random voltages between the points
            for lo, hi in zip(CAL_POINTS_MV, CAL_POINTS_MV[1:]):
                for _ in range(verify_per_segment):
                    mv = random.randint(lo + VERIFY_MARGIN_MV, hi - VERIFY_MARGIN_MV)
                    ui.step(f'Verifying at {mv} mV')
                    ppk.set_mv(mv)
                    time.sleep(SETTLE_S)
                    out, m = measure(f'cal vbat verify {actual(mv)}', RE_VERIFY)
                    result['verify'].append({'actual': int(m.group(2)), 'measured': int(m.group(1)),
                                             'err': int(m.group(3)), 'pass': m.group(4) == 'PASS'})
                    ui.result(result)
                    ui.log(out)
                    if not result['verify'][-1]['pass']:
                        break
                if result['verify'] and not result['verify'][-1]['pass']:
                    break

            if all(v['pass'] for v in result['verify']):
                break
            bad = result['verify'][-1]
            ui.log(f'verification failed at {bad["actual"]} mV ({bad["err"]:+d} mV) on try {attempt} '
                   f'of {CAL_ATTEMPTS}' + (': calibrating again' if attempt < CAL_ATTEMPTS else ''))
            if attempt < CAL_ATTEMPTS:
                ui.extend(2 + len(CAL_POINTS_MV) + len(result['verify']))   # the steps the new try repeats

        ui.step('Leaving calibration mode')
        ppk.set_mv(START_MV)
        time.sleep(SETTLE_S)
        con.cmd('cal vbat stop')
        in_cal_mode = False
    except Exception as e:   # noqa: BLE001 - reported once Bluetooth is back on
        error = e

    # Whatever happened: out of calibration mode, Bluetooth back on, power off
    try:
        restore()
    except Exception as e:   # noqa: BLE001
        ui.log(f'restoring the board failed: {e}')
        error = error or CalFailed(f'{e} (the calibration itself finished)')
    finally:
        if con:
            con.close()
        ppk.close()
    if error:
        raise error

    passed = len(result['verify']) > 0 and all(v['pass'] for v in result['verify'])
    row = {
        'timestamp': datetime.datetime.now().isoformat(timespec='seconds'),
        'serial_mac': result['ids']['serial_mac'],
        'unique_id': result['ids']['unique_id'],
        'gain': result['fit']['gain'],
        'offset_mv': result['fit']['offset_mv'],
        'points': result['fit']['points'],
        'max_fit_error_mv': result['fit']['max_err_mv'],
        'verify': '; '.join(f'{v["actual"]}:{v["err"]:+d}' for v in result['verify']),
        'result': 'PASS' if passed else 'FAIL',
        'ppk2_offset_mv': ppk2_offset_mv,
        'attempts': result['attempt'],
    }
    boarddb.save_cal(db_path, result['ids'], row, {'points': result['points'], 'verify': result['verify']},
                     row['timestamp'])
    return result, row
