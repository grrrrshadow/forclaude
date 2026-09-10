#!/usr/bin/env python3
"""Generate the raid rocket's sprites from the player's renders.

The artwork is sixteen renders supplied by the player: one rocket in two
liveries, grey and yellow, each seen from the game's eight headings. They
arrive as 248x248 RGBA images with soft edges, and a base-set sprite is
neither: it is 8bpp in the DOS palette, with index 0 standing for "nothing
here".

Three things have to be got right, and each of them is a build cycle when
it is got wrong.

* **One origin for all eight headings.** Every render is drawn on the same
  canvas around the same point, so the sprite's offset is measured from the
  middle of the canvas, not from the middle of the drawing. Centre each
  sprite on itself and the rocket jumps sideways every time it turns.

* **Scaling down, not drawing small.** The renders are four times the size
  the game draws them at, and shrinking wants the average of the block, not
  a resampling filter: LANCZOS rings around a hard edge and pulls the thin
  parts -- the fins, the nose -- under the "opaque enough" line. The average
  is taken weighted by alpha, or the transparent surround bleeds in and puts
  a dark fringe around the whole rocket.

* **Two materials, two ramps.** The rocket is a coloured body with grey
  fittings, and matching against the whole palette gets neither: a grey
  finds an olive, a gold finds a brown. Neutral pixels are matched against
  the greys and coloured pixels against the gold ramp, so each material
  stays the colour it was drawn.

Run from this directory; needs Pillow. It writes the sixteen sprites and
prints the lines to put in openttdgui.nfo, offsets and all.
"""

from __future__ import annotations

import argparse
import pathlib

from PIL import Image

HERE = pathlib.Path(__file__).parent
PALETTE_FROM = HERE / "openttdgui.png"

#: The liveries, in the order the game numbers them: set 0 and set 1.
LIVERIES = ("grey", "yellow")
#: The game's eight headings.
HEADINGS = 8

SOURCE = "rocket_render_{livery}_{heading}.png"
OUTPUT = "openttdgui_rocket_{set}_{heading}.png"

#: How much bigger the renders are than the game draws them. Eight puts the
#: rocket at twenty-five pixels long seen side on -- under half a map tile,
#: which is a rocket beside a ship and not another ship.
SCALE = 8

#: A pixel less than this opaque has no colour to be, and the game would
#: draw it solid, so it becomes nothing.
OPAQUE_ENOUGH = 96  # out of 255

#: The blank border every sprite in this file carries.
MARGIN = 2

#: The palette's neutral greys, dark to light.
GREYS = list(range(1, 16))
#: The gold ramp, dark to light. Past 68 it goes white.
GOLDS = list(range(61, 69))
#: How far from neutral a pixel has to be before it counts as coloured.
COLOURED = 20


def nearest(palette: list[int], candidates: list[int], rgb: tuple[int, int, int]) -> int:
    """The candidate closest to rgb, by plain distance in the cube."""
    r, g, b = rgb
    return min(candidates, key=lambda i: (palette[i * 3] - r) ** 2
               + (palette[i * 3 + 1] - g) ** 2 + (palette[i * 3 + 2] - b) ** 2)


def shrink(source: Image.Image, scale: int) -> list[list[tuple[int, int, int, int]]]:
    """Average each scale x scale block, weighting the colour by alpha."""
    src = source.load()
    width, height = source.width // scale, source.height // scale
    out = []
    for y in range(height):
        row = []
        for x in range(width):
            r = g = b = a = 0
            for dy in range(scale):
                for dx in range(scale):
                    pr, pg, pb, pa = src[x * scale + dx, y * scale + dy]
                    r += pr * pa
                    g += pg * pa
                    b += pb * pa
                    a += pa
            if a == 0:
                row.append((0, 0, 0, 0))
            else:
                row.append((r // a, g // a, b // a, a // (scale * scale)))
        out.append(row)
    return out


def convert(source: pathlib.Path, output: pathlib.Path, palette: list[int], scale: int) -> tuple[int, int, int, int]:
    """Shrink one render, match it to the palette, crop it, and save it.

    Returns the sprite's width, height and offset from the render's origin,
    which is the middle of the canvas.
    """
    with Image.open(source) as image:
        rgba = image.convert("RGBA")
        origin_x = rgba.width // 2 // scale
        origin_y = rgba.height // 2 // scale
        pixels = shrink(rgba, scale)

    height, width = len(pixels), len(pixels[0])
    indexed = [[0] * width for _ in range(height)]
    for y in range(height):
        for x in range(width):
            r, g, b, a = pixels[y][x]
            if a < OPAQUE_ENOUGH:
                continue
            ramp = GREYS if max(r, g, b) - min(r, g, b) < COLOURED else GOLDS
            indexed[y][x] = nearest(palette, ramp, (r, g, b))

    left = min((x for y in range(height) for x in range(width) if indexed[y][x]), default=0)
    right = max((x for y in range(height) for x in range(width) if indexed[y][x]), default=0) + 1
    top = min((y for y in range(height) for x in range(width) if indexed[y][x]), default=0)
    bottom = max((y for y in range(height) for x in range(width) if indexed[y][x]), default=0) + 1

    out = Image.new("P", (right - left + 2 * MARGIN, bottom - top + 2 * MARGIN), 0)
    out.putpalette(palette)
    dst = out.load()
    for y in range(top, bottom):
        for x in range(left, right):
            dst[x - left + MARGIN, y - top + MARGIN] = indexed[y][x]
    out.save(output)

    return right - left, bottom - top, left - origin_x, top - origin_y


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scale", type=int, default=SCALE,
                        help="how many render pixels make one game pixel")
    args = parser.parse_args()

    palette = Image.open(PALETTE_FROM).getpalette()

    lines = []
    for number, livery in enumerate(LIVERIES):
        for heading in range(HEADINGS):
            source = HERE / SOURCE.format(livery=livery, heading=heading)
            output = HERE / OUTPUT.format(set=number, heading=heading)
            width, height, x_off, y_off = convert(source, output, palette, args.scale)
            lines.append(f"   -1 sprites/{output.name} 8bpp   {MARGIN}    {MARGIN}"
                         f" {width:3d} {height:3d} {x_off:3d} {y_off:3d} normal")
            print(f"wrote {output.name} ({width}x{height} at {x_off},{y_off})")

    print()
    print("lines for openttdgui.nfo:")
    for line in lines:
        print(line)


if __name__ == "__main__":
    main()
