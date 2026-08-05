#!/usr/bin/env python3
"""Converts scripts/logo_data.json into src/ui/logo_ha.h.

The JSON is produced by scripts/gen_ha_logo.html, which rasterises Home
Assistant's own launch-screen SVG in a browser — no SVG rasteriser (rsvg,
inkscape, PIL) is installed on this machine. See CLAUDE.md.

Usage:  python3 scripts/gen_logo_header.py
"""
import json
import pathlib

SRC = pathlib.Path(__file__).parent / "logo_data.json"
DST = pathlib.Path(__file__).parent.parent / "src" / "ui" / "logo_ha.h"

d = json.load(open(SRC))
size, pal, rle = d["size"], d["palette"], d["rle"]

assert len(pal) <= 256, "palette index must fit a byte"
assert sum(rle[1::2]) == size * size, "RLE does not cover the bitmap"
assert all(1 <= n <= 255 for n in rle[1::2]), "run length out of byte range"
assert all(0 <= i < len(pal) for i in rle[0::2]), "palette index out of range"


def wrap(vals, per_line, fmt):
    out, line = [], []
    for v in vals:
        line.append(fmt(v))
        if len(line) == per_line:
            out.append("  " + ", ".join(line) + ",")
            line = []
    if line:
        out.append("  " + ", ".join(line) + ",")
    return "\n".join(out)


hdr = f"""#pragma once
#include <stdint.h>

// Home Assistant logo for the boot splash — {size}x{size}, RLE-encoded palette
// indices. GENERATED FILE, do not hand-edit: run
//   python3 scripts/gen_logo_header.py
// after regenerating scripts/logo_data.json with scripts/gen_ha_logo.html.
//
// The mark is Home Assistant's own launch-screen SVG. The palette's background
// entry is exactly C_BG, so the bitmap can be blitted as an opaque rectangle
// with no transparency handling.
//
// Storage: {len(rle)} bytes of RLE + {len(pal) * 2} bytes of palette, versus
// {size * size * 2} bytes for a raw RGB565 bitmap ({size * size * 2 / (len(rle) + len(pal) * 2):.0f}x smaller).

#define LOGO_HA_W       {size}
#define LOGO_HA_H       {size}
#define LOGO_HA_RLE_LEN {len(rle)}

static const uint16_t LOGO_HA_PAL[{len(pal)}] = {{
{wrap(pal, 8, lambda v: f"0x{v:04x}")}
}};

// Flat pairs: (palette index, run length). Runs may span row boundaries.
static const uint8_t LOGO_HA_RLE[LOGO_HA_RLE_LEN] = {{
{wrap(rle, 16, lambda v: f"{v:3d}")}
}};
"""

DST.write_text(hdr)
print(f"wrote {DST.relative_to(DST.parent.parent.parent)}")
print(f"  {size}x{size}, palette {len(pal)}, rle {len(rle)} B "
      f"(raw would be {size*size*2} B)")
