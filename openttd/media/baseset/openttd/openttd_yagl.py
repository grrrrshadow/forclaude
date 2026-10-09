#!/usr/bin/env python3
"""
Builds openttd.grf, the game's own graphics, with yagl -- the colleague's
yagl, the one tool this game's sets are made with (the player's rule: no
grfcodec). The source is openttd.yagl beside this script, the set as yagl
reads and writes it, with the game's original sprites on the palette sheet
openttd-8bpp-normal-0.png and the pictures of our own -- the school, the
automat, the statue, the plantation, the bus-stop girls, the hut, the
fireworks, the poster -- on sheets of our own, openttd-nase-*.png, laid out
here from the pieces openttd_budovy.py cuts from the colleague's renders
(budovy_manifest.json). Our GUI icons -- the blueprint toolbar, the rescue
engine, the crosshairs, the raid rocket, the cargo icons of the road
vehicles on wagons and of marijuana -- are palette sprites among the
original ones, each drawn by its own script (openttdgui_*.py) into a
picture of its own; their blocks read those pictures (GUI_ICONS below), so
an icon drawn again goes into the set as it is.

This script lays the pieces out on those sheets, each cut down to the
pixels it has (rows and columns with no alpha at all), 8 px apart as the
colleague's packers do, and writes them into
openttd.yagl: for every entry of the manifest, the block of the OpenTTD GUI
sprite it stands for (SPR_OPENTTD_BASE + 231 and on, the order of the
manifest) is made anew from the finest render there is -- the 16x piece
(zin16) where the colleague renders one, else the 8x piece (zin8), else the
4x piece (zin4, the poster). 32bpp only: the player's word is that we make
no palette pictures, as the colleague's sets have none. The levels not in
the file the game works out by scaling (ResizeSprites() in
src/spritecache.cpp), 4x from 8x among them: the renders go in as they are,
never scaled down here. The one exception is the poster, which the game
draws at 2x or normal on most screens and would scale by taking one pixel
of four: its 2x and normal levels are made by openttd_budovy.py, the mean
of 2x2 blocks, and go in beside the 4x. Then it runs yagl and writes openttd.grf.hash, the
checksum the game lists the set by, as Baseset.cmake reads it.

    python3 openttd_yagl.py              sheets, openttd.yagl, openttd.grf and its hash
    python3 openttd_yagl.py --jen-yagl   sheets and openttd.yagl only

yagl is looked for on the path, or at $YAGL. It is built from the colleague's
copy (grrrrf, yagl/yagl-main, his yagl/POSTUP.md: cmake -G Ninja .. && ninja).
"""
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
BASESET = HERE.parent
YAGL_FILE = HERE / "openttd.yagl"
MANIFEST = HERE / "budovy_manifest.json"
GRF = BASESET / "openttd.grf"
FIRST_GUI_SPRITE = 231  # SPR_OPENTTD_BASE + 231: the first picture of our own, the order of the manifest
GAP = 8  # pixels between pieces on a sheet, and around them
SHEET_WIDTH = 2048

#: The levels a manifest entry may hold, the sheet each goes on and how yagl names the level.
LEVELS = (("x16", "openttd-nase-zin16.png", "zin16", "c32bpp | chunked"),
          ("x8", "openttd-nase-zin8.png", "zin8", "c32bpp | chunked"),
          ("x4", "openttd-nase-zin4.png", "zin4", "c32bpp | chunked"),
          ("x2", "openttd-nase-zin2.png", "zin2", "c32bpp | chunked"),
          ("x1", "openttd-nase-normal.png", "normal", "c32bpp | chunked"))

#: Our GUI icons, palette sprites drawn by the openttdgui_*.py scripts: the
#: OpenTTD GUI sprite, then the picture, where the sprite sits on it, its size
#: and its offsets (openttdgui_rocket.py prints its own rows).
GUI_ICONS = (
    *[(0x00C0 + i, "openttdgui_blueprint.png", i * 24 + 2, 2, 20, 20, 0, 0) for i in range(16)],
    (0x00D0, "openttdgui_rescue.png", 2, 2, 16, 16, 0, 0),
    (0x00D2, "openttdgui_crosshair.png", 2, 2, 16, 16, 0, 0),
    (0x00D3, "openttdgui_crosshair_cursor.png", 2, 2, 32, 32, -16, -16),
    *[(0x00D4 + 8 * s + h, f"openttdgui_rocket_{s}_{h}.png", 2, 2, *size)
      for s in range(2) for h, size in enumerate([(7, 13, -3, -6), (19, 10, -10, -4), (25, 5, -12, -2), (19, 10, -10, -5),
                                                  (7, 13, -3, -6), (19, 10, -8, -5), (25, 5, -12, -2), (19, 10, -8, -4)])],
    (0x00E4, "openttdgui_cargo_rola.png", 2, 2, 10, 10, 0, 0),
    (0x00E5, "openttdgui_cargo_marijuana.png", 2, 2, 11, 11, 0, 0),
    (0x00E6, "openttdgui_crosshair_armed.png", 2, 2, 16, 16, 0, 0),
)


def wanted(entry: dict) -> list[str]:
    """Which of an entry's levels go into the set: the finest render there is, and the 2x and
    normal levels where the manifest has them made (the poster, openttd_budovy.py), nothing else."""
    for key in ("x16", "x8", "x4"):
        if entry.get(key): return [key] + [k for k in ("x2", "x1") if entry.get(k)]
    return []


