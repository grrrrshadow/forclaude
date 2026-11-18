#!/usr/bin/env python3
"""
Porost koleje (the player, 18. 11., topics 374 and 379: a track no trains go on grows over, "first grass, then grass
and tall marijuana"; on the turned map the grass over the stones): patches of grass lying on our track's embankment
(koleje.py) that the game lays over a piece of an overgrown track on the turned map (DrawThreeDTrack() of rail_cmd.cpp),
by the piece and the stage (GetTrackOvergrowthStage()). Written beside this script:

  kolej_porost_rovna_1.glb  over a straight piece (kolej_rovna_*): patches over about half of its embankment, between
                            the sleepers
  kolej_porost_rovna_2.glb  the same thick (the stage with the marijuana): over most of it, over the sleepers too
  kolej_porost_sikma_1.glb  over a diagonal piece (kolej_sikma_*), cut along the tile's edges as that is
  kolej_porost_sikma_2.glb
  kolej_porost_svah_1.glb   over a piece up a slope of the map (kolej_svah_*), a level (3 m) higher at its +X end
  kolej_porost_svah_2.glb

As koleje.py: glTF's Y up, along +X, centred on the origin, its foot at Y 0 on the ground at its middle. The patches
1.5 cm over the embankment's stones (thick, over the sleepers' tops) in three bands along it: between the rails and outside them to a little past the
embankment's edges -- the rails themselves bare, the narrow track's and the wide one's (their rails 0.72 and 0.86 m from
the middle). Each patch an irregular round of 6 to 9 corners, 0.3 to 0.9 m across. One small picture of grass for all
of them, taken from our grass (trava_mirna.png: its middle 128 px made 64), each patch its own piece of it; every u and
v within 0..1 (a texture is not repeated on the card).

    python3 porost.py
"""
import io
import math
import os
import random
import sys
import zlib

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import koleje as K  # noqa: E402 (beside this script)

OVER = {1: 0.015, 2: K.SLEEPER_OVER + 0.015}  # the patches over the stones; thick, over the sleepers too
BANDS = ((-0.42, 0.42), (1.08, 1.9), (-1.9, -1.08))  # across the track (Z): the middle, and outside each rail
COVER = {1: 0.5, 2: 0.85}                    # of the bands' area covered, by the stage
RADIUS = (0.15, 0.45)                         # a patch's half across
TEX_M = 2.0                                   # the picture's side, metres of ground


def grass_png():
    """Our grass's picture, its middle 128 px made 64 (about 3 cm a texel over TEX_M)."""
    im = Image.open(os.path.join(HERE, 'trava_mirna.png')).convert('RGB')
    w, h = im.size
    im = im.crop((w // 2 - 64, h // 2 - 64, w // 2 + 64, h // 2 + 64)).resize((64, 64), Image.LANCZOS)
    out = io.BytesIO()
    im.save(out, 'PNG', optimize=True)
    return out.getvalue()


def patches(length, climb, stage, seed):
    """The patches over a piece `length` long along X, `climb` higher at its +X end: a mesh of one material, 'trava'."""
    rnd = random.Random(seed)
    m = K.Mesh()
    pos, nrm, uv, idx = m.prims.setdefault('trava', ([], [], [], []))
    ln = math.hypot(climb / length, 1.0)
    normal = (-climb / length / ln, 1.0 / ln, 0.0)

    def y(x):
        return K.BED_H + OVER[stage] + climb * x / length
    area = sum(b - a for a, b in BANDS) * length
    mean = math.pi * ((RADIUS[0] + RADIUS[1]) / 2) ** 2
    count = int(COVER[stage] * area / mean)
    widths = [b - a for a, b in BANDS]
    for _ in range(count):
        # A band by its width, a place in it; its round no wider than the band.
        a, b = rnd.choices(BANDS, weights=widths)[0]
        cz = rnd.uniform(a, b)
        cx = rnd.uniform(-length / 2, length / 2)
        r = min(rnd.uniform(*RADIUS), (b - a) / 2)
        cz = min(max(cz, a + r), b - r)
        corners = rnd.randint(6, 9)
        turn = rnd.uniform(0, 2 * math.pi)
        u0, v0 = rnd.uniform(0.25, 0.75), rnd.uniform(0.25, 0.75)
        ring = []
        for k in range(corners):
            ang = turn + 2 * math.pi * k / corners
            rr = r * rnd.uniform(0.7, 1.2)
            x, z = cx + rr * math.cos(ang), cz + rr * math.sin(ang)
            x = min(max(x, -length / 2), length / 2)
            ring.append((x, z))
        base = len(pos)
        pos.append((cx, y(cx), cz))
        nrm.append(normal)
        uv.append((u0, v0))
        for x, z in ring:
            pos.append((x, y(x), z))
            nrm.append(normal)
            uv.append((min(max(u0 + (x - cx) / TEX_M, 0.0), 1.0), min(max(v0 + (z - cz) / TEX_M, 0.0), 1.0)))
        for k in range(corners):
            i0, i1 = base + 1 + k, base + 1 + (k + 1) % corners
            # Seen from above (+Y) counter-clockwise: the face up.
            ax, az = pos[i0][0] - cx, pos[i0][2] - cz
            bx, bz = pos[i1][0] - cx, pos[i1][2] - cz
            if az * bx - ax * bz >= 0:
                idx += [base, i0, i1]
            else:
                idx += [base, i1, i0]
    return m


def main():
    pictures = {'trava': grass_png()}
    for stage in (1, 2):
        for name, length, climb in (('rovna', K.TILE_M, 0.0), ('svah', K.TILE_M, K.LEVEL_MAP_M)):
            full = f'kolej_porost_{name}_{stage}'
            K.write(os.path.join(HERE, full + '.glb'), patches(length, climb, stage, zlib.crc32(full.encode())), full, pictures)
        # The diagonal piece as koleje.py's: made longer by the embankment's width, cut back along the tile's edges.
        longer = K.DIAGONAL_M + K.BED_W
        full = f'kolej_porost_sikma_{stage}'
        m = patches(longer, 0.0, stage, zlib.crc32(full.encode()))
        K.clip(m, 1.0, 1.0, K.DIAGONAL_M / 2)
        K.clip(m, -1.0, 1.0, K.DIAGONAL_M / 2)
        K.write(os.path.join(HERE, full + '.glb'), m, full, pictures)


if __name__ == '__main__':
    main()
