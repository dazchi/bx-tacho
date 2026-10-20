#!/usr/bin/env python3
"""Convert a stroked SVG icon (e.g. Tabler Icons) into a 1-bit C bitmap.

Supports the path commands Tabler uses (M/m, L/l, H/h, V/v, A/a, Z/z) with
round caps and joins, per-path stroke-width, and fill (fill="currentColor",
stroke="none" for solid shapes). Output is row-major, 1 bit per pixel, MSB = leftmost
pixel, rows padded to whole bytes, as drawn by display_bitmap().

    python svg_icon_to_c.py battery-off.svg --size 48 --name icon_battery_off_48 --preview
"""
import argparse
import math
import re
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw

SUPERSAMPLE = 8


def arc_points(x1, y1, rx, ry, phi_deg, large, sweep, x2, y2, steps_per_rad=16):
    """SVG endpoint arc -> list of points (SVG 1.1 implementation notes F.6)."""
    if rx == 0 or ry == 0 or (x1 == x2 and y1 == y2):
        return [(x2, y2)]
    rx, ry = abs(rx), abs(ry)
    phi = math.radians(phi_deg)
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = (x1p / rx) ** 2 + (y1p / ry) ** 2
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    coef = math.sqrt(max(0.0, num / den)) * (-1 if large == sweep else 1)
    cxp, cyp = coef * rx * y1p / ry, -coef * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x1 + x2) / 2
    cy = sp * cxp + cp * cyp + (y1 + y2) / 2

    def angle(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    n = max(2, int(abs(dt) * steps_per_rad))
    pts = []
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        px, py = rx * math.cos(t), ry * math.sin(t)
        pts.append((cp * px - sp * py + cx, sp * px + cp * py + cy))
    return pts


def path_to_polylines(d):
    tokens = re.findall(r"[MmLlHhVvAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?", d)
    polylines, cur, x, y, sx, sy, i, cmd = [], [], 0.0, 0.0, 0.0, 0.0, 0, None

    def num():
        nonlocal i
        v = float(tokens[i])
        i += 1
        return v

    while i < len(tokens):
        if re.match(r"[A-Za-z]", tokens[i]):
            cmd = tokens[i]
            i += 1
            if cmd in "Zz":
                cur.append((sx, sy))
                x, y = sx, sy
                continue
        rel = cmd.islower()
        c = cmd.upper()
        if c == "M":
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            if len(cur) > 1:
                polylines.append(cur)
            cur, sx, sy = [(x, y)], x, y
            cmd = "l" if rel else "L"   # implicit lineto after moveto
        elif c == "L":
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            cur.append((x, y))
        elif c == "H":
            nx = num()
            x = x + nx if rel else nx
            cur.append((x, y))
        elif c == "V":
            ny = num()
            y = y + ny if rel else ny
            cur.append((x, y))
        elif c == "A":
            rx, ry, rot, large, sweep, nx, ny = (num() for _ in range(7))
            ex, ey = (x + nx, y + ny) if rel else (nx, ny)
            cur.extend(arc_points(x, y, rx, ry, rot, int(large), int(sweep), ex, ey))
            x, y = ex, ey
        else:
            raise ValueError(f"unsupported path command {cmd}")
    if len(cur) > 1:
        polylines.append(cur)
    return polylines


def render(svg_path, size, stroke_scale):
    root = ET.parse(svg_path).getroot()
    vb = [float(v) for v in root.get("viewBox", "0 0 24 24").split()]
    root_stroke = float(root.get("stroke-width", "2"))
    root_fill = root.get("fill", "none")
    big = size * SUPERSAMPLE
    s = big / vb[2]
    img = Image.new("L", (big, big), 0)
    draw = ImageDraw.Draw(img)
    for el in root.iter():
        if not el.tag.endswith("path"):
            continue
        # Per-path stroke-width / stroke="none" / fill override the <svg> defaults
        w = float(el.get("stroke-width", root_stroke)) * stroke_scale * s
        stroked = el.get("stroke") != "none"
        filled = el.get("fill", root_fill) not in ("none", "transparent")
        for poly in path_to_polylines(el.get("d")):
            pts = [((px - vb[0]) * s, (py - vb[1]) * s) for px, py in poly]
            if filled and len(pts) > 2:
                draw.polygon(pts, fill=255)
            if stroked:
                draw.line(pts, fill=255, width=max(1, round(w)), joint="curve")
                for px, py in (pts[0], pts[-1]):   # round caps
                    draw.ellipse((px - w / 2, py - w / 2, px + w / 2, py + w / 2), fill=255)
    small = img.resize((size, size), Image.BOX)
    return [[1 if small.getpixel((cx, cy)) >= 128 else 0 for cx in range(size)] for cy in range(size)]


def to_c(bits, name, source):
    h, w = len(bits), len(bits[0])
    stride = (w + 7) // 8
    data = []
    for row in bits:
        for b in range(stride):
            byte = 0
            for k in range(8):
                xx = b * 8 + k
                if xx < w and row[xx]:
                    byte |= 0x80 >> k
            data.append(byte)
    lines = [", ".join(f"0x{v:02X}" for v in data[r * stride:(r + 1) * stride]) + ","
             for r in range(h)]
    body = "\n".join("    " + l for l in lines)
    return (f"// {name}: {w}x{h}, generated by tools/svg_icon_to_c.py from {source}\n"
            f"const uint8_t {name}[{len(data)}] = {{\n{body}\n}};\n")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("svg")
    ap.add_argument("--size", type=int, default=48)
    ap.add_argument("--name", required=True)
    ap.add_argument("--stroke-scale", type=float, default=1.0,
                    help="multiply the SVG stroke width (thicker lines read better on a small OLED)")
    ap.add_argument("--preview", action="store_true", help="print the bitmap as ASCII art")
    args = ap.parse_args()
    bits = render(args.svg, args.size, args.stroke_scale)
    if args.preview:
        for row in bits:
            print("".join("#" if v else "." for v in row))
    print(to_c(bits, args.name, args.svg.replace("\\", "/").split("/")[-1]))
