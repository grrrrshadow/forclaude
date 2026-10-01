#!/usr/bin/env python3
"""Generate the sprites of the buildings of the game's own industries.

The girls' grammar school, the vending machine, the statue of Karel Macha
and the marijuana plantation are industries of the game's own
(economy.extra_industries), drawn from the base set, since an industry of
the game cannot reach a NewGRF's sprites. The artwork is the player's,
rendered in Blender at 4x zoom (*_zin4.png; the scripts are in the player's
graphics repository), each with a second picture: the school, the machine
and the statue with girls about them, the plantation fully grown.

How a picture is cut:

- whole: one tile, the picture is the tile's sprite (machine, statue).
- girl: a girl at a bus stop, the picture is the sprite, placed by her feet.
- strips: the school on 2x2 tiles, in vertical strips along the tile
  columns, one per tile: the west tile takes the column left of the north
  and south tiles, the east tile the one right of them, and the south tile
  -- the front one, drawn last -- the middle column with whatever of the
  back tile stands in it. The north tile has no sprite of its own.
- tiles: the plantation on 5x4 tiles, cut along the tiles' diamonds: a
  pixel belongs to the tile whose diamond it lies in, and one above the back
  edges of the field to the back tile below it. Nothing is drawn twice, so
  the pieces meet without a seam whatever order they are drawn in.

Every sprite comes twice: 32bpp at 4x zoom, as rendered, and 8bpp at normal
zoom for a game without a 32bpp blitter, scaled down and put in the DOS
palette, without the shadow (the palette has no see-through black).

A tile on the render is 256 x 128 pixels at 4x (2:1), the game's own grid:
tiles lie TILE_PIXELS (32) apart at normal zoom. That a flat ground sprite
is only 31 rows high does not change it -- its 32nd row is the side corners
of the tiles beside it -- and the render is taken as it is. (Squeezed to 31
rows once, by mistake: the buildings came out 3 % low and their front edges
inside their tiles.) The offsets are from the north corner of the sprite's own tile, which on the
render sits on a pixel boundary; in the game it sits between the first and
second pixel of a flat tile's top row, hence the one pixel (four at 4x) to
the right. Writes the PNGs and the lines for openttdgui.nfo (to standard
output). Run from this directory; needs Pillow.
"""

from __future__ import annotations

import math
import pathlib

from PIL import Image

HERE = pathlib.Path(__file__).parent
PALETTE_FROM = HERE / "openttdgui.png"

#: The north corner of a flat tile lies this far right of a render's (4x).
NORTH_SHIFT = 4
#: Half a tile across and down at 4x.
HALF_WIDTH = 128
HALF_HEIGHT = 64
#: Below this alpha a pixel is nothing at normal zoom (out of 255).
OPAQUE_ENOUGH = 128
#: The shadow: dark and see-through; left out of the 8bpp sprites.
SHADOW_ALPHA = 200
SHADOW_DARK = 40
#: Palette indices the 8bpp sprites may use: not 0 (nothing), not the company
#: colours, not the animated ones at the top.
USABLE = [i for i in range(1, 0xE3) if not 0xC6 <= i <= 0xCD]

#: The pictures, in the order of openttdgui.nfo (and of SPR_GYMNASIUM_WEST
#: and the rest in table/sprites.h): output name, source, how it is cut, the
#: north corner of the picture's north tile on the render, and for strips the
#: x ranges of the strips with the north corner of each strip's tile, for
#: tiles the field's size in tiles (x, y).
SCHOOL_STRIPS = [("w", (104, 232), (232, 296)), ("s", (232, 488), (360, 360)), ("e", (488, 616), (488, 296))]
PICTURES = [
    ("gymnazium", "gymnazium_zin4.png", "strips", (360, 232), SCHOOL_STRIPS),
    ("automat", "automat_zin4.png", "whole", (192, 128), None),
    ("gymnazium_holky", "gymnazium_postavy_zin4.png", "strips", (360, 232), SCHOOL_STRIPS[:2]),
    ("automat_holky", "automat_postavy_zin4.png", "whole", (192, 128), None),
    ("socha_kamen", "socha_kamen_zin4.png", "whole", (192, 128), None),
    ("socha_kamen_holky", "socha_kamen_postavy_zin4.png", "whole", (192, 128), None),
    ("socha_bronz", "socha_bronz_zin4.png", "whole", (192, 128), None),
    ("socha_bronz_holky", "socha_bronz_postavy_zin4.png", "whole", (192, 128), None),
    ("pole_male", "pole_marihuany_faze1_zin4.png", "tiles", (672, 96), (5, 4)),
    ("pole_velke", "pole_marihuany_faze2_zin4.png", "tiles", (672, 96), (5, 4)),
    # The girls at a drive-through bus stop (SPR_BUS_STOP_GIRL_X_FAR and the
    # rest), drawn as a child of a shelter: where the feet (80, 80 on the
    # picture) stand, at 4x from the origin of that shelter's bounding box --
    # the far shelter's is the tile's north corner, the near one's on a road
    # along X lies 13/16 of a tile along y from it, (104, 52) at 4x.
    ("zastavka_x_vzadu", "zastavka_divka_s90_zin4.png", "girl", (-34.4, 34.8), None),
    ("zastavka_y_vzadu", "zastavka_divka_s0_zin4.png", "girl", (70.4, 52.8), None),
    ("zastavka_x_vpredu", "zastavka_divka_s260_zin4.png", "girl", (0 - 104, 120 - 52), None),
]

