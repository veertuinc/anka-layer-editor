#!/usr/bin/env python3
"""Render a tmux capture-pane -e stream as a PNG.

The letters come from a real monospace font (Menlo on macOS, DejaVu Sans Mono
on Linux) through Pillow, so the pictures stay readable.
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

FONT_SIZE = 20
# Bold first. A regular face at this size is too light once the picture is scaled in the README.
FONT_CANDIDATES = (
    ("/System/Library/Fonts/Menlo.ttc", 1),
    ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", 0),
    ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 0),
    ("/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf", 0),
    ("/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", 0),
)

DEFAULT_FG = (230, 230, 230)
DEFAULT_BG = (32, 32, 32)

# ANSI colors 0-7, then bright 0-7. Yellow and red match the editor pairs.
ANSI = (
    (20, 20, 20),
    (205, 49, 49),
    (13, 188, 121),
    (255, 210, 0),
    (36, 114, 200),
    (188, 63, 188),
    (17, 168, 205),
    (229, 229, 229),
    (128, 128, 128),
    (255, 99, 99),
    (80, 230, 160),
    (255, 220, 120),
    (120, 170, 255),
    (230, 120, 230),
    (120, 220, 240),
    (255, 255, 255),
)
CUBE = (0, 95, 135, 175, 215, 255)


def xterm_256(index):
    if index < 16:
        return ANSI[index]
    if index < 232:
        index -= 16
        return (CUBE[index // 36], CUBE[(index % 36) // 6], CUBE[index % 6])
    grey = 8 + (index - 232) * 10
    return (grey, grey, grey)


class Style:
    def __init__(self):
        self.fg = None
        self.bg = None
        self.reverse = False
        self.dim = False

    def reset(self):
        self.fg = None
        self.bg = None
        self.reverse = False
        self.dim = False

    def colors(self):
        fg = DEFAULT_FG if self.fg is None else xterm_256(self.fg)
        bg = DEFAULT_BG if self.bg is None else xterm_256(self.bg)
        if self.reverse:
            fg, bg = bg, fg
        if self.dim:
            fg = tuple(channel // 2 for channel in fg)
        return fg, bg


def apply_sgr(style, params):
    if not params:
        params = [0]
    i = 0
    while i < len(params):
        code = params[i]
        if code == 0:
            style.reset()
        elif code == 1:
            pass
        elif code == 2:
            style.dim = True
        elif code == 7:
            style.reverse = True
        elif code == 22:
            style.dim = False
        elif code == 27:
            style.reverse = False
        elif 30 <= code <= 37:
            style.fg = code - 30
        elif code == 39:
            style.fg = None
        elif 40 <= code <= 47:
            style.bg = code - 40
        elif code == 49:
            style.bg = None
        elif 90 <= code <= 97:
            style.fg = 8 + (code - 90)
        elif 100 <= code <= 107:
            style.bg = 8 + (code - 100)
        elif code == 38 and i + 2 < len(params) and params[i + 1] == 5:
            style.fg = params[i + 2]
            i += 2
        elif code == 48 and i + 2 < len(params) and params[i + 1] == 5:
            style.bg = params[i + 2]
            i += 2
        i += 1


def parse_pane(text):
    """Return rows of (character, style-copy) cells. Styles persist across lines.

    Pad a short row with the style at the end of that row. The style at the
    end of the capture is the reverse footer, and using it for every short
    row paints the empty area light grey.
    """
    style = Style()
    rows = []
    row = []
    i = 0
    while i < len(text):
        if text.startswith("\x1b[", i):
            end = i + 2
            while end < len(text) and text[end] not in "mH":
                if text[end].isalpha():
                    break
                end += 1
            if end < len(text) and text[end] == "m":
                body = text[i + 2 : end]
                params = [int(part) if part else 0 for part in body.split(";")] if body else [0]
                apply_sgr(style, params)
            i = end + 1
            continue
        char = text[i]
        i += 1
        if char == "\n":
            rows.append((row, StyleCopy(style)))
            row = []
            continue
        if char == "\r":
            continue
        row.append((char, StyleCopy(style)))
    if row:
        rows.append((row, StyleCopy(style)))
    width = max((len(line) for line, _style in rows), default=0)
    padded = []
    for line, end_style in rows:
        while len(line) < width:
            line.append((" ", StyleCopy(end_style)))
        padded.append(line)
    return padded


class StyleCopy:
    def __init__(self, style):
        self.fg = style.fg
        self.bg = style.bg
        self.reverse = style.reverse
        self.dim = style.dim

    def colors(self):
        return Style.colors(self)


def load_font():
    for path, index in FONT_CANDIDATES:
        if os.path.isfile(path):
            return ImageFont.truetype(path, FONT_SIZE, index=index)
    print("ansi_to_png.py: no monospace font found", file=sys.stderr)
    return None


def render(rows, font):
    cell_w = max(1, round(font.getlength("M")))
    ascent, descent = font.getmetrics()
    pad_y = 2
    cell_h = ascent + descent + pad_y
    width_cells = len(rows[0]) if rows else 0
    image = Image.new("RGB", (width_cells * cell_w, len(rows) * cell_h), DEFAULT_BG)
    draw = ImageDraw.Draw(image)
    for y, line in enumerate(rows):
        for x, (char, style) in enumerate(line):
            fg, bg = style.colors()
            origin_x = x * cell_w
            origin_y = y * cell_h
            draw.rectangle(
                (origin_x, origin_y, origin_x + cell_w, origin_y + cell_h),
                fill=bg,
            )
            if char != " ":
                draw.text((origin_x, origin_y + pad_y // 2), char, font=font, fill=fg)
    return image


def main(argv):
    if len(argv) != 2:
        print("usage: ansi_to_png.py OUTPUT.png", file=sys.stderr)
        return 2
    font = load_font()
    if font is None:
        return 1
    text = sys.stdin.read()
    rows = parse_pane(text)
    if not rows:
        print("ansi_to_png.py: empty pane", file=sys.stderr)
        return 1
    render(rows, font).save(argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
