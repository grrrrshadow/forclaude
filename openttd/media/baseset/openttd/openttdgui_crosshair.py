#!/usr/bin/env python3
"""Generate the crosshair sprites: the button icon and the mouse cursor.

The artwork is the "accuracy" crosshair from Icons8 (https://icons8.com),
supplied as PNGs with an alpha channel and anti-aliased edges. A base-set
sprite is neither: it is 8bpp in the DOS palette, with index 0 standing for
"nothing here". So, exactly as openttdgui_rescue.py does for the rescue
engine's hammer and wrench:

* anything less than half opaque becomes nothing, because a half-lit pixel
  has no colour to be and the game would draw it solid;
* every remaining pixel is snapped to one ramp of the palette and to
  nothing else, because letting it choose freely lands soft greys on the
  nearest blue and fringes the whole icon;
* a one pixel outline is drawn around the finished shape, the way every
  other icon in the game has one, or it disappears into whatever it is
  drawn on.

Two sprites come out of it:

* the button icon, 20x20 to match the other buttons of this build
  (Blueprint and the station waypoint), in the palette's grey ramp;
* the mouse cursor, 32x32, in the palette's red ramp, which is what the
  player asked for: a red crosshair.

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

#: The button icon: drawn small, so the 20 pixel source keeps its shape.
BUTTON_SOURCE = HERE / "icons8-accuracy-20.png"
BUTTON_OUTPUT = HERE / "openttdgui_crosshair.png"
#: The cursor: bigger, so it reads as a crosshair on the map.
CURSOR_SOURCE = HERE / "icons8-accuracy-32.png"
CURSOR_OUTPUT = HERE / "openttdgui_crosshair_cursor.png"

MARGIN = 2
OPAQUE_ENOUGH = 110  # out of 255

#: Grey ramp of the DOS palette, darkest to lightest.
GREYS = list(range(1, 16))
#: The pure red ramp, darkest to brightest. 184 is the brightest true red;
#: past it the ramp turns orange and then yellow, which is not a red
#: crosshair any more.
REDS = list(range(179, 185))
#: The outline colour: the second-darkest grey rather than pure black, which
#: the palette does not really have and which would look like a hole.
OUTLINE = 2


def nearest(palette: list[int], candidates: list[int], rgb: tuple[int, int, int]) -> int:
    """Index of the candidate palette entry closest to @p rgb."""
    def distance(index: int) -> int:
        r, g, b = palette[index * 3:index * 3 + 3]
        return (r - rgb[0]) ** 2 + (g - rgb[1]) ** 2 + (b - rgb[2]) ** 2

    return min(candidates, key=distance)


def convert(source: pathlib.Path, output: pathlib.Path, palette: list[int], candidates: list[int]) -> None:
    """Snap one icon onto the palette, outline it, and write it out."""
    src_image = Image.open(source).convert("RGBA")
    w, h = src_image.size

    out = Image.new("P", (w + 2 * MARGIN, h + 2 * MARGIN), 0)
    out.putpalette(palette)

    src = src_image.load()
    dst = out.load()

    for y in range(h):
        for x in range(w):
            r, g, b, a = src[x, y]
            if a < OPAQUE_ENOUGH:
                continue
            dst[x + MARGIN, y + MARGIN] = nearest(palette, candidates, (r, g, b))

    # The outline follows the snapped pixels, not the source's soft edge.
    filled = [[dst[x, y] != 0 for y in range(out.height)] for x in range(out.width)]
    for x in range(out.width):
        for y in range(out.height):
            if filled[x][y]:
                continue
            if any(0 <= x + dx < out.width and 0 <= y + dy < out.height and filled[x + dx][y + dy]
                   for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                dst[x, y] = OUTLINE

    out.save(output)
    print(f"wrote {output} ({out.width}x{out.height})")


def main() -> None:
    palette = Image.open(PALETTE_FROM).getpalette()
    convert(BUTTON_SOURCE, BUTTON_OUTPUT, palette, GREYS)
    convert(CURSOR_SOURCE, CURSOR_OUTPUT, palette, REDS)


if __name__ == "__main__":
    main()
