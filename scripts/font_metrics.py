#!/usr/bin/env python3
"""Re-derive the type metrics that src/ui/gfx.cpp and simulator.html hard-code.

Run this after changing which built-in face a FontRole maps to. It prints, ready
to paste:

  * ROLE_INK_TOP / ROLE_INK_BOT / ROLE_DY  for src/ui/gfx.cpp
  * the ADV / INK_TOP / INK_BOT / BASE tables for simulator.html

Why this exists rather than reading the font headers: the headers describe the
NOMINAL box (Font 2 is 16 tall with baseline 13, Font 4 is 26 tall with baseline
19), but every glyph sits inset inside that box with blank rows above and below
the ink. Font 2's caps start 3 rows down, Font 4's digits 2 rows down. Position
the degree ring off the nominal baseline and it lands in the wrong place, which
is exactly the class of bug the measured numbers below prevent.

No dependencies and no rasteriser: Font 2 is a plain row bitmap and Font 4 is
TFT_eSPI's 8-bit RLE, both decoded here directly from the library sources.

    python3 scripts/font_metrics.py
"""
import os
import re
import sys

LIB = os.environ.get(
    "TFT_ESPI_FONTS",
    os.path.join(os.path.dirname(__file__), os.pardir,
                 ".pio", "libdeps", "cyd", "TFT_eSPI", "Fonts"),
)

# The roles, in FontRole order, and the built-in font number each maps to.
# Font 1 is the 6x8 GLCD bitmap, which has no table to decode.
ROLES = [("F_NUM", 4), ("F_TITLE", 2), ("F_BODY", 2), ("F_MICRO", 1)]

# Nominal box from the font headers, used only to reproduce TFT_eSPI's own datum
# arithmetic — never as a stand-in for where the ink actually is.
BOX = {1: (8, 7), 2: (16, 13), 4: (26, 19)}      # font -> (height, baseline)

# Where the FreeSans build put the top ink row, relative to the cy an M* datum is
# given. F_NUM is pinned to its old value so the AC setpoint — the largest thing
# on the panel, with a degree ring aligned to its digit tops — comes out pixel
# identical. The Font 2 roles are re-centred instead (see want_top below).
LEGACY_TOP = {"F_NUM": -8, "F_TITLE": -6, "F_BODY": -6, "F_MICRO": -4}


def _strip_comments(src):
    return re.sub(r"//[^\n]*", "", src)


def _resolve_ifdefs(src, defines):
    """Keep the branch the compiler would keep for a simple #ifdef/#else/#endif."""
    def pick(m):
        name, yes, no = m.group(1), m.group(2), m.group(3) or ""
        return yes if name in defines else no
    return re.sub(r"#ifdef\s+(\w+)(.*?)(?:#else(.*?))?#endif", pick, src, flags=re.S)


def load_font(num):
    """Return (widths[96], glyph_rows(code) -> list of row bitlists)."""
    if num == 1:
        return None, None
    name = {2: "Font16", 4: "Font32rle"}[num]
    src = open(os.path.join(LIB, name + ".c")).read()
    defines = set(re.findall(r"^\s*#define\s+(\w+)", src, re.M))
    src = _resolve_ifdefs(_strip_comments(src), defines)

    tag = {2: "f16", 4: "f32"}[num]
    wm = re.search(r"widtbl_%s\[96\]\s*=\s*\{(.*?)\}" % tag, src, re.S)
    widths = [int(v) for v in re.findall(r"\d+", wm.group(1))][:96]
    assert len(widths) == 96, f"{name}: got {len(widths)} widths"

    data = {}
    for m in re.finditer(r"chr_%s_([0-9A-Fa-f]{2})\s*\[[^\]]*\]\s*=\s*\{(.*?)\}" % tag,
                         src, re.S):
        data[int(m.group(1), 16)] = [int(x, 0) for x in
                                     re.findall(r"0x[0-9A-Fa-f]+|\b\d+\b", m.group(2))]

    height = BOX[num][0]

    def rows(code):
        raw, w = data[code], widths[code - 32]
        if num == 2:                       # plain bitmap, MSB left, padded per row
            bpr = (w + 6) // 8
            out = []
            for r in range(height):
                bits = []
                for k in range(bpr):
                    b = raw[r * bpr + k]
                    bits += [(b >> (7 - i)) & 1 for i in range(8)]
                out.append(bits[:w])
            return out
        # Font 4: 8-bit RLE. High bit set => (n & 0x7F)+1 ink pixels, else n+1 blank.
        px, i, total = [], 0, w * height
        while len(px) < total and i < len(raw):
            b = raw[i]; i += 1
            px += [1] * ((b & 0x7F) + 1) if b & 0x80 else [0] * (b + 1)
        assert len(px) >= total, f"{name}: RLE for 0x{code:02X} is short"
        px = px[:total]
        return [px[r * w:(r + 1) * w] for r in range(height)]

    return widths, rows


