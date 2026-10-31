#!/usr/bin/env python3
"""Rasterize a pty dump into a PNG terminal screenshot (PIL + DejaVu Sans Mono).

Usage: screen_text_png.py <dump.bin> <out.png> [cols rows]
Falls back to a block-only renderer if PIL or the font is missing.
"""
import sys

import pyte

FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
]
DEFAULT_BG = (10, 14, 26)


def parse_color(s, default):
    if s is None or s == 'default':
        return default
    s = s.lstrip('#')
    if len(s) == 6:
        try:
            return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))
        except ValueError:
            return default
    return default


def main():
    dump, out = sys.argv[1], sys.argv[2]
    cols, rows = 92, 28
    rest = [a for a in sys.argv[3:] if not a.startswith('--')]
    if len(rest) >= 2:
        cols, rows = int(rest[0]), int(rest[1])

    scr = pyte.Screen(cols, rows)
    pyte.Stream(scr).feed(open(dump, 'rb').read().decode('utf-8', 'replace'))

    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        print("PIL missing — install pillow in the venv", file=sys.stderr)
        return 1
    font = None
    for path in FONT_CANDIDATES:
        try:
            font = ImageFont.truetype(path, 22)
            break
        except OSError:
            continue
    if font is None:
        print("no mono font found", file=sys.stderr)
        return 1

    cw, chh = 12, 24
    img = Image.new('RGB', (cols * cw, rows * chh), DEFAULT_BG)
    d = ImageDraw.Draw(img)

    for y in range(rows):
        row = scr.buffer[y]
        for x in range(cols):
            cell = row[x]
            data = cell.data
            if data == '':
                continue                      # right half of a wide glyph
            bg = parse_color(cell.bg, DEFAULT_BG)
            fg = parse_color(cell.fg, (226, 232, 240))
            d.rectangle([x * cw, y * chh, x * cw + cw - 1, y * chh + chh - 1], fill=bg)
            if data != ' ' and data != '\x00':
                d.text((x * cw, y * chh), data, font=font, fill=fg)
    img.save(out)
    print(f"wrote {out} ({img.width}x{img.height})")
    return 0


if __name__ == '__main__':
    sys.exit(main())
