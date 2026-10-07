#!/usr/bin/env python3
"""Generate the sprites of the buildings of the game's own industries.

The girls' grammar school, the vending machine, the statue of Karel Macha
and the marijuana plantation are industries of the game's own
(economy.extra_industries), and the girls at a bus stop go with them; all
are drawn from the base set, since an industry of the game cannot reach a
NewGRF's sprites. The artwork is the player's, rendered in Blender at 4x
zoom (*_zin4.png) and, where there is one, at 8x (*_zin8.png, exactly twice
the 4x picture; the scripts are in the player's graphics repository), each
with a second picture: the school, the machine and the statue with girls
about them; the plantation comes as single tiles laid over one another.

How a picture is cut:

- whole: one tile, the picture is the tile's sprite (machine, statue).
- girl: a girl at a bus stop, the picture is the sprite, placed by her feet.
- strips: the school on 2x2 tiles, in vertical strips along the tile
  columns, one per tile: the west tile takes the column left of the north
  and south tiles, the east tile the one right of them, and the south tile
  -- the front one, drawn last -- the middle column with whatever of the
  back tile stands in it. The north tile has no sprite of its own. The hut
  of the coffeeshop on 2x1 tiles likewise: the front tile the column left
  of the back tile's north corner, the back tile the one right of it.
- tiles: a field on several tiles in one picture, cut along the tiles'
  diamonds: a pixel belongs to the tile whose diamond it lies in, and one
  above the back edges of the field to the back tile below it. Nothing is
  drawn twice, so the pieces meet without a seam whatever order they are
  drawn in. (The plantation was cut so once; it is laid from single tiles
  now, see below, and nothing uses this cut at present.)
- whole, on the tile frame: the marijuana plantation is laid from pictures
  of one tile each, all in the same frame -- 264 x 200 at 4x, the tile's
  north corner at (132, 64), 64 rows above it for the plants -- and laid
  over one another in the game (industry_cmd.cpp): the soil, the plants on
  it, a girl at work among them; and on the row of the field road, drawn
  from the row behind it over a tile the player keeps, the road, the shed
  at its end and the girls by its edge. Each is cut as a whole picture.

Every sprite comes as 8bpp at normal zoom for a game without a 32bpp
blitter (scaled down and put in the DOS palette, without the shadow, since
the palette has no see-through black), as 32bpp at 4x, and where the render
exists as 32bpp at 8x. The 8bpp and 4x ones go into openttd.grf through
openttdgui.nfo (the lines this script prints); grfcodec knows no 8x, so
all three go into decouple/grafika/budovy.grf as well, which budovy_grf.py
builds from the manifest this script writes (budovy_manifest.json), with
the zoom code 6 of this game for the 8x level.

A tile on the render is 256 x 128 pixels at 4x and 512 x 256 at 8x (2:1),
the game's own grid: tiles lie TILE_PIXELS (32) apart at normal zoom. That
a flat ground sprite is only 31 rows high does not change it -- its 32nd
row is the side corners of the tiles beside it -- and the render is taken
as it is. (Squeezed to 31 rows once, by mistake: the buildings came out
3 % low and their front edges inside their tiles.) The offsets are from
the north corner of the sprite's own tile, which on the render sits on a
pixel boundary; in the game it sits between the first and second pixel of
a flat tile's top row, hence the one pixel (four at 4x, eight at 8x) to the
right. Run from this directory; needs Pillow.
"""

from __future__ import annotations

import json
import math
import pathlib

from PIL import Image

HERE = pathlib.Path(__file__).parent
PALETTE_FROM = HERE / "openttdgui.png"
MANIFEST = HERE / "budovy_manifest.json"

#: Below this alpha a pixel is nothing at normal zoom (out of 255).
OPAQUE_ENOUGH = 128
#: The shadow: dark and see-through; left out of the 8bpp sprites.
SHADOW_ALPHA = 200
SHADOW_DARK = 40
#: Palette indices the 8bpp sprites may use: not 0 (nothing), not the company
#: colours, not the animated ones at the top.
USABLE = [i for i in range(1, 0xE3) if not 0xC6 <= i <= 0xCD]

