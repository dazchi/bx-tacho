"""Regenerates web/js/font5x7.js from the firmware's OLED font.

Run from the repository root:  python web/tools/gen-font.py
"""
import re
from pathlib import Path

root = Path(__file__).resolve().parents[2]
src = (root / 'firmware/main/font5x7.c').read_text(encoding='utf-8')
body = src[src.index('s_font[][5] = {'):src.index('};')]
rows = re.findall(r'\{(0x[0-9A-Fa-f]{2}(?:,\s*0x[0-9A-Fa-f]{2}){4})\}', body)

out = [
    '// Generated from firmware/main/font5x7.c: the same 5x7 glyphs the OLED uses,',
    '// ASCII 0x20 ("space") to 0x5F ("_"). Each glyph is 5 columns, bit 0 at the top.',
    '// Regenerate if the firmware font changes (see web/README.md).',
    'export const FONT_FIRST = 0x20;',
    'export const FONT = [',
]
out += [f'  [{r}], // {chr(0x20 + i)!r}' for i, r in enumerate(rows)]
out.append('];')
(root / 'web/js/font5x7.js').write_text('\n'.join(out) + '\n', encoding='utf-8', newline='\n')
print(f'{len(rows)} glyphs written')
