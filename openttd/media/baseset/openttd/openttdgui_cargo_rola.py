#!/usr/bin/env python3
"""Generate openttdgui_cargo_rola.png: the cargo icon for road vehicles on wagons.

Every cargo the game has shows a small icon; the cargo for road vehicles
riding on wagons, ships and aircraft (CT_ROLA) had none of its own. The
artwork is a car from Icons8 (https://icons8.com), a 10x10 glyph -- the size
of the game's own cargo icons -- with an alpha channel and anti-aliased edges.

A base-set sprite is 8bpp in the DOS palette with index 0 standing for
"nothing here", so anything less than half opaque becomes nothing and the
rest takes the darkest grey of the palette's grey ramp: a black glyph, as it
was drawn, with no half-lit fringe that the game would draw solid.

The sprite is 10x10 at (2, 2), matching the line in openttdgui.nfo. Run from
this directory; needs Pillow.
"""

from __future__ import annotations

import pathlib

from PIL import Image

HERE = pathlib.Path(__file__).parent
SOURCE = HERE / "icons8-car-10.png"
PALETTE_FROM = HERE / "openttdgui.png"
OUTPUT = HERE / "openttdgui_cargo_rola.png"

MARGIN = 2
OPAQUE_ENOUGH = 110  # out of 255
#: The darkest grey of the DOS palette, which has no real black.
DARK = 1


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
            if src[x, y][3] >= OPAQUE_ENOUGH:
                dst[x + MARGIN, y + MARGIN] = DARK

    out.save(OUTPUT)
    print(f"wrote {OUTPUT} ({out.width}x{out.height})")


if __name__ == "__main__":
    main()
