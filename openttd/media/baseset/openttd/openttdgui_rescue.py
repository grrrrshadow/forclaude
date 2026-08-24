#!/usr/bin/env python3
"""Generate openttdgui_rescue.png: the rescue-engine button icon.

The artwork is a hammer and wrench from Icons8 (https://icons8.com), a
16x16 PNG with an alpha channel and anti-aliased edges. Neither of those
survives a base-set sprite, which is 8bpp in the DOS palette with index 0
standing for "nothing here", so this script does three things to it:

* Anything less than half opaque becomes nothing. Anti-aliased edges
  otherwise turn into half-lit pixels with no colour to be, and the game
  draws them as solid.
* Every remaining pixel is snapped to the nearest colour of the palette's
  grey ramp or its wood ramp, and to nothing else. Letting it pick freely
  is what ruins these icons: the DOS palette is short of neutral greys, so
  a soft grey edge lands on the nearest blue and the whole icon gains a
  violet fringe.
* A one pixel dark outline is drawn around the shape, the way every other
  button icon in the game has one. Without it the tools disappear into the
  grey of the button they sit on.

The sprite is 16x16 at (2, 2), matching the line appended to
openttdgui.nfo. Run from this directory; needs Pillow.
"""

from __future__ import annotations

import pathlib

from PIL import Image

HERE = pathlib.Path(__file__).parent
SOURCE = HERE / "icons8-hammer-and-wrench-16.png"
PALETTE_FROM = HERE / "openttdgui.png"
OUTPUT = HERE / "openttdgui_rescue.png"

MARGIN = 2
OPAQUE_ENOUGH = 110  # out of 255

#: Grey ramp of the DOS palette, darkest to lightest.
GREYS = list(range(1, 16))
#: Two wood/tan ramps, for the handles.
WOODS = [34, 35, 36, 37, 38, 39, 54, 55, 56, 57, 58, 59, 108, 109, 110, 111]
#: The outline colour: the second-darkest grey rather than pure black, which
#: the palette does not really have and which would look like a hole.
OUTLINE = 2


def nearest(palette: list[int], candidates: list[int], rgb: tuple[int, int, int]) -> int:
    """Index of the candidate palette entry closest to @p rgb."""
    def distance(index: int) -> int:
        r, g, b = palette[index * 3:index * 3 + 3]
        return (r - rgb[0]) ** 2 + (g - rgb[1]) ** 2 + (b - rgb[2]) ** 2

    return min(candidates, key=distance)


def main() -> None:
    palette = Image.open(PALETTE_FROM).getpalette()
    source = Image.open(SOURCE).convert("RGBA")
    w, h = source.size

    out = Image.new("P", (w + 2 * MARGIN, h + 2 * MARGIN), 0)
    out.putpalette(palette)

    src = source.load()
    dst = out.load()
    candidates = GREYS + WOODS

    for y in range(h):
        for x in range(w):
            r, g, b, a = src[x, y]
            if a < OPAQUE_ENOUGH:
                continue
            dst[x + MARGIN, y + MARGIN] = nearest(palette, candidates, (r, g, b))

    # The outline is drawn from the finished shape, so that it follows the
    # snapped pixels rather than the source's soft edge.
    filled = [[dst[x, y] != 0 for y in range(out.height)] for x in range(out.width)]
    for x in range(out.width):
        for y in range(out.height):
            if filled[x][y]:
                continue
            if any(0 <= x + dx < out.width and 0 <= y + dy < out.height and filled[x + dx][y + dy]
                   for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                dst[x, y] = OUTLINE

    out.save(OUTPUT)
    print(f"wrote {OUTPUT} ({out.width}x{out.height})")


if __name__ == "__main__":
    main()
