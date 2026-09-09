#!/usr/bin/env python3
"""Generate the crosshair sprites: the button icon and the mouse cursor.

The artwork is a crosshair from Icons8 (https://icons8.com), supplied as
PNGs with an alpha channel and anti-aliased edges. A base-set sprite is
neither: it is 8bpp in the DOS palette, with index 0 standing for "nothing
here". So, exactly as openttdgui_rescue.py does for the rescue engine's
hammer and wrench:

* anything less than half opaque becomes nothing, because a half-lit pixel
  has no colour to be and the game would draw it solid;
* a one pixel outline is drawn around the finished shape, the way every
  other icon in the game has one, or it disappears into whatever it is
  drawn on.

Both sprites come from the big drawing scaled down, not from the small one
drawn small: the crosshair is a thin ring, and a ring drawn at sixteen
pixels comes with half its circle under the "opaque enough" line and breaks
into dots. Scaled down from fifty pixels it stays a ring.

Two sprites come out of it:

* the button icon, 16x16, which is the size of every button icon in the
  vehicle window -- the orders button, the details button, the depot
  button and the rescue engine are all 16x16, and an icon of another size
  makes its row taller than the rest;
* the mouse cursor, 32x32, in the palette's brightest red, which is what
  the player asked for: a red crosshair.

Colour is not read off the drawing. The drawing is black, and asking the
palette which red is closest to black gets the darkest red there is --
(92, 0, 0), which on the map is a black crosshair with a story. The shape
is painted one flat colour and outlined in near-black instead, so it reads
at a glance on grass, on rails and on a town.

The cursor's hotspot is not in the picture. It is the offset written on the
sprite's line in openttdgui.nfo, and for a crosshair it is the middle:
half the width and half the height, negated.

Run from this directory; needs Pillow.
"""

from __future__ import annotations

import pathlib

from PIL import Image

HERE = pathlib.Path(__file__).parent
PALETTE_FROM = HERE / "openttdgui.png"

#: The drawing both sprites are scaled down from.
SOURCE = HERE / "icons8-crosshair-50.png"
BUTTON_OUTPUT = HERE / "openttdgui_crosshair.png"
CURSOR_OUTPUT = HERE / "openttdgui_crosshair_cursor.png"

#: Button icons in the vehicle window are 16x16; the cursor is bigger so it
#: reads as a crosshair on the map.
BUTTON_SIZE = 16
CURSOR_SIZE = 32

MARGIN = 2
OPAQUE_ENOUGH = 96  # out of 255

#: The button icon's colour: the same near-black the other button icons are
#: drawn in, on the grey button behind it.
BUTTON_COLOUR = 1
#: The cursor's colour: 184 is the brightest true red in the palette,
#: (252, 0, 0). Past it the ramp turns orange and then yellow.
CURSOR_COLOUR = 184
#: The outline colour: near-black, dark enough to hold the shape against
#: light ground without being a hole.
OUTLINE = 1


def convert(size: int, output: pathlib.Path, palette: list[int], colour: int, outline: int | None) -> None:
    """Scale the drawing down, flatten it onto one palette colour, outline it."""
    src_image = Image.open(SOURCE).convert("RGBA").resize((size, size), Image.LANCZOS)

    out = Image.new("P", (size + 2 * MARGIN, size + 2 * MARGIN), 0)
    out.putpalette(palette)

    src = src_image.load()
    dst = out.load()

    for y in range(size):
        for x in range(size):
            if src[x, y][3] < OPAQUE_ENOUGH:
                continue
            dst[x + MARGIN, y + MARGIN] = colour

    # The outline follows the flattened pixels, not the source's soft edge.
    if outline is not None:
        filled = [[dst[x, y] != 0 for y in range(out.height)] for x in range(out.width)]
        for x in range(out.width):
            for y in range(out.height):
                if filled[x][y]:
                    continue
                if any(0 <= x + dx < out.width and 0 <= y + dy < out.height and filled[x + dx][y + dy]
                       for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                    dst[x, y] = outline

    out.save(output)
    print(f"wrote {output} ({out.width}x{out.height})")


def main() -> None:
    palette = Image.open(PALETTE_FROM).getpalette()
    # No outline on the button: the drawing is a thin ring already dark
    # against a grey button, and a ring of sixteen pixels with a border
    # around it is a blob.
    convert(BUTTON_SIZE, BUTTON_OUTPUT, palette, BUTTON_COLOUR, None)
    convert(CURSOR_SIZE, CURSOR_OUTPUT, palette, CURSOR_COLOUR, OUTLINE)


if __name__ == "__main__":
    main()
