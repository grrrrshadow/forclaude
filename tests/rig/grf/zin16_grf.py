#!/usr/bin/env python3
"""
Writes zin16_test.grf, the rig's set for the 16x level of a sprite (zoom code 7,
ZoomLevel::In16x, LoadSpriteV2() in src/spriteloader/grf.cpp), read by the
scene zin16 with 'testzoom8 sprite <id>'. Container version 2, written here
by hand as budovy_grf.py writes the game's own set, since grfcodec knows no
zoom code past 5; only the standard library, so the battery can run it.

Two sprites, replaced with Action A (SPR_FLAT_GRASS_TILE and the one after
it, 3981 and 3982 of the base set), each a square with a plain colour per
level so the middle pixel the probe reads says which level it came from:

    3981  4x 16 px red, 8x 32 px green, 16x 64 px blue
          -- 16x off: 8x is the set's green, the 16x record stays unread
          (the set has an 8x); 16x on: 16x is the set's blue
    3982  16x only, 64 px, red and blue pixels alternating like a chessboard
          -- 16x off: 8x is made from it, the mean of each 2x2 block,
          (128, 0, 128); the 4x the same; 16x on: 16x is the set's own

    python3 zin16_grf.py <output .grf>
"""

import struct
import sys

GRFID = b"RIG6"
FIRST_SPRITE = 3981  # SPR_FLAT_GRASS_TILE
ZOOM_4X, ZOOM_8X, ZOOM_16X = 1, 6, 7
COLOUR_RGBA = 0x01 | 0x02


def lz77_literal(data: bytes) -> bytes:
    """The GRF sprite LZ77 with literal runs only (int8 code >= 0: that many bytes follow, 0 is 128)."""
    out = bytearray()
    for start in range(0, len(data), 128):
        run = data[start:start + 128]
        out.append(0 if len(run) == 128 else len(run))
        out.extend(run)
    return bytes(out)


def sprite_entry(sprite_id: int, size: int, zoom: int, pixel) -> bytes:
    """One level of a real sprite: a size x size square, pixel(x, y) its RGBA; offsets 0."""
    raw = b"".join(bytes(pixel(x, y)) for y in range(size) for x in range(size))
    body = struct.pack("<BBHHhh", COLOUR_RGBA, zoom, size, size, 0, 0) + lz77_literal(raw)
    return struct.pack("<II", sprite_id, len(body)) + body


def pseudo(payload: bytes) -> bytes:
    return struct.pack("<IB", len(payload), 0xFF) + payload


def real(sprite_id: int) -> bytes:
    return struct.pack("<IBI", 4, 0xFD, sprite_id)


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    red, green, blue = (255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255)
    data = bytearray()
    sprites = bytearray()
    data += pseudo(struct.pack("<I", 5))  # sprite 0: how many sprites follow
    data += pseudo(b"\x08\x08" + GRFID + b"Rig: zin16\x00" + b"Two sprites with a 16x level (zoom code 7)\x00")
    data += pseudo(bytes([0x0A, 0x01, 0x02]) + struct.pack("<H", FIRST_SPRITE))  # Action A: 2 sprites from FIRST_SPRITE
    data += real(1)
    sprites += sprite_entry(1, 16, ZOOM_4X, lambda x, y: red)
    sprites += sprite_entry(1, 32, ZOOM_8X, lambda x, y: green)
    sprites += sprite_entry(1, 64, ZOOM_16X, lambda x, y: blue)
    data += real(2)
    sprites += sprite_entry(2, 64, ZOOM_16X, lambda x, y: red if (x + y) % 2 == 0 else blue)
    data += struct.pack("<I", 0)
    sprites += struct.pack("<I", 0)

    header = b"\x00\x00GRF\x82\x0d\x0a\x1a\x0a"
    compression = b"\x00"
    grf = header + struct.pack("<I", len(compression) + len(data)) + compression + bytes(data) + bytes(sprites)
    with open(sys.argv[1], "wb") as f:
        f.write(grf)
    print(f"{sys.argv[1]}: {len(grf)} bytes, sprites {FIRST_SPRITE} (4x, 8x, 16x) and {FIRST_SPRITE + 1} (16x only)")


if __name__ == "__main__":
    main()
