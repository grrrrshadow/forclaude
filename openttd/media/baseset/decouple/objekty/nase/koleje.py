#!/usr/bin/env python3
"""
Koleje (the player, test3: "our own 3D rails"; 8. 11.: "the rails from above on and in 3D on too by default"; "a very
low embankment"; "the embankment one block"; "do it normally, the sleepers over the embankment, a low embankment so one
can walk over it"): the pieces of a track the game lays on a plain rail tile instead of its sprites (DrawThreeDTrack()
of rail_cmd.cpp), our own geometry and our own pictures. Written beside this script:

  kolej_rovna_drevo.glb     a straight piece the tile long, wooden sleepers
  kolej_rovna_beton.glb     the same, concrete sleepers (an electrified track)
  kolej_sikma_drevo.glb     a diagonal piece across a corner of a tile, from the middle of an edge to the middle of the
  kolej_sikma_beton.glb     next (the game's upper, lower, left and right track): the tile's half diagonal long
  kolej_svah_drevo.glb      a straight piece up a slope, a level of the map (3 m) higher at its +X end
  kolej_svah_beton.glb
  kolej_svah_oci_drevo.glb  the same up a level of the view from the eyes (5 m, EYE_LEVEL_STRETCH)
  kolej_svah_oci_beton.glb
  kolej_pul_drevo.glb       a straight piece half the tile long: a depot's, from its door to its middle
  kolej_pul_beton.glb
  kolej_prejezd_drevo.glb   a level crossing's piece: the straight piece the tile long, over it where the road goes
  kolej_prejezd_beton.glb   (9.2 m along the track) concrete panels as high as the rails' heads 4.4 m across the track,
                            the grooves inside the rails' heads in them, a ramp of asphalt down to the road 2.5 m each
                            side (the road vehicles over it lifted so, ThreeDRoadOverTrack())
  kolej_*_siroka.glb        each of them WIDE (1.2) times as wide across, as high and as long: for the vehicles at the
                            original game's size (the player's test9: "make the rails wider, as wide as the original
                            rails when the original size is on ... and the smaller ones narrow"; ThreeDTrackWide())

glTF's Y up. Along +X, centred on the origin along and across (Z); its foot at Y 0 on the ground at its middle (a
slope's half way up). The embankment one block 0.12 m high and 3.4 m across: its top and its two sides, no ends (the next
piece goes on), the stones' picture on it with the sleepers and the rails' lines drawn in it (seen from far, where they
themselves are less than a pixel). The sleepers boxes on it, 6 cm over it (the rest of them in the stones, not made):
2.6 m long, every 0.6 m or so, a whole number to a piece; wooden 0.26 m wide, concrete 0.28 m. The rails two boxes on
them, standard gauge (1435 mm), their heads' tops 0.37 m over the ground (RAIL_TOP_M: the trains raised so much in 3D).
Drawn here, 32 bpp; every u and v within 0..1 (a building's texture is not repeated on the card).

    python3 koleje.py
"""
import io
import json
import math
import os
import random
import struct

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
TILE_M = 14.83765                  # the game's tile, metres (OBJECT_UNITS_PER_METRE)
DIAGONAL_M = TILE_M / math.sqrt(2) # a diagonal piece: half the tile's diagonal
LEVEL_MAP_M = 3.0                  # a level of the map (OBJECT_HEIGHT_PER_METRE)
LEVEL_EYE_M = 5.0                  # a level from the eyes (EYE_LEVEL_STRETCH)
WIDE = 1.2                         # the wide track's: as the vehicles at the original size (ObjectScale())
GAUGE = 1.435
BED_H = 0.12                       # a low embankment, to walk over
BED_W = 3.4
SLEEPER_L = 2.6
SLEEPER_STEP = 0.6
SLEEPER_W = {'drevo': 0.26, 'beton': 0.28}
SLEEPER_OVER = 0.06                # the sleepers' tops over the stones
PLATE = 0.02                       # the plate under a rail
RAIL_W, RAIL_H = 0.072, 0.172      # UIC 60: its head's width, its height
RAIL_TOP_M = BED_H + SLEEPER_OVER + PLATE + RAIL_H
TEX_SIDE = 4                       # the picture's columns: a side, the top, the other side
TEX_TOP = 80                       # across the top: 3.4 m, 4.25 cm a pixel
TEX_ALONG_M = 0.029                # along: about 2.9 cm a pixel


def png(im):
    out = io.BytesIO()
    im.save(out, format='PNG', optimize=True)
    return out.getvalue()