#: The pictures, in the order of openttdgui.nfo (and of SPR_GYMNASIUM_WEST
#: and the rest in table/sprites.h): output name, 4x source, how it is cut,
#: the north corner of the picture's north tile on the 4x render, and for
#: strips the x ranges of the strips with the north corner of each strip's
#: tile, for tiles the field's size in tiles (x, y), for a girl where her
#: feet stand from the origin of the shelter's bounding box, at 4x. The 8x
#: source, when there is one, is the 4x name with zin8 for zin4, and all
#: its measures are twice these.
SCHOOL_STRIPS = [("w", (104, 232), (232, 296)), ("s", (232, 488), (360, 360)), ("e", (488, 616), (488, 296))]
#: The hut (the coffeeshop) on 2x1 tiles, two along x: the front tile -- the
#: yard, drawn last -- takes the column left of the back tile's north corner
#: with what of the hut stands in it, the back tile the column right of it.
#: The girls stand and sit in the front column only (checked by main()).
HUT_STRIPS = [("predni", (8, 264), (136, 184)), ("zadni", (264, 392), (264, 120))]
#: Pictures that hold only what is laid over another picture (the girls at
#: the hut): every pixel of them must fall in the strips they are cut in, or
#: it would be lost. (The school with girls is cut in fewer strips than the
#: school on purpose: its east column is the same with girls or without.)
LAYERS = {"chatka_stoji", "chatka_sedi"}
#: The girls at work on the plantation, in the order of their sprites.
PLANTATION_WORKERS = ["real", "sedi", "punk", "pubg", "char16", "chill"]
PICTURES = [
    ("gymnazium", "gymnazium_zin4.png", "strips", (360, 232), SCHOOL_STRIPS),
    ("automat", "automat_zin4.png", "whole", (192, 128), None),
    ("gymnazium_holky", "gymnazium_postavy_zin4.png", "strips", (360, 232), SCHOOL_STRIPS[:2]),
    ("automat_holky", "automat_postavy_zin4.png", "whole", (192, 128), None),
    ("socha_kamen", "socha_kamen_zin4.png", "whole", (192, 128), None),
    ("socha_kamen_holky", "socha_kamen_postavy_zin4.png", "whole", (192, 128), None),
    ("socha_bronz", "socha_bronz_zin4.png", "whole", (192, 128), None),
    ("socha_bronz_holky", "socha_bronz_postavy_zin4.png", "whole", (192, 128), None),
    # The marijuana plantation, tile by tile (SPR_MARIJUANA_SOIL and the
    # rest), every picture on the one tile frame: the soil, the field road
    # along x, the shed beside the road's end, the plants small and grown,
    # the girls at the road's north-west and south-east edge, and six girls
    # at work among the plants, each with the small plants and with the
    # grown ones in front of her. Order as in table/sprites.h.
    *[(f"pole_{name}", f"pole_{name}_zin4.png", "whole", (132, 64), None) for name in
        ["mari_zaklad", "cesta", "bouda", "mari_male", "mari_vzrostle", "holky_sz", "holky_jv"]
        + [f"prace_{girl}_f2" for girl in PLANTATION_WORKERS] + [f"prace_{girl}_f3" for girl in PLANTATION_WORKERS]],
    # The girls at a drive-through bus stop (SPR_BUS_STOP_GIRL_X_FAR and the
    # rest), drawn as a child of a shelter: where the feet (the middle of the
    # picture) stand, from the origin of that shelter's bounding box -- the
    # far shelter's is the tile's north corner, the near one's on a road
    # along X lies 13/16 of a tile along y from it, (104, 52) at 4x.
    ("zastavka_x_vzadu", "zastavka_divka_s90_zin4.png", "girl", (-34.4, 34.8), None),
    ("zastavka_y_vzadu", "zastavka_divka_s0_zin4.png", "girl", (70.4, 52.8), None),
    ("zastavka_x_vpredu", "zastavka_divka_s260_zin4.png", "girl", (0 - 104, 120 - 52), None),
    # The hut of the coffeeshop (SPR_HUT_FRONT and the rest), and the girls
    # laid over its front tile: standing after studentky came, sitting when
    # marijuana came to them as well. A layer holds only the girls.
    ("chatka", "chatka_zin4.png", "strips", (264, 120), HUT_STRIPS),
    ("chatka_stoji", "chatka_stojici_zin4.png", "strips", (264, 120), HUT_STRIPS[:1]),
    ("chatka_sedi", "chatka_sedici_zin4.png", "strips", (264, 120), HUT_STRIPS[:1]),
    # The haystack by the plantation's field road, across from the shed, on
    # the tile frame (SPR_MARIJUANA_HAY): with the shed it hides the player's
    # stop at the road's end.
    ("pole_seno", "pole_seno_zin4.png", "whole", (132, 64), None),
    # Matylda at a drive-through bus stop on a road along Y, by the near
    # edge, looking up the road for the bus (SPR_BUS_STOP_GIRL_Y_NEAR): a
    # child of the far shelter like the other girl along Y, whose box starts
    # at the tile's north corner, so the near shelter is drawn after her --
    # the original one hides her, CZTR's leaves her on the open pavement.
    # Model "Matilda" by nicolekeane, CC BY-NC-SA 4.0 (see CREDITS.md).
    ("zastavka_y_vpredu", "zastavka_divka_matylda_s270_zin4.png", "girl", (-67.2, 73.6), None),
    # The fireworks over the coffeeshop's hut (SPR_FIREWORK_START and the
    # rest), laid over its front tile like the girls: the yellow launch, then
    # the ball of sparks in three phases, each in green, blue and red. The
    # colleague drew them at 8x only (FIREWORK_ONLY_8X), every one on the
    # same 640 x 960 frame with the launch point on the ground at (320, 944);
    # it stands in the middle of the hut's yard, half a tile (64 px at 4x)
    # below the front tile's north corner.
    ("ohnostroj_start", "ohnostroj_start_zin4.png", "whole", (160, 472 - 64), None),
    *[(f"ohnostroj_{colour}_{phase}", f"ohnostroj_{colour}_{phase}_zin4.png", "whole", (160, 472 - 64), None)
        for colour in ("zelena", "modra", "cervena") for phase in (1, 2, 3)],
    # The poster that drives in at every start of the game (SPR_START_POSTER,
    # main_gui.cpp): the colleague's orange Tatra 148 with marijuana, its
    # half-size render (1533 x 1043, the player's word: "vezmem ten menší")
    # taken as the 4x level. Not a tile: its offsets are its top left corner.
    ("plakat_tatra148", "plakat_tatra148_zin4.png", "poster", None, None),
]


