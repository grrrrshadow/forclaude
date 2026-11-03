#!/usr/bin/env python3
"""
Trava 3D (the player, 3. 11.: "denser grass, like stubble at least, simple, colourful by its colouring ... it need not
be one tuft, the tuft can be modular, with flowers to add"): the pieces the game strews our grass of the temperate
climate with about the walker in the view from the eyes, our own geometry and textures. Written beside this script:

  trava_trs.glb     a patch of short grass 2 x 2 m: blades in clumps and alone, each a single triangle, mostly a
                    stubble's few centimetres, some up to a quarter of a metre; each blade one of eight shades of a
                    small palette (fresh, dark, yellowish, bluish, olive, light green, dry straw, brownish).
  trava_kvitky.glb  flowers to add among it: a few stalks with a head of five petals and an eye each, the petals of
                    one of six colours (white, yellow, violet, blue, pink, red).

glTF's Y up, metres, the middle of the patch (of the flowers) at the origin on the ground; seen from both sides,
normals up (lit as the ground is). The game lays the patches over a tile (sheared to its slope) and adds the flowers.

    python3 trava_3d.py
"""
import io
import json
import math
import os
import random
import struct

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SEED = 20261103

# The grass's shades (root darker, tip lighter) and how often each is.
GRASS = [
    ((88, 150, 48), 6),   # fresh green
    ((52, 98, 34), 4),    # dark green
    ((128, 160, 58), 3),  # yellowish green
    ((62, 122, 72), 2),   # bluish green
    ((108, 118, 50), 2),  # olive
    ((122, 178, 70), 3),  # light green
    ((176, 166, 98), 1),  # dry straw
    ((140, 118, 70), 1),  # brownish, dry
]
# The petals' colours, and the stalk's and the eye's.
PETALS = [(240, 240, 232), (246, 214, 40), (150, 92, 196), (82, 120, 220), (236, 140, 186), (214, 46, 40)]
STALK = (70, 120, 44)
EYE = (236, 196, 40)
CELL = 4  # pixels a column of the palette is wide