def stone(d, rnd, cx, cy, scale):
    r = rnd.uniform(0.9, 2.0) * scale
    g = rnd.randint(96, 178)
    warm = rnd.randint(-6, 14)
    fill = (g + warm, g + warm // 2, g - warm // 2)
    edge = (int(fill[0] * 0.6), int(fill[1] * 0.6), int(fill[2] * 0.6))
    d.ellipse([cx - r, cy - r * 0.8, cx + r, cy + r * 0.8], fill=fill, outline=edge)


def sleeper_colour(kind, rnd, x, y):
    if kind == 'drevo':
        grain = int(4 * math.sin(y * 0.5 + x * 1.3)) + rnd.randint(-3, 3)
        return (max(0, 64 + grain), max(0, 48 + grain), max(0, 37 + grain))
    n = rnd.randint(-8, 8)
    return (168 + n, 166 + n, 160 + n)


def sleeper_png(kind):
    """A sleeper's own picture, its boxes': u along it, 64 x 16; the wood's grain and cracks, the concrete's ends darker."""
    rnd = random.Random(11 if kind == 'drevo' else 13)
    im = Image.new('RGB', (64, 16))
    for y in range(16):
        tone = rnd.randint(-6, 6) if kind == 'drevo' else 0
        for x in range(64):
            r, g, b = sleeper_colour(kind, rnd, y, x * 0.27)
            if kind == 'drevo':
                crack = -14 if (y == 5 and 10 < x < 40) or (y == 11 and 30 < x < 58) else 0
                r, g, b = r + tone + crack, g + tone + crack, b + tone // 2 + crack
            elif x < 3 or x > 60:
                r, g, b = r - 12, g - 12, b - 12
            im.putpixel((x, y), (max(0, r), max(0, g), max(0, b)))
    return png(im)


def top_png(length, kind):
    """The block's picture: u across (a side, the top, the other side), v along the piece. The stones, the sleepers in
    them (a few stones over their edges: buried), the rails' lines."""
    rnd = random.Random(7 if kind == 'drevo' else 9)
    w = TEX_SIDE * 2 + TEX_TOP
    h = max(32, 32 * round(length / TEX_ALONG_M / 32))
    px_u = TEX_TOP / BED_W           # pixels a metre across
    px_v = h / length                # pixels a metre along
    im = Image.new('RGB', (w, h), (64, 60, 56))
    d = ImageDraw.Draw(im)
    for _ in range(w * h // 5):
        stone(d, rnd, rnd.uniform(-2, w + 2), rnd.uniform(-2, h + 2), 1.0)
    count = max(1, round(length / SLEEPER_STEP))
    step = length / count
    mid = TEX_SIDE + TEX_TOP / 2
    half_l = SLEEPER_L / 2 * px_u
    half_w = SLEEPER_W[kind] / 2 * px_v
    for i in range(count):
        c = ((i + 0.5) * step) * px_v
        y0, y1 = int(round(c - half_w)), int(round(c + half_w))
        x0, x1 = int(round(mid - half_l)), int(round(mid + half_l))
        for y in range(max(0, y0), min(h, y1)):
            for x in range(x0, x1):
                col = sleeper_colour(kind, rnd, x, y)
                if y == y0 or y == y1 - 1:
                    col = tuple(int(v * 0.7) for v in col)
                im.putpixel((x, y), col)
        # Buried: stones over its edges and ends.
        for _ in range(int((x1 - x0) * 0.5)):
            stone(d, rnd, rnd.uniform(x0, x1), rnd.choice((y0, y1)) + rnd.uniform(-0.8, 0.8), 0.8)
        for _ in range(6):
            stone(d, rnd, rnd.choice((x0, x1)) + rnd.uniform(-1.5, 1.5), rnd.uniform(y0, y1), 0.8)
    # The rails' lines (seen from far), their plates under them darker.
    for side in (-1, 1):
        c = mid + side * (GAUGE / 2 + RAIL_W / 2) * px_u
        for y in range(h):
            im.putpixel((int(round(c - 1)), y), (52, 42, 36))
            im.putpixel((int(round(c)), y), (150, 150, 154))
    return png(im)


class Mesh:
    def __init__(self):
        self.prims = {}  # material -> (pos, nrm, uv, idx)

    def quad(self, mat, corners, uvs):
        """A face: 4 corners counter-clockwise seen from outside, their u, v."""
        pos, nrm, uv, idx = self.prims.setdefault(mat, ([], [], [], []))
        a, b, c = corners[0], corners[1], corners[2]
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
        base = len(pos)
        for p, t in zip(corners, uvs):
            pos.append(p)
            nrm.append((nx / ln, ny / ln, nz / ln))
            uv.append(t)
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]


def block(m, mat, x0, x1, y0, y1, z0, z1, rise, uvs):
    """A box along X, x0..x1, heights y0..y1 raised by rise(x), across Z z0..z1: its top and its two long sides; uvs
    the u of its sides' and its top's columns (left side's outer, top from, top to, right side's outer), v 0 at x0."""
    def p(x, y, z):
        return (x, y + rise(x), z)
    sl, t0, t1, sr = uvs
    m.quad(mat, [p(x0, y1, z1), p(x1, y1, z1), p(x1, y1, z0), p(x0, y1, z0)], [(t0, 0), (t0, 1), (t1, 1), (t1, 0)])
    m.quad(mat, [p(x0, y0, z1), p(x1, y0, z1), p(x1, y1, z1), p(x0, y1, z1)], [(sl, 0), (sl, 1), (t0, 1), (t0, 0)])
    m.quad(mat, [p(x1, y0, z0), p(x0, y0, z0), p(x0, y1, z0), p(x1, y1, z0)], [(sr, 1), (sr, 0), (t1, 0), (t1, 1)])


def piece(length, climb, kind):
    """A piece of track along X, `length` long, `climb` metres higher at its +X end than at its -X end."""
    m = Mesh()

    def rise(x):
        return climb * x / length
    x0, x1 = -length / 2, length / 2
    w = TEX_SIDE * 2 + TEX_TOP
    # The top's columns: Z from +W/2 (u at the top's start) to -W/2; half a pixel in from the picture's edges.
    block(m, 'nasep', x0, x1, 0.0, BED_H, -BED_W / 2, BED_W / 2, rise, (0.5 / w, TEX_SIDE / w, (TEX_SIDE + TEX_TOP) / w, (w - 0.5) / w))
    # The sleepers where the picture has them, over the stones: their tops, their long sides, their ends.
    count = max(1, round(length / SLEEPER_STEP))
    step = length / count
    half_w, half_l = SLEEPER_W[kind] / 2, SLEEPER_L / 2
    y0, y1 = BED_H - 0.01, BED_H + SLEEPER_OVER

    def p(x, y, z):
        return (x, y + rise(x), z)
    for i in range(count):
        c = x0 + (i + 0.5) * step
        a, b = c - half_w, c + half_w
        m.quad('prazec', [p(a, y1, half_l), p(b, y1, half_l), p(b, y1, -half_l), p(a, y1, -half_l)], [(0, 0), (0, 1), (1, 1), (1, 0)])
        m.quad('prazec', [p(b, y0, half_l), p(b, y0, -half_l), p(b, y1, -half_l), p(b, y1, half_l)], [(0, 1), (1, 1), (1, 0.75), (0, 0.75)])
        m.quad('prazec', [p(a, y0, -half_l), p(a, y0, half_l), p(a, y1, half_l), p(a, y1, -half_l)], [(1, 1), (0, 1), (0, 0.75), (1, 0.75)])
        m.quad('prazec', [p(a, y0, half_l), p(b, y0, half_l), p(b, y1, half_l), p(a, y1, half_l)], [(0, 1), (0.05, 1), (0.05, 0), (0, 0)])
        m.quad('prazec', [p(b, y0, -half_l), p(a, y0, -half_l), p(a, y1, -half_l), p(b, y1, -half_l)], [(1, 1), (0.95, 1), (0.95, 0), (1, 0)])
    base = BED_H + SLEEPER_OVER + PLATE
    for side in (-1, 1):
        c = side * (GAUGE / 2 + RAIL_W / 2)
        z0, z1 = c - RAIL_W / 2, c + RAIL_W / 2
        block(m, 'kolejnice', x0, x1, base - PLATE, base + RAIL_H, z0, z1, rise, (0, 0, 1, 1))
        # Its head's top worn bright, a hair over.
        y = base + RAIL_H + 0.001
        m.quad('hlava', [(x0, y + rise(x0), z1), (x1, y + rise(x1), z1), (x1, y + rise(x1), z0), (x0, y + rise(x0), z0)], [(0, 0)] * 4)
    # The rails' tops in the 'kolejnice' block under the bright quads: drop them (the bright one is it).
    pos, nrm, uv, idx = m.prims['kolejnice']
    keep = []
    for t in range(0, len(idx), 6):
        if all(nrm[idx[t + k]][1] < 0.5 for k in range(6)):
            keep += idx[t:t + 6]
    m.prims['kolejnice'] = (pos, nrm, uv, keep)
    return m


MATERIALS = {
    'kolejnice': ('colour', [0.38, 0.26, 0.19, 1.0]),
    'hlava': ('colour', [0.70, 0.71, 0.74, 1.0]),
    'asfalt': ('colour', [0.25, 0.25, 0.26, 1.0]),
}

PANEL_HALF = 2.2                   # a crossing's panels each side of the track's middle (ThreeDRoadOverTrack())
RAMP = 2.5                         # its ramps of asphalt down to the road
ROAD_HALF = 4.6                    # the road's half width along the track: the panels and ramps only over it
PANEL_SINK = 0.004                 # the panels' top so much under the rails' heads (their bright tops over it)
PANEL_TEX_W, PANEL_TEX_H = 64, 256


def panel_png(length):
    """A crossing's concrete panels from above: u across (the panels' width), v along the track; their seams every
    1.2 m or so, the grooves inside the rails' heads dark."""
    w, h = PANEL_TEX_W, PANEL_TEX_H
    im = Image.new('RGB', (w, h), (150, 148, 142))
    rnd = random.Random(17)
    px = im.load()
    for y in range(h):
        for x in range(w):
            n = rnd.randint(-7, 7)
            px[x, y] = (150 + n, 148 + n, 142 + n)
    panels = max(1, round(length / 1.2))
    for k in range(panels + 1):
        y = min(h - 1, int(k * h / panels))
        for x in range(w):
            px[x, y] = (96, 95, 92)
    for x in (w // 2 - 1, w // 2):
        for y in range(h):
            px[x, y] = (110, 108, 104)
    per_m = w / (2 * PANEL_HALF)
    for side in (-1, 1):
        c = w / 2 + side * (GAUGE / 2) * per_m
        inner = c - side * 2.0
        for y in range(h):
            for x in (int(round(inner)) - 1, int(round(inner))):
                if 0 <= x < w:
                    px[x, y] = (40, 38, 36)
    return png(im)


def crossing(length, kind):
    """A level crossing's piece along +X: the track the tile long (piece()), over it where the road goes its panels a
    hair under the rails' heads, its ramps and their ends."""
    m = piece(length, 0.0, kind)
    x0, x1 = -ROAD_HALF, ROAD_HALF
    top, a, b = RAIL_TOP_M - PANEL_SINK, PANEL_HALF, PANEL_HALF + RAMP
    m.quad('panel', [(x0, top, a), (x1, top, a), (x1, top, -a), (x0, top, -a)], [(1, 0), (1, 1), (0, 1), (0, 0)])
    m.quad('asfalt', [(x0, 0, b), (x1, 0, b), (x1, top, a), (x0, top, a)], [(0, 0)] * 4)
    m.quad('asfalt', [(x1, 0, -b), (x0, 0, -b), (x0, top, -a), (x1, top, -a)], [(0, 0)] * 4)
    m.quad('asfalt', [(x1, 0, b), (x1, 0, -b), (x1, top, -a), (x1, top, a)], [(0, 0)] * 4)
    m.quad('asfalt', [(x0, 0, -b), (x0, 0, b), (x0, top, a), (x0, top, -a)], [(0, 0)] * 4)
    return m


def write(path, mesh, name, pictures):
    """The pieces' glb: `pictures` a material's texture (png) by its name, the others their colour (MATERIALS)."""
    blob = bytearray()
    views = []

    def view(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        off = len(blob)
        blob.extend(data)
        v = {'buffer': 0, 'byteOffset': off, 'byteLength': len(data)}
        if target is not None:
            v['target'] = target
        views.append(v)
        return len(views) - 1

    accessors, primitives, materials, textures, images = [], [], [], [], []
    triangles = 0
    for mat, (pos, nrm, uv, idx) in mesh.prims.items():
        used = sorted(set(idx))
        remap = {old: new for new, old in enumerate(used)}
        pos = [pos[i] for i in used]
        nrm = [nrm[i] for i in used]
        uv = [uv[i] for i in used]
        idx = [remap[i] for i in idx]
        fp = [c for p in pos for c in p]
        fn = [c for p in nrm for c in p]
        ft = [c for p in uv for c in p]
        xs, ys, zs = fp[0::3], fp[1::3], fp[2::3]
        a = len(accessors)
        accessors += [
            {'bufferView': view(struct.pack('<%df' % len(fp), *fp), 34962), 'componentType': 5126, 'count': len(pos), 'type': 'VEC3', 'min': [min(xs), min(ys), min(zs)], 'max': [max(xs), max(ys), max(zs)]},
            {'bufferView': view(struct.pack('<%df' % len(fn), *fn), 34962), 'componentType': 5126, 'count': len(pos), 'type': 'VEC3'},
            {'bufferView': view(struct.pack('<%df' % len(ft), *ft), 34962), 'componentType': 5126, 'count': len(pos), 'type': 'VEC2'},
            {'bufferView': view(struct.pack('<%dH' % len(idx), *idx), 34963), 'componentType': 5123, 'count': len(idx), 'type': 'SCALAR'},
        ]
        triangles += len(idx) // 3
        m = {'name': name + '_' + mat, 'pbrMetallicRoughness': {'metallicFactor': 0.0, 'roughnessFactor': 1.0}}
        if mat in pictures:
            images.append({'bufferView': view(pictures[mat]), 'mimeType': 'image/png'})
            textures.append({'source': len(images) - 1, 'sampler': 0})
            m['pbrMetallicRoughness']['baseColorTexture'] = {'index': len(textures) - 1}
        else:
            m['pbrMetallicRoughness']['baseColorFactor'] = MATERIALS[mat][1]
        materials.append(m)
        primitives.append({'attributes': {'POSITION': a, 'NORMAL': a + 1, 'TEXCOORD_0': a + 2}, 'indices': a + 3, 'material': len(materials) - 1})
    gltf = {
        'asset': {'version': '2.0', 'generator': 'koleje.py (forclaude)'},
        'scene': 0,
        'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0, 'name': name}],
        'meshes': [{'name': name, 'primitives': primitives}],
        'materials': materials,
        'samplers': [{'magFilter': 9729, 'minFilter': 9987, 'wrapS': 33071, 'wrapT': 33071}],
        'accessors': accessors,
        'buffers': [{'byteLength': 0}],
        'bufferViews': views,
    }
    if textures:
        gltf['textures'] = textures
        gltf['images'] = images
    while len(blob) % 4:
        blob.append(0)
    gltf['buffers'][0]['byteLength'] = len(blob)
    js = json.dumps(gltf, separators=(',', ':')).encode()
    while len(js) % 4:
        js += b' '
    out = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob))
    out += struct.pack('<II', len(js), 0x4E4F534A) + js
    out += struct.pack('<II', len(blob), 0x004E4942) + bytes(blob)
    with open(path, 'wb') as f:
        f.write(out)
    print(f'{os.path.basename(path)}: {triangles} triangles, {len(out)} bytes')


def widen(factor):
    """Every measure across the track so many times (the wide track): the gauge, the embankment, the sleepers' length,
    the crossing's panels; the heights, the rails' own width and the lengths along as they are."""
    global GAUGE, BED_W, SLEEPER_L, PANEL_HALF
    GAUGE, BED_W, SLEEPER_L, PANEL_HALF = 1.435 * factor, 3.4 * factor, 2.6 * factor, 2.2 * factor


def main():
    for suffix, factor in (('', 1.0), ('_siroka', WIDE)):
        widen(factor)
        for kind in ('drevo', 'beton'):
            for name, length, climb in (('rovna', TILE_M, 0.0), ('sikma', DIAGONAL_M, 0.0), ('svah', TILE_M, LEVEL_MAP_M), ('svah_oci', TILE_M, LEVEL_EYE_M)):
                full = f'kolej_{name}_{kind}{suffix}'
                write(os.path.join(HERE, full + '.glb'), piece(length, climb, kind), full, {'nasep': top_png(length, kind), 'prazec': sleeper_png(kind)})
            full = f'kolej_pul_{kind}{suffix}'
            write(os.path.join(HERE, full + '.glb'), piece(TILE_M / 2, 0.0, kind), full, {'nasep': top_png(TILE_M / 2, kind), 'prazec': sleeper_png(kind)})
            full = f'kolej_prejezd_{kind}{suffix}'
            write(os.path.join(HERE, full + '.glb'), crossing(TILE_M, kind), full, {'nasep': top_png(TILE_M, kind), 'prazec': sleeper_png(kind), 'panel': panel_png(2 * ROAD_HALF)})
    print(f'hlava kolejnice {RAIL_TOP_M:.3f} m nad zemi, sikmy kus {DIAGONAL_M:.2f} m')


if __name__ == '__main__':
    main()
