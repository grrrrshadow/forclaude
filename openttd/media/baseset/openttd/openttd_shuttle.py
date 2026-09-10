#!/usr/bin/env python3
"""Generate the shuttle sprites: one craft in two liveries, eight headings each.

The artwork is drawn rather than rendered, by tools/drawplane.py in the
grrrrf repository, which projects a solid model the way OpenTTD projects the
world -- `u = (y - x) * 2`, `v = x + y - z` -- so a craft heading north
points up the screen and one heading east points right. It writes what a
renderer would: eight PNGs per livery on a 248x248 canvas with the craft's
origin at the centre and an alpha channel round the edges, drawn at four
times the zoom the base set uses.

    drawplane.py --out . --livery ground --gear down --span 200 --canvas 248
    drawplane.py --out . --livery air    --gear up   --span 200 --canvas 248

A base-set sprite is none of those, so this does the same three things
openttdgui_crosshair.py does for the crosshair, and one more:

* **scales down by four**, because that is the difference between the zoom
  the renders were made for and the one the base set is drawn at. Scaled
  down from two hundred pixels the shape keeps its engines; drawn at fifty
  they would be a smudge.
* **throws away anything less than half opaque**, because a half-lit pixel
  has no colour to be and the game draws it solid.
* **snaps what is left onto the palette**, nearest colour, weighted the way
  an eye weighs them -- green most, blue least. Unlike the icons this is a
  shaded solid rather than one flat shape, so it cannot be painted a single
  colour off one ramp; it needs the whole palette.
* **keeps the origin**, which is the part that matters for a moving thing.
  Each render puts the craft's origin at the centre of its canvas, so the
  offsets on the nfo lines are measured from there and not from the middle
  of the cropped picture. Crop each heading around itself instead and the
  craft steps sideways as it turns.

Four parts of the palette are kept out of it:

* index 0, which is not a colour but "nothing here";
* 198-205, the company colour ramp, so nothing here changes colour with
  whoever owns it;
* 227-254, the animated colours -- oil refinery fires, lighthouses, glittery
  water -- which cycle while the game runs, so a sprite that lands on one
  flickers;
* 255, a second pure white, so white only ever comes out as index 15.

The scaling is done on premultiplied alpha. Scaling straight RGBA mixes the
colour of transparent pixels into the edge, and since transparent here is
black, every edge would come out darker than the craft.

Run from this directory; needs Pillow.
"""

from __future__ import annotations

import pathlib

from PIL import Image

HERE = pathlib.Path(__file__).parent
PALETTE_FROM = HERE / "openttdgui.png"
OUTPUT = HERE / "openttd_shuttle.png"

#: The two liveries, and the name each gets in the nfo comments.
CRAFT = [("red", "airport livery, wheels down"), ("grey", "airborne livery, wheels up")]
HEADINGS = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"]

#: The renders are drawn for zoom-in-4x; the base set is normal zoom.
SCALE = 4
#: Anything dimmer than this is not part of the craft.
OPAQUE_ENOUGH = 128
#: Space between sprites on the sheet, and round the outside.
GAP = 4
MARGIN = 2

RESERVED = {0, 255} | set(range(198, 206)) | set(range(226, 255))


def palette_of(path):
    """The base set's own palette, so these sprites use exactly the same one."""
    with Image.open(path) as im:
        raw = im.getpalette()
    return [tuple(raw[i * 3:i * 3 + 3]) for i in range(256)]


def nearest(palette, rgb):
    """Nearest colour, weighted 2:4:3 -- the eye's own weighting of red, green, blue."""
    r, g, b = rgb
    best, best_d = 1, None
    for i, (pr, pg, pb) in enumerate(palette):
        if i in RESERVED:
            continue
        d = 2 * (r - pr) ** 2 + 4 * (g - pg) ** 2 + 3 * (b - pb) ** 2
        if best_d is None or d < best_d:
            best, best_d = i, d
    return best


def shrink(path):
    """One render, scaled to base-set size, with its alpha kept honest."""
    with Image.open(path) as im:
        image = im.convert("RGBA")
    size = (image.width // SCALE, image.height // SCALE)

    # Premultiply, scale, then divide the colour back out. Scaling RGBA
    # directly would blend the colour behind the transparent pixels -- black --
    # into every edge.
    alpha = image.getchannel("A")
    premultiplied = Image.merge("RGB", [
        Image.eval(Image.merge("L", [c]).point(lambda v: v), lambda v: v)
        for c in image.split()[:3]])
    premultiplied = Image.composite(premultiplied,
                                    Image.new("RGB", image.size, (0, 0, 0)), alpha)
    small_rgb = premultiplied.resize(size, Image.LANCZOS)
    small_a = alpha.resize(size, Image.LANCZOS)

    out = Image.new("RGBA", size, (0, 0, 0, 0))
    rgb, a = small_rgb.load(), small_a.load()
    px = out.load()
    for y in range(size[1]):
        for x in range(size[0]):
            if a[x, y] < OPAQUE_ENOUGH:
                continue
            r, g, b = rgb[x, y]
            k = 255 / a[x, y]
            px[x, y] = (min(255, int(r * k)), min(255, int(g * k)),
                        min(255, int(b * k)), 255)
    return out


def main():
    palette = palette_of(PALETTE_FROM)
    cache = {}

    sprites = []
    for livery, _ in CRAFT:
        row = []
        for i, heading in enumerate(HEADINGS):
            small = shrink(HERE / f"shuttle_{livery}_{i}.png")
            box = small.getchannel("A").point(lambda v: 255 if v else 0).getbbox()
            origin = small.width // 2, small.height // 2
            row.append((small.crop(box), box[0] - origin[0], box[1] - origin[1],
                        livery, heading))
        sprites.append(row)

    columns = [max(row[i][0].width for row in sprites) for i in range(8)]
    xs, x = [], MARGIN
    for width in columns:
        xs.append(x)
        x += width + GAP
    height = MARGIN
    ys = []
    for row in sprites:
        ys.append(height)
        height += max(cell[0].height for cell in row) + GAP

    sheet = Image.new("P", (x - GAP + MARGIN, height - GAP + MARGIN), 0)
    flat = [v for colour in palette for v in colour]
    sheet.putpalette(flat)
    pixels = sheet.load()

    lines = []
    for r, row in enumerate(sprites):
        for c, (image, xoff, yoff, livery, heading) in enumerate(row):
            src = image.load()
            for y in range(image.height):
                for x0 in range(image.width):
                    pixel = src[x0, y]
                    if pixel[3] == 0:
                        continue
                    key = pixel[:3]
                    if key not in cache:
                        cache[key] = nearest(palette, key)
                    pixels[xs[c] + x0, ys[r] + y] = cache[key]
            lines.append(f"   -1 sprites/openttd_shuttle.png 8bpp {xs[c]:3d} {ys[r]:4d} "
                         f"{image.width:3d} {image.height:3d} {xoff:3d} {yoff:3d} normal"
                         f" // {livery} {heading}")

    sheet.save(OUTPUT)
    print(f"wrote {OUTPUT.name} {sheet.size[0]}x{sheet.size[1]}, "
          f"{len(set(cache.values()))} palette entries used")
    print()
    print("\n".join(lines))


if __name__ == "__main__":
    main()