def palette(columns, rows, shade):
    """A texture of columns of colours, each CELL wide, its rows from the root (bottom) to the tip (top)."""
    im = Image.new('RGBA', (len(columns) * CELL, rows))
    for k, c in enumerate(columns):
        for y in range(rows):
            f = shade(1.0 - y / max(1, rows - 1))
            for x in range(CELL):
                im.putpixel((k * CELL + x, y), (min(255, int(c[0] * f)), min(255, int(c[1] * f)), min(255, int(c[2] * f)), 255))
    out = io.BytesIO()
    im.save(out, format='PNG')
    return out.getvalue(), len(columns) * CELL


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
        'asset': {'version': '2.0', 'generator': 'trava_3d.py (forclaude)'},
        'scene': 0,
        'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0, 'name': name}],
        'meshes': [{'name': name, 'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2}, 'indices': 3, 'material': 0}]}],
        'materials': [{'name': name, 'doubleSided': True, 'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}, 'metallicFactor': 0.0, 'roughnessFactor': 1.0}}],
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


def column_u(k, width):
    return (k * CELL + CELL / 2) / width


def blade(m, rnd, x, z, height, width, lean, u):
    """A blade: one triangle from its root (two corners on the ground) to its tip, leaning out; its face turned anyhow."""
    face = rnd.uniform(0, math.pi)
    wx, wz = math.cos(face) * width / 2, math.sin(face) * width / 2
    a = rnd.uniform(0, 2 * math.pi)
    tx, tz = x + math.cos(a) * lean, z + math.sin(a) * lean
    tip_h = math.sqrt(max(0.0, height * height - lean * lean))
    # Lit as the ground: up, a little toward its face.
    nx, nz = -math.sin(face) * 0.3, math.cos(face) * 0.3
    n = [nx, 0.95, nz]
    r0 = m.corner([x - wx, 0.0, z - wz], n, [u, 0.97])
    r1 = m.corner([x + wx, 0.0, z + wz], n, [u, 0.97])
    t = m.corner([tx, tip_h, tz], n, [u, 0.03])
    m.tri(r0, r1, t)


def grass_patch():
    rnd = random.Random(SEED)
    png, width = palette([c for c, _ in GRASS], 8, lambda t: 0.6 + 0.6 * t)
    pick = [k for k, (_, w) in enumerate(GRASS) for _ in range(w)]
    m = Mesh()
    half = 1.0

    def one(x, z, tall):
        h = rnd.uniform(0.12, 0.24) if tall else rnd.uniform(0.04, 0.12)
        blade(m, rnd, x, z, h, rnd.uniform(0.016, 0.03), h * rnd.uniform(0.05, 0.35), column_u(rnd.choice(pick), width))

    def inside(v):
        return max(-half, min(half, v))

    # Clumps: a few blades of one shade (mostly) close together; the rest alone, between them.
    for _ in range(46):
        cx, cz = rnd.uniform(-half, half), rnd.uniform(-half, half)
        shade = rnd.choice(pick)
        tall = rnd.random() < 0.35
        for _ in range(rnd.randint(4, 7)):
            x, z = inside(cx + rnd.gauss(0, 0.04)), inside(cz + rnd.gauss(0, 0.04))
            h = rnd.uniform(0.12, 0.24) if tall else rnd.uniform(0.05, 0.13)
            k = shade if rnd.random() < 0.7 else rnd.choice(pick)
            blade(m, rnd, x, z, h, rnd.uniform(0.016, 0.03), h * rnd.uniform(0.1, 0.4), column_u(k, width))
    for _ in range(70):
        one(rnd.uniform(-half, half), rnd.uniform(-half, half), rnd.random() < 0.2)
    write_glb('trava_trs', m, png)


def flowers():
    rnd = random.Random(SEED + 1)
    columns = PETALS + [STALK, EYE]
    png, width = palette(columns, 4, lambda t: 0.85 + 0.15 * t)
    m = Mesh()
    up = [0.0, 1.0, 0.0]
    for _ in range(7):
        x, z = rnd.uniform(-0.35, 0.35), rnd.uniform(-0.35, 0.35)
        h = rnd.uniform(0.15, 0.35)
        lean = rnd.uniform(0.0, 0.05)
        a = rnd.uniform(0, 2 * math.pi)
        hx, hz = x + math.cos(a) * lean, z + math.sin(a) * lean
        # The stalk: a thin triangle up to the head.
        face = rnd.uniform(0, math.pi)
        wx, wz = math.cos(face) * 0.005, math.sin(face) * 0.005
        su = column_u(len(PETALS), width)
        s0 = m.corner([x - wx, 0.0, z - wz], up, [su, 0.5])
        s1 = m.corner([x + wx, 0.0, z + wz], up, [su, 0.5])
        s2 = m.corner([hx, h, hz], up, [su, 0.5])
        m.tri(s0, s1, s2)
        # The head: five petals about its eye, tipped up a little.
        pu = column_u(rnd.randrange(len(PETALS)), width)
        r = rnd.uniform(0.035, 0.05)
        turn = rnd.uniform(0, 2 * math.pi)
        mid = m.corner([hx, h, hz], up, [pu, 0.5])
        for p in range(5):
            b = turn + 2 * math.pi * p / 5
            c0, c1 = b - 0.42, b + 0.42
            e0 = m.corner([hx + math.cos(c0) * r, h + r * 0.25, hz + math.sin(c0) * r], up, [pu, 0.5])
            e1 = m.corner([hx + math.cos(c1) * r, h + r * 0.25, hz + math.sin(c1) * r], up, [pu, 0.5])
            m.tri(mid, e0, e1)
        eu = column_u(len(PETALS) + 1, width)
        er = r * 0.35
        e = [m.corner([hx + math.cos(turn + 2 * math.pi * k / 3) * er, h + 0.003, hz + math.sin(turn + 2 * math.pi * k / 3) * er], up, [eu, 0.5]) for k in range(3)]
        m.tri(e[0], e[1], e[2])
    write_glb('trava_kvitky', m, png)


if __name__ == '__main__':
    grass_patch()
    flowers()