def lay_out(pieces: list[tuple[int, Image.Image]]) -> tuple[dict[int, tuple[int, int]], int]:
    """Rows of pieces GAP apart, as the colleague's packers lay a sheet out: (index -> (x, y)), height."""
    x = y = GAP
    row = 0
    at = {}
    for index, image in pieces:
        if x + image.width + GAP > SHEET_WIDTH:
            x = GAP
            y += row + GAP
            row = 0
        at[index] = (x, y)
        x += image.width + GAP
        row = max(row, image.height)
    return at, y + row + GAP


def cut_down(piece: dict) -> tuple[Image.Image, int, int, int, int]:
    """The piece without its see-through margin: image, width, height, and its offsets moved by the cut.

    Only rows and columns with no alpha at all go, as grfcodec cut them: a faint
    shadow or glow to the edge stays, the picture is the colleague's and is not
    thinned here. His renders mostly reach the edge that way, so most pieces
    keep their size.
    """
    image = Image.open(HERE / piece["file"]).convert("RGBA")
    box = image.getchannel("A").getbbox()
    if box is None: box = (0, 0, 1, 1)
    left, top, right, bottom = box
    return image.crop(box), right - left, bottom - top, piece["x"] + left, piece["y"] + top


def make_sheets(manifest: list[dict]) -> dict[tuple[int, str], tuple[str, int, int, int, int, int, int]]:
    """Write the sheets of our own; returns (entry index, level) -> (sheet, x, y, width, height, x offset, y offset)."""
    where = {}
    for key, sheet, _, _ in LEVELS:
        pieces = []
        geometry = {}
        for index, entry in enumerate(manifest):
            if key not in wanted(entry): continue
            image, w, h, xo, yo = cut_down(entry[key])
            pieces.append((index, image))
            geometry[index] = (w, h, xo, yo)
        if not pieces:
            (HERE / sheet).unlink(missing_ok=True)
            continue
        at, height = lay_out(pieces)
        out = Image.new("RGBA", (SHEET_WIDTH, height), (0, 0, 0, 0))
        for index, image in pieces:
            out.paste(image, at[index])
            where[(index, key)] = (sheet, *at[index], *geometry[index])
        out.save(HERE / sheet)
        print(f"{sheet}: {len(pieces)} pieces, {SHEET_WIDTH} x {height}")
    return where


def block_lines(index: int, entry: dict, where: dict) -> list[str]:
    lines = []
    for key, _, zoom, depth in LEVELS:
        if key not in wanted(entry): continue
        sheet, x, y, w, h, xo, yo = where[(index, key)]
        lines.append(f'        [{w}, {h}, {xo}, {yo}], {zoom}, {depth}, "{sheet}", [{x}, {y}];')
    return lines


def rewrite(text: str, manifest: list[dict], where: dict) -> str:
    """Put the manifest's pieces into the yagl: the block of each of our sprites made anew."""
    out = text
    for index, entry in enumerate(manifest):
        number = FIRST_GUI_SPRITE + index
        head = f"    // Replace OpenTTDGUI sprite 0x{number:04X}\n"
        at = out.find(head)
        if at < 0:
            sys.exit(f"openttd.yagl has no OpenTTD GUI sprite 0x{number:04X} for {entry['name']}")
        open_brace = out.index("    {\n", at)
        close_brace = out.index("    }\n", open_brace)
        body = "\n".join(block_lines(index, entry, where)) + "\n"
        out = out[:open_brace + len("    {\n")] + body + out[close_brace:]
    return out


def rewrite_icons(text: str) -> str:
    """Point the block of each of our GUI icons at the picture its script draws."""
    out = text
    for number, picture, x, y, w, h, xo, yo in GUI_ICONS:
        head = f"    // Replace OpenTTDGUI sprite 0x{number:04X}\n"
        at = out.find(head)
        if at < 0:
            sys.exit(f"openttd.yagl has no OpenTTD GUI sprite 0x{number:04X} for {picture}")
        line_at = out.index("        [", at)
        line_end = out.index("\n", line_at)
        line = out[line_at:line_end]
        expected = f"        [{w}, {h}, {xo}, {yo}], normal, c8bpp, "
        if not line.startswith(expected):
            sys.exit(f"0x{number:04X}: the block says {line.strip()!r}, the icon {picture} is {w}x{h} at {xo},{yo}")
        out = out[:line_at] + f'{expected}"{picture}", [{x}, {y}];' + out[line_end:]
    return out


def main() -> None:
    manifest = json.loads(MANIFEST.read_text())
    where = make_sheets(manifest)
    text = YAGL_FILE.read_text()
    new = rewrite_icons(rewrite(text, manifest, where))
    if new != text:
        YAGL_FILE.write_text(new)
        print(f"openttd.yagl: {len(manifest)} sprites of our own written in")
    else:
        print("openttd.yagl: unchanged")
    if "--jen-yagl" in sys.argv:
        return

    yagl = os.environ.get("YAGL") or shutil.which("yagl")
    if yagl is None:
        sys.exit("yagl not found: put it on the path or in $YAGL (built from grrrrf yagl/yagl-main)")
    # yagl reads <dir>/openttd.yagl for openttd.grf beside <dir>, and the
    # sheets relative to <dir>; --best keeps the smaller encoding of each sprite.
    subprocess.run([yagl, "-e", "-b", GRF.name, HERE.name], cwd=BASESET, check=True)
    md5 = subprocess.run([yagl, "-m", GRF.name], cwd=BASESET, check=True, capture_output=True, text=True).stdout.strip().splitlines()[-1]
    (BASESET / "openttd.grf.hash").write_text(md5.lower() + "\n")
    print(f"openttd.grf: {GRF.stat().st_size} bytes, md5 {md5.lower()}")


if __name__ == "__main__":
    main()
