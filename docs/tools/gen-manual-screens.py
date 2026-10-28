# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Darren Chi
# Draws the OLED screens shown in the user manual (docs/manual-zh-TW.html) as
# SVG files in docs/manual-screens/, with the firmware's own font and icons
# (firmware/main/font5x7.c, icons.c) at the coordinates main.c uses.
# Run from the repository root after the firmware's screens change:
#   python docs/tools/gen-manual-screens.py
import os
import re

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
MAIN = os.path.join(ROOT, 'firmware', 'main')
OUT = os.path.join(ROOT, 'docs', 'manual-screens')
W, H = 128, 64


def c_arrays(path):
    """name -> list of byte values for every `const uint8_t name[...] = {...}`"""
    src = open(path, encoding='utf-8').read()
    out = {}
    for m in re.finditer(r'const uint8_t (\w+)\[[^\]]*\]\s*=\s*\{(.*?)\};', src, re.S):
        body = re.sub(r'//[^\n]*', '', m.group(2))
        out[m.group(1)] = [int(v, 0) for v in re.findall(r'0x[0-9A-Fa-f]+|\b\d+\b', body)]
    return out


def font():
    src = open(os.path.join(MAIN, 'font5x7.c'), encoding='utf-8').read()
    rows = re.findall(r'\{\s*(0x[0-9A-Fa-f]{2}(?:\s*,\s*0x[0-9A-Fa-f]{2}){4})\s*\}', src)
    return [[int(v, 16) for v in r.split(',')] for r in rows]   # from ' ' (0x20)


ICONS = c_arrays(os.path.join(MAIN, 'icons.c'))
FONT = font()