class Zoom:
    """The measures of one zoom: 4x or 8x."""

    def __init__(self, factor: int):
        self.factor = factor
        self.half_width = 32 * factor
        self.half_height = 16 * factor
        self.north_shift = factor
        self.scale = factor / 4  # of the 4x measures

    def of(self, value):
        """A 4x measure (a number or a pair) at this zoom."""
        if isinstance(value, (tuple, list)):
            return tuple(v * self.scale for v in value)
        return value * self.scale


ZOOM4 = Zoom(4)
ZOOM8 = Zoom(8)


def load(source: str) -> Image.Image | None:
    """The render, as RGBA, or None when there is none."""
    path = HERE / source
    if not path.exists():
        return None
    return Image.open(path).convert("RGBA")


def half_of(image: Image.Image) -> Image.Image:
    """An 8x render at 4x: the mean of each 2x2 block, weighted by alpha, as the game makes a missing 4x (ResizeSpriteOut())."""
    w, h = image.width // 2, image.height // 2
    src = image.load()
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    dst = out.load()
    for y in range(h):
        for x in range(w):
            block = [src[2 * x + dx, 2 * y + dy] for dy in (0, 1) for dx in (0, 1)]
            sa = sum(p[3] for p in block)
            if sa == 0:
                continue
            dst[x, y] = tuple((sum(p[c] * p[3] for p in block) + sa // 2) // sa for c in range(3)) + ((sa + 2) // 4,)
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


def tile_of(x: float, y: float, north: tuple[float, float], half_width: float, half_height: float, size: tuple[int, int]) -> tuple[int, int]:
    """The tile of a field a pixel centre lies on, clamped to the field: above the back edges it is the back tile below."""
    dx = (x - north[0]) / half_width
    dy = (y - north[1]) / half_height
    tx = math.floor((dy - dx) / 2)
    ty = math.floor((dy + dx) / 2)
    return min(max(tx, 0), size[0] - 1), min(max(ty, 0), size[1] - 1)


def piece(image: Image.Image, cut: str, extra, zoom: Zoom, north, which) -> Image.Image:
    """One piece of a picture at the given zoom: which is the strip or the tile."""
    if cut in ("whole", "girl"):
        return image
    if cut == "strips":
        left, right = zoom.of(which[1])
        return keep(image, lambda x, y: left <= x < right)
    n = zoom.of(north)
    return keep(image, lambda x, y: tile_of(x + 0.5, y + 0.5, n, zoom.half_width, zoom.half_height, extra) == which)


def pal_piece(small: Image.Image, cut: str, extra, north, which) -> Image.Image:
    """One piece of the 8bpp picture, cut after scaling so the pieces meet without a seam."""
    if cut in ("whole", "girl"):
        return small
    if cut == "strips":
        left, right = which[1]
        return keep(small, lambda x, y: left // 4 <= x < right // 4)
    n = (north[0] / 4, north[1] / 4)
    return keep(small, lambda x, y: tile_of(x + 0.5, y + 0.5, n, 32, 16, extra) == which)


def cropped(image: Image.Image, north: tuple[float, float], zoom: Zoom, name: str) -> dict:
    """Save a piece cropped to its pixels and give its file and offsets from its tile's north corner."""
    box = image.getbbox()
    if box is None:
        raise SystemExit(f"{name}: nothing in this piece")
    image.crop(box).save(HERE / name)
    return {"file": name, "w": box[2] - box[0], "h": box[3] - box[1], "x": box[0] - round(north[0]) + zoom.north_shift, "y": box[1] - round(north[1])}


def emit(lines: list[str], manifest: list[dict], name: str, big4: Image.Image, small: Image.Image, big8: Image.Image | None, north4) -> None:
    """Save a sprite's pieces at every zoom and write its nfo lines and manifest entry; north4 is its tile's north corner at 4x."""
    x4 = cropped(big4, north4, ZOOM4, f"{name}_32bpp.png")
    sbox = small.getbbox()
    small.crop(sbox).save(HERE / f"{name}_8bpp.png")
    pal = {"file": f"{name}_8bpp.png", "w": sbox[2] - sbox[0], "h": sbox[3] - sbox[1],
            "x": sbox[0] - math.floor((north4[0] - ZOOM4.north_shift) / 4), "y": sbox[1] - math.floor(north4[1] / 4)}
    x8 = cropped(big8, ZOOM8.of(north4), ZOOM8, f"{name}_8x_32bpp.png") if big8 is not None else None
    lines.append(f"   -1 sprites/{pal['file']} 8bpp 0 0 {pal['w']} {pal['h']} {pal['x']} {pal['y']} normal")
    lines.append(f"    | sprites/{x4['file']} 32bpp 0 0 {x4['w']} {x4['h']} {x4['x']} {x4['y']} zi4")
    manifest.append({"name": name, "pal": pal, "x4": x4, "x8": x8})


def main() -> None:
    lines = []
    manifest = []
    for name, source, cut, north, extra in PICTURES:
        image4 = load(source)
        image8 = load(source.replace("zin4", "zin8"))
        if image4 is None and image8 is not None:
            # Drawn at 8x only (the fireworks): the 4x and the 8bpp
            # sprites are made from it here.
            image4 = half_of(image8)
        small = to_8bpp(image4)
        if cut == "girl":
            # emit() counts from a tile's north corner; put that where the
            # feet land on the picture less the place they stand on. The feet
            # are the middle of the picture at either zoom.
            feet = (image4.width / 2, image4.height / 2)
            corner = (feet[0] - north[0] + ZOOM4.north_shift, feet[1] - north[1])
            emit(lines, manifest, name, image4, small, image8, corner)
        elif cut == "whole":
            emit(lines, manifest, name, image4, small, image8, north)
        elif cut == "poster":
            # Offsets from the picture's top left corner: emit() adds the
            # north corner's shift, so the corner given is that shift.
            emit(lines, manifest, name, image4, small, image8, (ZOOM4.north_shift, 0))
        elif cut == "strips":
            # Nothing of a layer may be left outside the strips cut: a layer
            # cut in fewer strips than its picture would lose what stands
            # elsewhere.
            for image, zoom in ((image4, ZOOM4), (image8, ZOOM8)):
                if name not in LAYERS or image is None:
                    continue
                spans = [zoom.of(strip[1]) for strip in extra]
                outside = keep(image, lambda x, y: not any(left <= x < right for left, right in spans)).getchannel("A").point(lambda a: 255 if a > 16 else 0).getbbox()
                if outside is not None:
                    raise SystemExit(f"{name}: pixels outside the strips at {zoom.factor}x: {outside}")
            for strip in extra:
                emit(lines, manifest, f"{name}_{strip[0]}", piece(image4, cut, extra, ZOOM4, north, strip), pal_piece(small, cut, extra, north, strip),
                     piece(image8, cut, extra, ZOOM8, north, strip) if image8 is not None else None, strip[2])
        else:
            size = extra
            for ty in range(size[1]):
                for tx in range(size[0]):
                    tile_north = (north[0] + (ty - tx) * ZOOM4.half_width, north[1] + (tx + ty) * ZOOM4.half_height)
                    emit(lines, manifest, f"{name}_{tx}_{ty}", piece(image4, cut, extra, ZOOM4, north, (tx, ty)), pal_piece(small, cut, extra, north, (tx, ty)),
                         piece(image8, cut, extra, ZOOM8, north, (tx, ty)) if image8 is not None else None, tile_north)
    MANIFEST.write_text(json.dumps(manifest, indent=1) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