#: Where a girl's feet are on her picture.
GIRL_FEET = (80, 80)


def load(source: str) -> Image.Image:
    """The render, as RGBA."""
    return Image.open(HERE / source).convert("RGBA")


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
    image = image.copy()
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


def keep(image: Image.Image, mine) -> Image.Image:
    """The image with only the pixels mine(x, y) says belong to the piece."""
    out = Image.new(image.mode, image.size, 0 if image.mode == "P" else (0, 0, 0, 0))
    if image.mode == "P":
        out.putpalette(image.getpalette())
    src, dst = image.load(), out.load()
    for y in range(image.height):
        for x in range(image.width):
            if mine(x, y):
                dst[x, y] = src[x, y]
    return out


def tile_of(x: float, y: float, north: tuple[float, float], scale: float, size: tuple[int, int]) -> tuple[int, int]:
    """The tile of a field a pixel centre lies on, clamped to the field: above the back edges it is the back tile below."""
    dx = (x - north[0]) / (HALF_WIDTH / scale)
    dy = (y - north[1]) / (HALF_HEIGHT / scale)
    tx = math.floor((dy - dx) / 2)
    ty = math.floor((dy + dx) / 2)
    return min(max(tx, 0), size[0] - 1), min(max(ty, 0), size[1] - 1)


def emit(lines: list[str], name: str, big: Image.Image, small: Image.Image, north: tuple[float, float]) -> None:
    """Save a piece at both zooms and write its lines; north is its tile's north corner at 4x."""
    box = big.getbbox()
    sbox = small.getbbox()
    big.crop(box).save(HERE / f"{name}_32bpp.png")
    small.crop(sbox).save(HERE / f"{name}_8bpp.png")
    bx = box[0] - round(north[0]) + NORTH_SHIFT
    by = box[1] - round(north[1])
    sx = sbox[0] - math.floor((north[0] - NORTH_SHIFT) / 4)
    sy = sbox[1] - math.floor(north[1] / 4)
    lines.append(f"   -1 sprites/{name}_8bpp.png 8bpp 0 0 {sbox[2] - sbox[0]} {sbox[3] - sbox[1]} {sx} {sy} normal")
    lines.append(f"    | sprites/{name}_32bpp.png 32bpp 0 0 {box[2] - box[0]} {box[3] - box[1]} {bx} {by} zi4")


def main() -> None:
    lines = []
    for name, source, cut, north, extra in PICTURES:
        image = load(source)
        small = to_8bpp(image)
        if cut == "girl":
            # emit() counts from a tile's north corner; put that where the
            # feet land on the picture less the place they stand on.
            corner = (GIRL_FEET[0] - north[0] + NORTH_SHIFT, GIRL_FEET[1] - north[1])
            emit(lines, name, image, small, corner)
        elif cut == "whole":
            emit(lines, name, image, small, north)
        elif cut == "strips":
            for strip, (left, right), tile_north in extra:
                big = keep(image, lambda x, y: left <= x < right)
                piece = keep(small, lambda x, y: left // 4 <= x < right // 4)
                emit(lines, f"{name}_{strip}", big, piece, tile_north)
        else:
            size = extra
            for ty in range(size[1]):
                for tx in range(size[0]):
                    big = keep(image, lambda x, y: tile_of(x + 0.5, y + 0.5, north, 1, size) == (tx, ty))
                    piece = keep(small, lambda x, y: tile_of(x + 0.5, y + 0.5, (north[0] / 4, north[1] / 4), 4, size) == (tx, ty))
                    tile_north = (north[0] + (ty - tx) * HALF_WIDTH, north[1] + (tx + ty) * HALF_HEIGHT)
                    emit(lines, f"{name}_{tx}_{ty}", big, piece, tile_north)
    print("\n".join(lines))


if __name__ == "__main__":
    main()
