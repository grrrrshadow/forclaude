#!/usr/bin/env python3
"""
Writes the game's own set of building graphics, budovy.grf, from the pieces
openttd_budovy.py cut (budovy_manifest.json beside this script).

Why a set of its own and not openttd.grf: grfcodec, which builds openttd.grf
from the nfo, knows the zoom levels normal, zi2, zi4 and the zoomed-out ones,
but not 8x (zoom code 6, ZoomLevel::In8x in the game). So the 8x renders go
into a GRF container version 2 written here by hand, which the game loads as a
static set of its own in every game (baseset/decouple/grafika/, see
AppendStaticGRFConfigs() in src/newgrf_config.cpp).

The set replaces OpenTTD GUI sprites openttd.grf has for these buildings
(Action 5, type 0x15 with offset, one per run of consecutive sprites from
SPR_OPENTTD_BASE + 231 on). A sprite is replaced whole, all its levels, so
only the sprites the colleague rendered at 8x are here, and each of them
three times: the palette one for the 8bpp blitter at normal zoom, the 32bpp
one at 4x and the 32bpp one at 8x. The others (the plantation fields) stay in
openttd.grf alone. Levels not in the file the game makes by scaling, as it
does for openttd.grf.

Format, as the game reads it (src/spriteloader/grf.cpp, src/spritecache.cpp
ReadGRFSpriteOffsets()):

    header     00 00 'G' 'R' 'F' 82 0D 0A 1A 0A, uint32 offset of the sprite
               section counted from after that uint32, uint8 compression (0)
    data       entries of uint32 size, uint8 type: FF = pseudo-sprite with
               size bytes of payload, FD = real sprite, 4 bytes holding the
               uint32 id of its sprite section entries; a uint32 0 ends it
    sprites    entries of uint32 id, uint32 size (from the colour byte on),
               uint8 colour (01 RGB, 02 alpha, 04 palette), uint8 zoom
               (0 normal, 1 4x, 2 2x, 3..5 zoomed out, 6 8x), uint16 height,
               uint16 width, int16 x offset, int16 y offset, LZ77 data; one
               entry per level, the same id for all of them; a uint32 0 ends it

LZ77: int8 code >= 0 means that many literal bytes follow (0 means 128);
code < 0 copies -(code >> 3) bytes (1..16) from ((code & 7) << 8 | next byte)
bytes back (1..2047). The decoder must land exactly on the pixel count.

    python3 budovy_grf.py
    python3 budovy_grf.py --bez-4x <cesta>   the same set without its 4x
                                             sprites, to see the 4x the game
                                             makes from 8x (ResizeSpriteOut())
"""

import json
import struct
import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "budovy_manifest.json"
OUT = HERE.parent / "decouple" / "grafika" / "budovy.grf"

GRFID = b"DCP8"
NAME = "Decouple: budovy (gymnázium, automat, socha, pole, holky)"
DESCRIPTION = ("Grafika budov hry: 8bpp, 4x a 8x. Vestavěný statický GRF hry, "
               "sprity OpenTTD GUI od 231. Píše budovy_grf.py z obrázků kolegy.")
FIRST_GUI_SPRITE = 231  # SPR_OPENTTD_BASE + 231: the first of the buildings in openttdgui.nfo

ZOOM_NORMAL, ZOOM_4X, ZOOM_8X = 0, 1, 6
COLOUR_RGBA, COLOUR_PALETTE = 0x01 | 0x02, 0x04


def lz77(data: bytes) -> bytes:
    """Greedy LZ77 in the GRF sprite flavour: matches of 3..16 bytes up to 2047 back."""
    out = bytearray()
    literal = bytearray()
    heads: dict[bytes, list[int]] = {}
    n = len(data)
    pos = 0

    def flush_literal() -> None:
        while literal:
            run = literal[:128]
            del literal[:128]
            out.append(0 if len(run) == 128 else len(run))
            out.extend(run)

    while pos < n:
        best_len = 0
        best_dist = 0
        if pos + 3 <= n:
            key = data[pos:pos + 3]
            candidates = heads.get(key)
            if candidates:
                limit = min(16, n - pos)
                for cand in reversed(candidates):
                    dist = pos - cand
                    if dist > 2047:
                        break
                    length = 3
                    while length < limit and data[cand + length] == data[pos + length]:
                        length += 1
                    if length > best_len:
                        best_len, best_dist = length, dist
                        if length == limit:
                            break
        if best_len >= 3:
            flush_literal()
            out.append(((-best_len) << 3 | (best_dist >> 8)) & 0xFF)
            out.append(best_dist & 0xFF)
            step = best_len
        else:
            literal.append(data[pos])
            step = 1
        for p in range(pos, pos + step):
            if p + 3 <= n:
                lst = heads.setdefault(data[p:p + 3], [])
                lst.append(p)
                if len(lst) > 24:
                    del lst[0]
        pos += step
    flush_literal()
    return bytes(out)


