#!/usr/bin/env python3
"""Generate the sprites of the girls' grammar school and the vending machine.

Both are industries of the game's own (economy.extra_industries) and are
drawn from the base set, since an industry of the game cannot reach a
NewGRF's sprites. The artwork is the player's, rendered in Blender at 4x zoom
(gymnazium_zin4.png, automat_zin4.png; the scripts are in the player's
graphics repository): the school on 2x2 tiles, the machine on one.

The school is cut into vertical strips along the tile columns, one per tile:
the west tile takes the column left of the north and south tiles, the east
tile the one right of them, and the south tile -- the front one, drawn last
-- the middle column with whatever of the back tile stands in it. The north
tile has no building sprite of its own; only its grass is drawn.

Every sprite comes twice: 32bpp at 4x zoom, as rendered, and 8bpp at normal
zoom for a game without a 32bpp blitter, scaled down and put in the DOS
palette, without the shadow (the palette has no see-through black).

The offsets are from the north corner of the sprite's own tile, which on
the render sits on a pixel boundary; in the game it sits between the first
and second pixel of a flat tile's top row, hence the one pixel (four at 4x)
to the right. Writes the PNGs and prints the lines for openttdgui.nfo. Run
from this directory; needs Pillow.
"""

from __future__ import annotations

import pathlib

from PIL import Image

HERE = pathlib.Path(__file__).parent
PALETTE_FROM = HERE / "openttdgui.png"

#: Sprites in the order of openttdgui.nfo: output name, source, x range of the
#: strip on the render (None for all), north corner of the sprite's tile.
SPRITES = [
    ("gymnazium_w", "gymnazium_zin4.png", (104, 232), (232, 296)),
    ("gymnazium_s", "gymnazium_zin4.png", (232, 488), (360, 360)),
    ("gymnazium_e", "gymnazium_zin4.png", (488, 616), (488, 296)),
    ("automat", "automat_zin4.png", None, (192, 128)),
]

#: The north corner of a flat tile lies this far right of a render's (4x).
NORTH_SHIFT = 4
#: A tile on the render is 128 pixels high at 4x (2:1); in the game it is 124
#: (31 rows at normal zoom), so the render is squeezed upright to fit, or its
#: front edges would lie a pixel (at normal zoom) on the tiles in front.
RENDER_TILE_HEIGHT = 128
GAME_TILE_HEIGHT = 124
#: Below this alpha a pixel is nothing at normal zoom (out of 255).
OPAQUE_ENOUGH = 128
#: The shadow: dark and see-through; left out of the 8bpp sprites.
SHADOW_ALPHA = 200
SHADOW_DARK = 40
#: Palette indices the 8bpp sprites may use: not 0 (nothing), not the company
#: colours, not the animated ones at the top.
USABLE = [i for i in range(1, 0xE3) if not 0xC6 <= i <= 0xCD]


def load(source: str) -> Image.Image:
    """The render, squeezed upright to the game's tile height."""
    image = Image.open(HERE / source).convert("RGBA")
    height = round(image.height * GAME_TILE_HEIGHT / RENDER_TILE_HEIGHT)
    return image.resize((image.width, height), Image.LANCZOS)


def squeeze(y: int) -> int:
    """A height on the render as it is after load()."""
    return round(y * GAME_TILE_HEIGHT / RENDER_TILE_HEIGHT)


def strip(image: Image.Image, columns: tuple[int, int] | None) -> Image.Image:
    """The image with everything outside the columns made transparent."""
    if columns is None:
        return image.copy()
    out = Image.new("RGBA", image.size, (0, 0, 0, 0))
    left, right = columns
    out.paste(image.crop((left, 0, right, image.height)), (left, 0))
    return out


def palette_image() -> Image.Image:
    """A palette image holding only the usable DOS colours, for quantising."""
    dos = Image.open(PALETTE_FROM).getpalette()
    pal = []
    for i in USABLE:
        pal += dos[i * 3:i * 3 + 3]
    pal += pal[:3] * (256 - len(USABLE))
    image = Image.new("P", (1, 1))
    image.putpalette(pal)
    return image


def to_8bpp(image: Image.Image) -> Image.Image:
    """Scale a 4x RGBA image to normal zoom and put it in the DOS palette."""
    pixels = image.load()
    for y in range(image.height):
        for x in range(image.width):
            r, g, b, a = pixels[x, y]
            if a < SHADOW_ALPHA and max(r, g, b) < SHADOW_DARK:
                pixels[x, y] = (0, 0, 0, 0)
    small = image.resize((image.width // 4, image.height // 4), Image.LANCZOS)
    alpha = small.getchannel("A")
    rgb = Image.new("RGB", small.size, (0, 0, 0))
    rgb.paste(small.convert("RGB"), mask=alpha)
    quantised = rgb.quantize(palette=palette_image(), dither=Image.Dither.NONE)
    out = Image.new("P", small.size, 0)
    out.putpalette(Image.open(PALETTE_FROM).getpalette())
    q, a, o = quantised.load(), alpha.load(), out.load()
    for y in range(small.height):
        for x in range(small.width):
            if a[x, y] >= OPAQUE_ENOUGH:
                o[x, y] = USABLE[q[x, y]] if q[x, y] < len(USABLE) else USABLE[0]
    return out


def to_8bpp_cut(image: Image.Image, columns: tuple[int, int] | None) -> Image.Image:
    """The 8bpp sprite of a strip: the whole render scaled, then cut."""
    whole = to_8bpp(image)
    if columns is None:
        return whole
    out = Image.new("P", whole.size, 0)
    out.putpalette(whole.getpalette())
    left, right = columns[0] // 4, columns[1] // 4
    out.paste(whole.crop((left, 0, right, whole.height)), (left, 0))
    return out


def main() -> None:
    lines = []
    for name, source, columns, north in SPRITES:
        north = (north[0], squeeze(north[1]))
        image = strip(load(source), columns)

        box = image.getchannel("A").getbbox()
        big = image.crop(box)
        big.save(HERE / f"{name}_32bpp.png")
        bx = box[0] - north[0] + NORTH_SHIFT
        by = box[1] - north[1]

        # Scaled whole and cut after, so that the strips meet without a seam.
        small_p = to_8bpp_cut(load(source), columns)
        sbox = small_p.getbbox()
        small_p.crop(sbox).save(HERE / f"{name}_8bpp.png")
        sx = sbox[0] - (north[0] - NORTH_SHIFT) // 4
        sy = sbox[1] - north[1] // 4

        lines.append(f"   -1 sprites/{name}_8bpp.png 8bpp 0 0 {sbox[2] - sbox[0]} {sbox[3] - sbox[1]} {sx} {sy} normal")
        lines.append(f"    | sprites/{name}_32bpp.png 32bpp 0 0 {big.width} {big.height} {bx} {by} zi4")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
