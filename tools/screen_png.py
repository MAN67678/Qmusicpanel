#!/usr/bin/env python3
"""Rasterize a pty dump into a PNG so colours (and the cover art) are visible.

Usage: screen_png.py <dump.bin> <out.png> [cols rows] [--bg RRGGBB] [--cell W H]
Block/quadrant/half glyphs are drawn as real sub-cell coverage; everything else
is drawn as its foreground colour over the cell background.
"""
import sys, zlib, struct

import pyte

BLOCK_COVERAGE = {
    ' ': 0.0,
    '█': 1.0, '▇': 1.0,
    '▀': None,   # top half
    '▄': None,   # bottom half
    '▌': None,   # left half
    '▐': None,   # right half
    '▊': None, '▋': None, '▉': None, '▍': None, '▎': None, '▏': None,
    '▁': None, '▂': None, '▃': None, '▅': None, '▆': None,
    '░': 0.25, '▒': 0.5, '▓': 0.75,
    '●': 0.7, '▪': 0.8,
}


def parse_color(s, default):
    if s is None or s == 'default':
        return default
    s = s.lstrip('#')
    if len(s) == 6:
        try:
            return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))
        except ValueError:
            return default
    if s.startswith('('):  # rgb:rr/gg/bb or similar
        nums = []
        for part in s.strip('()').replace('/', ',').split(','):
            part = part.split(':')[-1]
            try:
                nums.append(int(part, 16) if not part.isdigit() else int(part))
            except ValueError:
                nums.append(0)
        if len(nums) == 3:
            return tuple(3 if n < 8 else 14 for n in nums)
    if s.isdigit():
        return default
    return default


def coverage(ch, x, y, cw, chh):
    """Return per-pixel blend of fg over bg for this cell, or None for a full fg."""
    if ch == '█' or ch == '▇':
        return None
    if ch in ('▀',):
        return [[1.0 if py < chh // 2 else 0.0 for _ in range(cw)] for py in range(chh)]
    if ch in ('▄',):
        return [[1.0 if py >= chh // 2 else 0.0 for _ in range(cw)] for py in range(chh)]
    if ch in ('▌', '▍', '▎', '▏'):
        frac = {'▌': .5, '▍': .375, '▎': .25, '▏': .125}[ch]
        return [[1.0 if px < int(cw * frac) else 0.0 for px in range(cw)] for _ in range(chh)]
    if ch in ('▐',):
        return [[1.0 if px >= cw // 2 else 0.0 for px in range(cw)] for _ in range(chh)]
    if ch in ('▊', '▋', '▉'):
        frac = {'▊': 1.0, '▋': .875, '▉': .75}[ch]
        return [[1.0 if px < int(cw * frac) else 0.0 for px in range(cw)] for _ in range(chh)]
    if ch in ('▁', '▂', '▃', '▅', '▆'):
        frac = {'▁': .125, '▂': .25, '▃': .375, '▅': .625, '▆': .75}[ch]
        top = int(chh * (1 - frac))
        return [[1.0 if py >= top else 0.0 for _ in range(cw)] for py in range(chh)]
    cov = BLOCK_COVERAGE.get(ch)
    if cov is None:
        return None  # solid fg glyph
    return [[cov for _ in range(cw)] for _ in range(chh)]


def write_png(path, w, h, pixels):
    raw = b''.join(b'\x00' + bytes(pixels[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def main():
    dump = sys.argv[1]
    out = sys.argv[2]
    cols, rows = 92, 28
    args = sys.argv[3:]
    bg_default = (12, 14, 24)
    cw, chh = 8, 16
    i = 0
    while i < len(args):
        if args[i] == '--bg':
            bg_default = tuple(int(args[i + 1][j:j + 2], 16) for j in (0, 2, 4)); i += 2
        elif args[i] == '--cell':
            cw, chh = (int(v) for v in args[i + 1].split('x')); i += 2
        elif args[i].isdigit() and i + 1 < len(args) and args[i + 1].isdigit():
            cols, rows = int(args[i]), int(args[i + 1]); i += 2
        else:
            i += 1

    data = open(dump, 'rb').read().decode('utf-8', 'replace')
    scr = pyte.Screen(cols, rows)
    pyte.Stream(scr).feed(data)

    W, H = cols * cw, rows * chh
    px = bytearray([0] * (W * H * 3))
    for y in range(rows):
        row = scr.buffer[y]
        for x in range(cols):
            cell = row[x]
            fg = parse_color(cell.fg, (230, 232, 240))
            bg = parse_color(cell.bg, bg_default)
            cov = coverage(cell.data, x, y, cw, chh)
            bx, by = x * cw, y * chh
            for py in range(chh):
                o = ((by + py) * W + bx) * 3
                for pxi in range(cw):
                    if cov is None:
                        a = 1.0
                    else:
                        a = cov[py][pxi]
                    r = int(bg[0] + (fg[0] - bg[0]) * a)
                    g = int(bg[1] + (fg[1] - bg[1]) * a)
                    b = int(bg[2] + (fg[2] - bg[2]) * a)
                    px[o] = r; px[o + 1] = g; px[o + 2] = b
                    o += 3
    write_png(out, W, H, px)
    print(f"wrote {out} ({W}x{H})")


if __name__ == '__main__':
    main()