class Screen:
    def __init__(self):
        self.px = set()

    def set(self, x, y):
        if 0 <= x < W and 0 <= y < H:
            self.px.add((x, y))

    def hline(self, x0, x1, y):
        for x in range(x0, x1 + 1):
            self.set(x, y)

    def bitmap(self, x, y, w, h, name):
        bits = ICONS[name]
        stride = (w + 7) // 8
        for row in range(h):
            for col in range(w):
                if bits[row * stride + col // 8] & (0x80 >> (col % 8)):
                    self.set(x + col, y + row)

    @staticmethod
    def width(s, scale=1):
        return (len(s) * 6 - 1) * scale if s else 0

    def text(self, x, y, s, scale=1):
        for ch in s:
            c = ord(ch) - 0x20
            g = FONT[c] if 0 <= c < len(FONT) else FONT[ord('?') - 0x20]
            for col in range(5):
                for row in range(8):
                    if g[col] & (1 << row):
                        for dx in range(scale):
                            for dy in range(scale):
                                self.set(x + col * scale + dx, y + row * scale + dy)
            x += 6 * scale

    def center(self, y, s, scale=1):
        self.text((W - self.width(s, scale)) // 2, y, s, scale)

    def svg(self, dim=False):
        color = '#5f7a99' if dim else '#cfe8ff'
        path = ''.join(f'M{x} {y}h1v1h-1z' for x, y in sorted(self.px))
        return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="-4 -4 {W + 8} {H + 8}" '
                f'shape-rendering="crispEdges">'
                f'<rect x="-4" y="-4" width="{W + 8}" height="{H + 8}" rx="3" fill="#000"/>'
                f'<path fill="{color}" d="{path}"/></svg>\n')


# ---- the screens, as main.c draws them

def status_line(s, left, batt='pct', pct=85, low=False, ble='on', volts='4.1V'):
    """draw_status_line(): state left, battery right, BLE icon in the middle"""
    s.text(0, 1, left)
    icon_x = W - 16
    if batt == 'charging':
        s.bitmap(icon_x, 0, 16, 9, 'icon_battery_charging_16x9')
        right = volts
    elif batt == 'full':
        s.bitmap(icon_x, 0, 16, 9, 'icon_battery_full_16x9')
        right = 'FULL'
    else:
        s.bitmap(icon_x, 0, 16, 9, 'icon_battery_16x9')
        fill = (pct * 10 + 99) // 100
        for y in range(5):
            if fill:
                s.hline(icon_x + 2, icon_x + 2 + fill - 1, 2 + y)
        right = f'{pct}%'
    right_x = icon_x - 2 - s.width(right)
    s.text(right_x, 1, right)
    if low:
        s.text(right_x - 3 - s.width('LOW'), 1, 'LOW')
    if ble == 'connected':
        s.bitmap((W - 9) // 2, 0, 9, 9, 'icon_ble_connected_9x9')
    elif ble == 'off':
        s.bitmap((W - 9) // 2, 0, 9, 9, 'icon_ble_off_9x9')
    else:
        s.bitmap((W - 5) // 2, 0, 5, 9, 'icon_ble_on_5x9')
    s.hline(0, W - 1, 10)


def main_screen(left, big, label, note='', **kw):
    """draw_screen() in run mode"""
    s = Screen()
    status_line(s, left, **kw)
    s.text((W - s.width(big, 4)) // 2, 18, big, 4)
    s.text(0, 56, label)
    s.text(W - s.width(note), 56, note)
    return s


ADDR = '7C:E8:B1:9D:4E:BE'


def splash():
    s = Screen()
    s.bitmap(6, (H - 40) // 2, 40, 40, 'icon_beyblade_40')
    s.text(54, 12, 'BX', 2)
    s.text(54, 28, 'TACHO', 2)
    s.text(54, 46, 'v2.3.0')
    return s


def credit():
    s = Screen()
    s.center(8, 'MADE BY')
    s.center(22, 'DAZCHI', 2)
    s.center(46, 'github.com/dazchi')
    return s


def low_battery():
    s = Screen()
    s.bitmap((W - 48) // 2, 0, 48, 48, 'icon_battery_off_48')
    s.center(47, 'LOW BATTERY')
    s.center(56, '3.28 V')
    return s


def countdown():
    s = Screen()
    s.center(0, 'DEEP SLEEP IN')
    s.hline(0, W - 1, 10)
    s.center(15, '3', 4)
    s.center(46, 'LOW BATTERY')
    s.center(56, 'PLUG IN USB TO WAKE')
    return s


def ble_gesture(on):
    s = Screen()
    w = 5 if on else 9
    s.bitmap((W - w) // 2, 6, w, 9, 'icon_ble_on_5x9' if on else 'icon_ble_off_9x9')
    s.center(22, 'BLE ON' if on else 'BLE OFF', 2)
    s.center(50, 'RESTARTING')
    return s


def update(percent):
    s = Screen()
    s.center(0, 'FIRMWARE UPDATE')
    s.hline(0, W - 1, 10)
    s.center(16, f'{percent}%', 3)
    s.center(40, 'DO NOT TURN OFF')
    fill = percent * W // 100
    for y in range(50, 56):
        s.hline(0, fill - 1, y)
    s.hline(0, W - 1, 57)
    return s


def update_done():
    s = Screen()
    s.center(0, 'FIRMWARE UPDATE')
    s.hline(0, W - 1, 10)
    s.center(20, 'DONE', 3)
    s.center(54, 'RESTARTING')
    return s


def console_code(code):
    """draw_console_code(): the code to type into the web app"""
    s = Screen()
    s.center(0, 'BLUETOOTH CONSOLE')
    s.hline(0, W - 1, 10)
    s.center(22, code, 3)
    s.center(56, 'ENTER THIS CODE')
    return s


SCREENS = {
    'splash': splash(),
    'credit': credit(),
    'ready': main_screen('READY', '----', 'RPM', ADDR),
    'loaded': main_screen('LOADED', '----', 'RPM', ADDR),
    'launch': main_screen('LAUNCH!', '9264', 'RPM'),
    'result': main_screen('RESULT', '12816', 'PEAK RPM'),
    'loaded-again': main_screen('LOADED', '12816', 'PEAK RPM'),
    'battery': main_screen('RESULT', '12816', 'PEAK RPM', pct=85),
    'low': main_screen('RESULT', '12816', 'PEAK RPM', pct=15, low=True),
    'charging': main_screen('RESULT', '12816', 'PEAK RPM', batt='charging'),
    'full': main_screen('RESULT', '12816', 'PEAK RPM', batt='full'),
    'ble-on': main_screen('READY', '----', 'RPM', ADDR, ble='on'),
    'ble-connected': main_screen('READY', '----', 'RPM', ble='connected'),
    'ble-off': main_screen('READY', '----', 'RPM', ble='off'),
    'low-battery': low_battery(),
    'countdown': countdown(),
    'ble-gesture-on': ble_gesture(True),
    'ble-gesture-off': ble_gesture(False),
    'update': update(47),
    'update-done': update_done(),
    'console-code': console_code('482 917'),
}

os.makedirs(OUT, exist_ok=True)
for name, screen in SCREENS.items():
    with open(os.path.join(OUT, f'{name}.svg'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(screen.svg())
# The battery icons alone, to show inside the manual's text
for name, icon, w in (('icon-charging', 'icon_battery_charging_16x9', 16), ('icon-full', 'icon_battery_full_16x9', 16),
                      ('icon-ble-connected', 'icon_ble_connected_9x9', 9)):
    bits = Screen()
    bits.bitmap(0, 0, w, 9, icon)
    path = ''.join(f'M{x} {y}h1v1h-1z' for x, y in sorted(bits.px))
    with open(os.path.join(OUT, f'{name}.svg'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="-2 -2 {w + 4} 13" shape-rendering="crispEdges">'
                f'<rect x="-2" y="-2" width="{w + 4}" height="13" rx="1.5" fill="#000"/>'
                f'<path fill="#cfe8ff" d="{path}"/></svg>\n')
with open(os.path.join(OUT, 'dim.svg'), 'w', encoding='utf-8', newline='\n') as f:
    f.write(SCREENS['result'].svg(dim=True))
print(f'{len(SCREENS) + 1} screens written to {os.path.normpath(OUT)}')
