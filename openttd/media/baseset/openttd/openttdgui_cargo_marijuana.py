#!/usr/bin/env python3
"""Generate openttdgui_cargo_marijuana.png: the cargo icon for marijuana.

The marijuana plantation (economy.extra_industries) grows a cargo the
original graphics have no icon for. The artwork is a cannabis leaf from
Icons8 (https://icons8.com), a 10x10 glyph -- the size of the game's own
cargo icons -- green, with an alpha channel and anti-aliased edges.

A base-set sprite is 8bpp in the DOS palette with index 0 standing for
"nothing here", so anything less than half opaque becomes nothing. The
palette has no green as bright as the glyph's, so the leaf takes the
brightest plain green there is, and its half-lit edge a darker one, which
keeps the fingers of the leaf apart at this size.

The sprite is 10x10 at (2, 2), matching the line in openttdgui.nfo. Run from
this directory; needs Pillow.
"""

from __future__ import annotations

import pathlib

from PIL import Image

HERE = pathlib.Path(__file__).parent
SOURCE = HERE / "icons8-cannabis-10.png"
PALETTE_FROM = HERE / "openttdgui.png"
OUTPUT = HERE / "openttdgui_cargo_marijuana.png"

MARGIN = 2
OPAQUE_ENOUGH = 110  # out of 255
SOLID = 192  # out of 255
#: Greens of the DOS palette: the leaf, and its edge.
LEAF = 0xCF
EDGE = 0x54


def main() -> None:
    palette = Image.open(PALETTE_FROM).getpalette()
    source = Image.open(SOURCE).convert("RGBA")
    w, h = source.size

    out = Image.new("P", (w + 2 * MARGIN, h + 2 * MARGIN), 0)
    out.putpalette(palette)

    src = source.load()
    dst = out.load()
    for y in range(h):
        for x in range(w):
            alpha = src[x, y][3]
            if alpha >= SOLID:
                dst[x + MARGIN, y + MARGIN] = LEAF
            elif alpha >= OPAQUE_ENOUGH:
                dst[x + MARGIN, y + MARGIN] = EDGE

    out.save(OUTPUT)
    print(f"wrote {OUTPUT} ({out.width}x{out.height})")


if __name__ == "__main__":
    main()
