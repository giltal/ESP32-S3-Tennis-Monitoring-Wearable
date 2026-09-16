#!/usr/bin/env python3
"""Generate LVGL v9 ARGB8888 C-array icons for the Play-mode ring.
Draws each icon supersampled (x5) with Pillow, downsamples for smooth edges,
emits main/play_icons.c + main/play_icons.h."""
import os
from PIL import Image, ImageDraw

SIZE = 44          # on-device icon size (px)
SS   = 5           # supersample factor
S    = SIZE * SS   # working canvas
WHITE = (255, 255, 255, 255)
DARK  = (20, 20, 20, 255)

def canvas():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)

def px(*v):            # scale 44-space coords to supersampled space
    return tuple(int(round(c * SS)) for c in v)

def line(d, pts, col, w, round_cap=True):
    p = [px(x, y) for (x, y) in pts]
    d.line([c for xy in p for c in xy], fill=col, width=int(w * SS), joint="curve")
    if round_cap:
        r = w * SS / 2.0
        for (x, y) in p:
            d.ellipse([x - r, y - r, x + r, y + r], fill=col)

def disc(d, cx, cy, r, col):
    x0, y0 = px(cx - r, cy - r); x1, y1 = px(cx + r, cy + r)
    d.ellipse([x0, y0, x1, y1], fill=col)

def ring(d, cx, cy, r, col, w):
    x0, y0 = px(cx - r, cy - r); x1, y1 = px(cx + r, cy + r)
    d.ellipse([x0, y0, x1, y1], outline=col, width=int(w * SS))

def rect(d, x0, y0, x1, y1, col, w):
    d.rectangle([*px(x0, y0), *px(x1, y1)], outline=col, width=int(w * SS))

def poly(d, pts, col):
    d.polygon([c for (x, y) in pts for c in px(x, y)], fill=col)

# ── icon painters (44x44 space, centre 22,22) ──────────────────────────────
def ic_good():          # white check
    img, d = canvas()
    line(d, [(9, 23), (18, 32), (36, 11)], WHITE, 5)
    return img

def ic_out():           # court rectangle + centre line, ball landed OUTSIDE
    img, d = canvas()
    rect(d, 5, 13, 27, 32, DARK, 3)
    line(d, [(16, 13), (16, 32)], DARK, 2, round_cap=False)
    disc(d, 36, 11, 6, DARK)
    return img

def ic_bad():           # tennis net with the ball caught inside it
    img, d = canvas()
    # net frame: top cord + two posts
    line(d, [(7, 13), (37, 13)], WHITE, 2, round_cap=False)
    line(d, [(7, 13), (7, 34)], WHITE, 2, round_cap=False)
    line(d, [(37, 13), (37, 34)], WHITE, 2, round_cap=False)
    line(d, [(7, 34), (37, 34)], WHITE, 2, round_cap=False)
    # mesh
    for x in (13, 19, 25, 31):
        line(d, [(x, 13), (x, 34)], WHITE, 1, round_cap=False)
    for y in (19, 25, 31):
        line(d, [(7, y), (37, y)], WHITE, 1, round_cap=False)
    # ball resting in the net (white disc + orange-ish seam so it reads round)
    disc(d, 22, 26, 6, WHITE)
    line(d, [(17, 24), (27, 24)], (235, 120, 20, 255), 1, round_cap=False)
    # a couple of mesh strands in FRONT of the ball -> "in the net"
    line(d, [(19, 20), (19, 32)], WHITE, 1, round_cap=False)
    line(d, [(25, 20), (25, 32)], WHITE, 1, round_cap=False)
    return img

def ic_unforced():      # bold white X
    img, d = canvas()
    line(d, [(10, 10), (34, 34)], WHITE, 5)
    line(d, [(34, 10), (10, 34)], WHITE, 5)
    return img

def ic_ace():           # serve "bolt" (fast/unreturnable)
    img, d = canvas()
    poly(d, [(27, 6), (12, 25), (21, 25), (16, 38), (34, 18), (24, 18)], WHITE)
    return img

ICONS = [("good", ic_good), ("out", ic_out), ("bad", ic_bad),
         ("unforced", ic_unforced), ("ace", ic_ace)]

# main/ dir resolved relative to this script (scripts/gen_play_icons.py)
MAIN = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..", "main"))

def to_c_bytes(img):
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    data = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = img.getpixel((x, y))
            data += bytes((b, g, r, a))      # LVGL ARGB8888 little-endian = B,G,R,A
    return data

def main():
    c = ['#include "play_icons.h"', '']
    for name, fn in ICONS:
        data = to_c_bytes(fn())
        c.append(f"static const uint8_t play_ic_{name}_map[] = {{")
        for i in range(0, len(data), 16):
            c.append("    " + ",".join(str(v) for v in data[i:i+16]) + ",")
        c.append("};")
        c.append(f"const lv_image_dsc_t play_ic_{name} = {{")
        c.append("    .header = { .magic = LV_IMAGE_HEADER_MAGIC,")
        c.append("                .cf = LV_COLOR_FORMAT_ARGB8888, .flags = 0,")
        c.append(f"                .w = {SIZE}, .h = {SIZE}, .stride = {SIZE*4} }},")
        c.append(f"    .data_size = {SIZE*SIZE*4},")
        c.append(f"    .data = play_ic_{name}_map,")
        c.append("};")
        c.append("")
    with open(os.path.join(MAIN, "play_icons.c"), "w") as f:
        f.write("\n".join(c))

    h = ['#pragma once', '#include "lvgl.h"', '']
    for name, _ in ICONS:
        h.append(f"extern const lv_image_dsc_t play_ic_{name};")
    h.append("")
    with open(os.path.join(MAIN, "play_icons.h"), "w") as f:
        f.write("\n".join(h))
    print("wrote play_icons.c / play_icons.h")

if __name__ == "__main__":
    main()