def ink_extent(rows_fn, chars):
    top, bot = None, None
    for ch in chars:
        for i, row in enumerate(rows_fn(ord(ch))):
            if any(row):
                top = i if top is None else min(top, i)
                bot = i if bot is None else max(bot, i)
    return top, bot


CAPS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
DIGITS = "0123456789"
ALL = "".join(chr(c) for c in range(0x20, 0x7F))

top_out, bot_out, dy, widths_out, base_out, cap_out = {}, {}, {}, {}, {}, {}

for role, fnum in ROLES:
    if fnum == 1:
        # GLCD 6x8: a 5x7 face in a 6x8 cell, row 7 reserved for descenders.
        # Nothing here changes — F_MICRO was already this font.
        top_out[role], bot_out[role], dy[role] = -4, 3, 0
        base_out[role], cap_out[role], widths_out[role] = 3, 7, None
        continue

    widths, rows = load_font(fnum)
    height, nominal_base = BOX[fnum]

    # TFT_eSPI centres a built-in font on its FULL box: glyph_top = cy - height/2.
    glyph_top = -(height // 2)

    # The box to place is cap top down to the baseline. Q's tail dips past the
    # baseline and must NOT count as cap height, or every line sits too high.
    cap_top, _ = ink_extent(rows, CAPS + DIGITS)
    cap_h = nominal_base - cap_top
    _, all_bot = ink_extent(rows, ALL)

    # F_NUM keeps its exact old pixels. The Font 2 roles get re-centred: their cap
    # box shrank 13px -> 10px, and holding the old TOP would leave every line
    # riding high in a row height that was budgeted for the taller face.
    want_top = LEGACY_TOP[role] if role == "F_NUM" else -(cap_h // 2)
    dy[role] = want_top - (glyph_top + cap_top)

    top_out[role] = glyph_top + cap_top + dy[role]
    bot_out[role] = glyph_top + all_bot + dy[role]
    base_out[role] = glyph_top + nominal_base + dy[role]
    cap_out[role] = cap_h
    widths_out[role] = widths[:95]                 # 0x20..0x7E, as simulator.html

    print(f"# {role:<8} font {fnum}: box {height}px, cap ink from row {cap_top}, "
          f"baseline row {nominal_base}, descend to row {all_bot}", file=sys.stderr)
    print(f"#   -> dy {dy[role]:+d}; ink cy{top_out[role]:+d}..cy{bot_out[role]:+d}, "
          f"baseline cy{base_out[role]:+d}, cap height {cap_h}px", file=sys.stderr)

order = [r for r, _ in ROLES]
print("\n// ---- src/ui/gfx.cpp ----")
print("static const int8_t ROLE_INK_TOP[F_ROLES] = { %s };"
      % ", ".join(f"{top_out[r]:>2}" for r in order))
print("static const int8_t ROLE_INK_BOT[F_ROLES] = { %s };"
      % ", ".join(f"{bot_out[r]:>2}" for r in order))
print("static const int8_t ROLE_DY[F_ROLES]      = { %s };"
      % ", ".join(f"{dy[r]:>2}" for r in order))

print("\n// ---- simulator.html ----")
print("const ADV={")
for r in order:
    if widths_out[r] is None:
        continue
    print(f" {r[2:]}:[{','.join(str(v) for v in widths_out[r])}],")
print("};")
for nm, tbl in (("INK_TOP", top_out), ("INK_BOT", bot_out),
                ("BASE", base_out), ("CAPH", cap_out)):
    print("const %s={%s};" % (nm, ",".join(f"{r[2:]}:{tbl[r]}" for r in order)))
