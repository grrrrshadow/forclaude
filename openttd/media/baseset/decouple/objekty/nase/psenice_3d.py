#!/usr/bin/env python3
"""
Psenice 3D (the player, 11. 11.: "throw out the squares under the wheat ... and leave the wheat in squares, that will
not help"; before: "4 x 4 boards on one tile, as high as half a stalk of the wheat"): the tufts the game strews the
wheat of the farm's fields with in the view from the eyes, as our grass (trava_3d.py) -- each turned anyhow, laid
anywhere on the tile, no rows, the stalks from the ground. Our own geometry and textures. Written beside this script:

  psenice_trs_mlada.glb  young green wheat 0.6 m: blades from the ground, curving out.
  psenice_trs_zrala.glb  ripe wheat 1.2 m: golden stalks from the ground, an ear with its awns on each, a few dry
                         leaves low.

Each is three upright planes 1.6 m wide crossing at their middle (a star of six arms), each plane its own picture of
the stalks on nothing (alpha 0 between them: the material's "MASK"). glTF's Y up, metres, the middle at the origin on
the ground; seen from both sides, normals up (lit as the ground is). The game lays the tufts over a field's tile, each
sheared to its slope.

    python3 psenice_3d.py
"""
import io
import json
import math
import os
import random
import struct

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SEED = 20261111
WIDTH_M = 1.6      # a plane's width
PLANES = 3         # crossing at the middle, 60 degrees apart
VARIANT_PX = 128   # a plane's picture is this wide
HEIGHT_PX = 256    # and this high


def mix(a, b, t):
    return tuple(int(round(a[k] + (b[k] - a[k]) * t)) for k in range(3))


def shade(c, f):
    return tuple(max(0, min(255, int(round(c[k] * f)))) for k in range(3))


def stalk(d, x0, y0, x1, y1, base, tip, width, bend=0.0, steps=10):
    """A stalk from (x0, y0) at the ground to (x1, y1): its colour from base to tip, bent sideways by bend pixels."""
    prev = (x0, y0)
    for s in range(1, steps + 1):
        t = s / steps
        x = x0 + (x1 - x0) * t + bend * t * t
        y = y0 + (y1 - y0) * t
        w = width if t < 0.6 else max(1, width - 1)
        d.line([prev, (x, y)], fill=mix(base, tip, t) + (255,), width=w)
        prev = (x, y)
    return prev


def ripe_variant(d, ox, rnd):
    """A plane of ripe wheat: dense stalks to 0.74-0.97 of the height, each with an ear and its awns; dry leaves low."""
    h = HEIGHT_PX
    # Dry leaves low among the stalks.
    for _ in range(26):
        x0 = ox + rnd.uniform(2, VARIANT_PX - 3)
        top = h - h * rnd.uniform(0.18, 0.42)
        side = rnd.choice((-1, 1)) * rnd.uniform(6, 16)
        stalk(d, x0, h - 1, x0 + side * 0.4, top, (112, 96, 46), (178, 156, 86), 2, side * 0.6, 6)
    for _ in range(92):
        x0 = ox + rnd.uniform(2, VARIANT_PX - 3)
        top = h - h * rnd.uniform(0.74, 0.97)
        lean = rnd.gauss(0, 3.5)
        ear = rnd.uniform(18, 24)
        tone = rnd.uniform(0.9, 1.08)
        base, tip = shade((118, 90, 40), tone), shade((206, 170, 84), tone)
        x1 = max(ox + 2, min(ox + VARIANT_PX - 3, x0 + lean))
        # The stalk to the ear's foot.
        stalk(d, x0, h - 1, x1, top + ear, base, tip, 2, rnd.gauss(0, 1.5))
        # The ear: a narrow spindle of grains, lighter, the grains' edges darker by turns on its two sides.
        body = shade((216, 176, 86), tone)
        dark = shade((168, 128, 52), tone)
        for k in range(int(ear)):
            y = top + ear - k
            half = 1.9 * math.sin(math.pi * (k + 1) / (ear + 1)) + 0.5
            d.line([(x1 - half, y), (x1 + half, y)], fill=body + (255,), width=1)
            d.point((x1 - half if k % 2 == 0 else x1 + half, y), fill=dark + (255,))
        # Its awns, up from it, a little apart.
        for a in (-1, 0, 1):
            ax = x1 + a * rnd.uniform(1.0, 3.0)
            d.line([(x1 + a * 0.8, top + 2), (ax + a * rnd.uniform(1, 3), top - rnd.uniform(8, 14))], fill=shade((228, 202, 134), tone) + (255,), width=1)


def young_variant(d, ox, rnd):
    """A plane of young wheat: blades from the ground to 0.5-1.0 of the height, curving out, in greens."""
    h = HEIGHT_PX
    for _ in range(120):
        x0 = ox + rnd.uniform(2, VARIANT_PX - 3)
        top = h - h * rnd.uniform(0.5, 1.0)
        bend = rnd.gauss(0, 12)
        tone = rnd.uniform(0.85, 1.12)
        x1 = max(ox + 2, min(ox + VARIANT_PX - 3, x0 + rnd.gauss(0, 4)))
        base, tip = shade((44, 90, 32), tone), shade((112, 172, 66), tone)
        # Not past its plane's picture: the bend kept in it.
        bend = max(ox + 2 - x1, min(ox + VARIANT_PX - 3 - x1, bend))
        stalk(d, x0, h - 1, x1, top, base, tip, 3 if rnd.random() < 0.4 else 2, bend, 12)