def unlz77(code: bytes, size: int) -> bytes:
    """The game's decoder, to check what was written."""
    out = bytearray()
    i = 0
    while len(out) < size:
        c = struct.unpack_from("b", code, i)[0]
        i += 1
        if c >= 0:
            run = 128 if c == 0 else c
            out.extend(code[i:i + run])
            i += run
        else:
            dist = ((c & 7) << 8) | code[i]
            i += 1
            for _ in range(-(c >> 3)):
                out.append(out[-dist])
    assert len(out) == size and i == len(code), (len(out), size, i, len(code))
    return bytes(out)


def pixels(piece: dict, palette: bool) -> bytes:
    image = Image.open(HERE / piece["file"])
    assert image.size == (piece["w"], piece["h"]), (piece, image.size)
    if palette:
        assert image.mode == "P", piece
        return image.tobytes()
    return image.convert("RGBA").tobytes()


def sprite_entry(sprite_id: int, piece: dict, zoom: int, palette: bool) -> bytes:
    raw = pixels(piece, palette)
    code = lz77(raw)
    assert unlz77(code, len(raw)) == raw
    body = struct.pack("<BBHHhh", COLOUR_PALETTE if palette else COLOUR_RGBA, zoom,
                       piece["h"], piece["w"], piece["x"], piece["y"]) + code
    return struct.pack("<II", sprite_id, len(body)) + body


def pseudo(payload: bytes) -> bytes:
    return struct.pack("<IB", len(payload), 0xFF) + payload


def real(sprite_id: int) -> bytes:
    return struct.pack("<IBI", 4, 0xFD, sprite_id)


def action14_palette() -> bytes:
    # Node ids are read as little-endian uint32 and compared to 'INFO' and
    # 'PALS', so they stand reversed in the file. 'D': DOS palette, the one
    # openttd_budovy.py quantises to.
    return b"\x14" + b"C" + b"OFNI" + b"B" + b"SLAP" + struct.pack("<H", 1) + b"D" + b"\x00" + b"\x00"


def action8() -> bytes:
    return b"\x08\x08" + GRFID + NAME.encode("utf-8") + b"\x00" + DESCRIPTION.encode("utf-8") + b"\x00"


def extended_byte(value: int) -> bytes:
    """An NFO extended byte: the value itself under 255, else FF and the value as uint16."""
    assert 0 <= value < 0x10000
    return bytes([value]) if value < 0xFF else b"\xff" + struct.pack("<H", value)


def action5(count: int, offset: int) -> bytes:
    return bytes([0x05, 0x15 | 0x80]) + extended_byte(count) + extended_byte(offset)


def runs_with_8x(entries: list[dict]) -> list[tuple[int, list[dict]]]:
    """Runs of consecutive manifest entries that have an 8x piece, as (first GUI sprite offset, entries)."""
    runs: list[tuple[int, list[dict]]] = []
    for index, entry in enumerate(entries):
        if not entry["x8"]:
            continue
        offset = FIRST_GUI_SPRITE + index
        if runs and runs[-1][0] + len(runs[-1][1]) == offset:
            runs[-1][1].append(entry)
        else:
            runs.append((offset, [entry]))
    return runs


def main() -> None:
    out = OUT
    with_4x = True
    if len(sys.argv) == 3 and sys.argv[1] == "--bez-4x":
        out = Path(sys.argv[2])
        with_4x = False
    elif len(sys.argv) != 1:
        sys.exit(__doc__)
    entries = json.loads(MANIFEST.read_text())
    runs = runs_with_8x(entries)
    count = sum(len(run) for _, run in runs)

    data = bytearray()
    sprites = bytearray()
    data += pseudo(struct.pack("<I", 3 + len(runs) + count))  # sprite 0: how many sprites; the game skips it
    data += pseudo(action14_palette())
    data += pseudo(action8())
    sprite_id = 0
    for offset, run in runs:
        data += pseudo(action5(len(run), offset))
        for entry in run:
            sprite_id += 1
            data += real(sprite_id)
            sprites += sprite_entry(sprite_id, entry["pal"], ZOOM_NORMAL, palette=True)
            if with_4x:
                sprites += sprite_entry(sprite_id, entry["x4"], ZOOM_4X, palette=False)
            sprites += sprite_entry(sprite_id, entry["x8"], ZOOM_8X, palette=False)
    data += struct.pack("<I", 0)
    sprites += struct.pack("<I", 0)

    header = b"\x00\x00GRF\x82\x0d\x0a\x1a\x0a"
    compression = b"\x00"
    grf = header + struct.pack("<I", len(compression) + len(data)) + compression + bytes(data) + bytes(sprites)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(grf)
    where = ", ".join(f"{offset}..{offset + len(run) - 1}" for offset, run in runs)
    print(f"{out}: {count} sprites (OpenTTD GUI {where}), "
          f"each at normal (8bpp), {'4x and ' if with_4x else ''}8x, {len(grf) / 1024 / 1024:.2f} MB")


if __name__ == "__main__":
    main()