def picture(variant, rnd):
    im = Image.new('RGBA', (VARIANT_PX * PLANES, HEIGHT_PX), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for k in range(PLANES):
        variant(d, k * VARIANT_PX, rnd)
    # Whole or nothing: the game takes a texel of alpha below half as nothing.
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            px[x, y] = (r, g, b, 255) if a >= 128 else (0, 0, 0, 0)
    out = io.BytesIO()
    im.save(out, format='PNG')
    return out.getvalue()


class Mesh:
    def __init__(self):
        self.pos, self.nrm, self.uv, self.idx = [], [], [], []

    def corner(self, p, n, t):
        self.pos.extend(p)
        self.nrm.extend(n)
        self.uv.extend(t)
        return len(self.pos) // 3 - 1

    def tri(self, a, b, c):
        self.idx.extend([a, b, c])


def star(height):
    """The planes crossing at the middle, each its own column of the picture."""
    m = Mesh()
    for k in range(PLANES):
        a = math.pi * k / PLANES
        dx, dz = math.cos(a) * WIDTH_M / 2, math.sin(a) * WIDTH_M / 2
        # Lit as the ground: up, a little toward its face.
        n = [-math.sin(a) * 0.25, 0.97, math.cos(a) * 0.25]
        u0, u1 = (k * VARIANT_PX + 0.5) / (PLANES * VARIANT_PX), ((k + 1) * VARIANT_PX - 0.5) / (PLANES * VARIANT_PX)
        c0 = m.corner([-dx, 0.0, -dz], n, [u0, 1.0])
        c1 = m.corner([dx, 0.0, dz], n, [u1, 1.0])
        c2 = m.corner([dx, height, dz], n, [u1, 0.0])
        c3 = m.corner([-dx, height, -dz], n, [u0, 0.0])
        m.tri(c0, c1, c2)
        m.tri(c0, c2, c3)
    return m


def write_glb(name, mesh, png):
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

    corners = len(mesh.pos) // 3
    p_view = view(struct.pack('<%df' % len(mesh.pos), *mesh.pos), 34962)
    n_view = view(struct.pack('<%df' % len(mesh.nrm), *mesh.nrm), 34962)
    t_view = view(struct.pack('<%df' % len(mesh.uv), *mesh.uv), 34962)
    i_view = view(struct.pack('<%dH' % len(mesh.idx), *mesh.idx), 34963)
    img_view = view(png)
    xs, ys, zs = mesh.pos[0::3], mesh.pos[1::3], mesh.pos[2::3]
    gltf = {
        'asset': {'version': '2.0', 'generator': 'psenice_3d.py (forclaude)'},
        'scene': 0,
        'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0, 'name': name}],
        'meshes': [{'name': name, 'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2}, 'indices': 3, 'material': 0}]}],
        'materials': [{'name': name, 'doubleSided': True, 'alphaMode': 'MASK', 'alphaCutoff': 0.5,
                       'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}, 'metallicFactor': 0.0, 'roughnessFactor': 1.0}}],
        'textures': [{'source': 0, 'sampler': 0}],
        'samplers': [{'magFilter': 9728, 'minFilter': 9728}],
        'images': [{'bufferView': img_view, 'mimeType': 'image/png'}],
        'accessors': [
            {'bufferView': p_view, 'componentType': 5126, 'count': corners, 'type': 'VEC3', 'min': [min(xs), min(ys), min(zs)], 'max': [max(xs), max(ys), max(zs)]},
            {'bufferView': n_view, 'componentType': 5126, 'count': corners, 'type': 'VEC3'},
            {'bufferView': t_view, 'componentType': 5126, 'count': corners, 'type': 'VEC2'},
            {'bufferView': i_view, 'componentType': 5123, 'count': len(mesh.idx), 'type': 'SCALAR'},
        ],
        'buffers': [{'byteLength': 0}],
        'bufferViews': views,
    }
    while len(blob) % 4:
        blob.append(0)
    gltf['buffers'][0]['byteLength'] = len(blob)
    js = json.dumps(gltf, separators=(',', ':')).encode()
    while len(js) % 4:
        js += b' '
    out = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob))
    out += struct.pack('<II', len(js), 0x4E4F534A) + js
    out += struct.pack('<II', len(blob), 0x004E4942) + bytes(blob)
    path = os.path.join(HERE, name + '.glb')
    with open(path, 'wb') as f:
        f.write(out)
    print(f'{path}: {len(mesh.idx) // 3} triangles, {corners} corners, {len(out)} bytes')


if __name__ == '__main__':
    write_glb('psenice_trs_mlada', star(0.6), picture(young_variant, random.Random(SEED)))
    write_glb('psenice_trs_zrala', star(1.2), picture(ripe_variant, random.Random(SEED + 1)))
