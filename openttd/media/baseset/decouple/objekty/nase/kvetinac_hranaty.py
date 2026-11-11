#!/usr/bin/env python3
"""
Hranatý květináč (the player, test6: "the depot has a roof, so it cannot be stretched to the edge of the tile. Let us
cover those rails with a tall prism -- yes, a big square planter with grass in it ... tufts of grass and three nice big
marijuana plants"): a long concrete planter with soil and tufts of grass, our own geometry and textures. The game stands
it behind the back wall of each rail depot, over the rails of the tile left between the wall and the tile's edge, and
the three plants in it (ReadDepot() of three_d_objects.cpp).

Written as kvetinac_hranaty.glb beside this script: glTF's Y up, metres, its foot's middle at the origin; LENGTH long
along glTF's Z (the game's y), DEPTH deep along x, HEIGHT high. Three materials, 32x32 textures each (32bpp): the
concrete (repeated over its walls), the soil, the grass (seen from both sides: each blade a single triangle).

    python3 kvetinac_hranaty.py
"""
import io
import json
import math
import os
import random
import struct

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SEED = 20261111

LENGTH, DEPTH, HEIGHT = 6.0, 0.5, 0.9
WALL = 0.06            # the concrete's thickness
SOIL = 0.82            # the soil's top inside
CONCRETE_TILE = 1.0    # metres one copy of the concrete's texture covers

# The grass's shades (root darker, tip lighter), as our grass of the map (trava_3d.py).
GRASS = [
    ((88, 150, 48), 6),   # fresh green
    ((52, 98, 34), 4),    # dark green
    ((128, 160, 58), 3),  # yellowish green
    ((62, 122, 72), 2),   # bluish green
    ((108, 118, 50), 2),  # olive
    ((122, 178, 70), 3),  # light green
    ((176, 166, 98), 1),  # dry straw
]
CELL = 4  # pixels a column of the grass's palette is wide


def png_of(im):
    out = io.BytesIO()
    im.save(out, format='PNG')
    return out.getvalue()


def concrete():
    """Grey concrete, a little mottled, the joints of its formwork boards faint across it."""
    rnd = random.Random(SEED)
    im = Image.new('RGBA', (32, 32))
    for y in range(32):
        for x in range(32):
            n = rnd.randint(-9, 9)
            band = -10 if y % 8 == 7 else 0
            v = 150 + n + band
            im.putpixel((x, y), (v, v - 2, v - 8, 255))
    return png_of(im)


def soil():
    rnd = random.Random(SEED + 1)
    im = Image.new('RGBA', (32, 32))
    for y in range(32):
        for x in range(32):
            n = rnd.randint(-8, 8)
            crumb = 14 if rnd.random() < 0.06 else 0
            im.putpixel((x, y), (66 + n + crumb, 46 + n // 2 + crumb // 2, 32 + n // 2, 255))
    return png_of(im)


def grass_palette():
    """Columns of the grass's shades, each CELL wide, its rows from the root (bottom) to the tip (top)."""
    im = Image.new('RGBA', (32, 32))
    for k in range(32 // CELL):
        c = GRASS[k % len(GRASS)][0]
        for y in range(32):
            f = 0.6 + 0.6 * (1.0 - y / 31)
            for x in range(CELL):
                im.putpixel((k * CELL + x, y), (min(255, int(c[0] * f)), min(255, int(c[1] * f)), min(255, int(c[2] * f)), 255))
    return png_of(im)


class Mesh:
    def __init__(self):
        self.pos, self.nrm, self.uv, self.idx = [], [], [], []

    def corner(self, p, n, t):
        """A corner in the game's frame (x, y, z up), written in glTF's (x, z, -y)."""
        self.pos.extend([p[0], p[2], -p[1]])
        self.nrm.extend([n[0], n[2], -n[1]])
        self.uv.extend(t)
        return len(self.pos) // 3 - 1

    def tri(self, a, b, c):
        self.idx.extend([a, b, c])

    def quad(self, corners, normal, uvs):
        """Four corners in order round it, its outer side the normal's: wound so whichever order they come in."""
        k = [self.corner(p, normal, t) for p, t in zip(corners, uvs)]
        a, b, c = corners[0], corners[1], corners[2]
        u = [b[i] - a[i] for i in range(3)]
        v = [c[i] - a[i] for i in range(3)]
        g = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        if sum(g[i] * normal[i] for i in range(3)) >= 0:
            self.tri(k[0], k[1], k[2])
            self.tri(k[0], k[2], k[3])
        else:
            self.tri(k[0], k[2], k[1])
            self.tri(k[0], k[3], k[2])


def box_walls(m):
    """The concrete: its four outer walls, the rim over them, its four inner walls down to the soil."""
    x0, x1, y0, y1 = -DEPTH / 2, DEPTH / 2, -LENGTH / 2, LENGTH / 2
    ix0, ix1, iy0, iy1 = x0 + WALL, x1 - WALL, y0 + WALL, y1 - WALL
    t = CONCRETE_TILE

    def wall(ax, ay, bx, by, z0, z1, normal):
        run = math.hypot(bx - ax, by - ay) / t
        m.quad([(ax, ay, z0), (bx, by, z0), (bx, by, z1), (ax, ay, z1)], normal,
               [(0, z0 / t), (run, z0 / t), (run, z1 / t), (0, z1 / t)])

    # Outer walls.
    wall(x0, y0, x0, y1, 0.0, HEIGHT, (-1, 0, 0))
    wall(x1, y0, x1, y1, 0.0, HEIGHT, (1, 0, 0))
    wall(x0, y0, x1, y0, 0.0, HEIGHT, (0, -1, 0))
    wall(x0, y1, x1, y1, 0.0, HEIGHT, (0, 1, 0))
    # Inner walls, facing in, from the soil up to the rim.
    wall(ix0, iy0, ix0, iy1, SOIL, HEIGHT, (1, 0, 0))
    wall(ix1, iy0, ix1, iy1, SOIL, HEIGHT, (-1, 0, 0))
    wall(ix0, iy0, ix1, iy0, SOIL, HEIGHT, (0, 1, 0))
    wall(ix0, iy1, ix1, iy1, SOIL, HEIGHT, (0, -1, 0))

    # The rim: four strips over the walls' tops.
    def top(ax0, ay0, ax1, ay1):
        m.quad([(ax0, ay0, HEIGHT), (ax1, ay0, HEIGHT), (ax1, ay1, HEIGHT), (ax0, ay1, HEIGHT)], (0, 0, 1),
               [(ax0 / t, ay0 / t), (ax1 / t, ay0 / t), (ax1 / t, ay1 / t), (ax0 / t, ay1 / t)])

    top(x0, y0, ix0, y1)
    top(ix1, y0, x1, y1)
    top(ix0, y0, ix1, iy0)
    top(ix0, iy1, ix1, y1)


def soil_top(m):
    x0, x1, y0, y1 = -DEPTH / 2 + WALL, DEPTH / 2 - WALL, -LENGTH / 2 + WALL, LENGTH / 2 - WALL
    m.quad([(x0, y0, SOIL), (x1, y0, SOIL), (x1, y1, SOIL), (x0, y1, SOIL)], (0, 0, 1),
           [(x0, y0), (x1, y0), (x1, y1), (x0, y1)])


def grass(m):
    """Tufts over the soil: blades of one shade (mostly) close together, a few alone between them."""
    rnd = random.Random(SEED + 2)
    pick = [k for k, (_, w) in enumerate(GRASS) for _ in range(w)]
    hx, hy = DEPTH / 2 - WALL - 0.01, LENGTH / 2 - WALL - 0.01

    def blade(x, y, height, width, lean, k):
        face = rnd.uniform(0, math.pi)
        wx, wy = math.cos(face) * width / 2, math.sin(face) * width / 2
        a = rnd.uniform(0, 2 * math.pi)
        tx, ty = x + math.cos(a) * lean, y + math.sin(a) * lean
        tip = math.sqrt(max(0.0, height * height - lean * lean))
        u = (k * CELL + CELL / 2) / 32
        n = (-math.sin(face) * 0.3, math.cos(face) * 0.3, 0.95)
        r0 = m.corner((x - wx, y - wy, SOIL), n, (u, 0.97))
        r1 = m.corner((x + wx, y + wy, SOIL), n, (u, 0.97))
        tp = m.corner((tx, ty, SOIL + tip), n, (u, 0.03))
        m.tri(r0, r1, tp)

    def inside(v, h):
        return max(-h, min(h, v))

    for _ in range(64):
        cx, cy = rnd.uniform(-hx, hx), rnd.uniform(-hy, hy)
        shade = rnd.choice(pick)
        tall = rnd.uniform(0.3, 0.6)
        for _ in range(rnd.randint(8, 13)):
            x, y = inside(cx + rnd.gauss(0, 0.035), hx), inside(cy + rnd.gauss(0, 0.06), hy)
            h = tall * rnd.uniform(0.6, 1.0)
            k = shade if rnd.random() < 0.75 else rnd.choice(pick)
            blade(x, y, h, rnd.uniform(0.03, 0.05), h * rnd.uniform(0.15, 0.5), k)
    for _ in range(80):
        blade(rnd.uniform(-hx, hx), rnd.uniform(-hy, hy), rnd.uniform(0.1, 0.3), rnd.uniform(0.02, 0.04), rnd.uniform(0.0, 0.08), rnd.choice(pick))


def write_glb(parts):
    """parts: (material name, mesh, png, seen from both sides) each."""
    blob = bytearray()
    views, accessors, primitives, materials, textures, images = [], [], [], [], [], []

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

    for k, (name, m, png, both) in enumerate(parts):
        corners = len(m.pos) // 3
        xs, ys, zs = m.pos[0::3], m.pos[1::3], m.pos[2::3]
        a = len(accessors)
        accessors.append({'bufferView': view(struct.pack('<%df' % len(m.pos), *m.pos), 34962), 'componentType': 5126, 'count': corners, 'type': 'VEC3',
                          'min': [min(xs), min(ys), min(zs)], 'max': [max(xs), max(ys), max(zs)]})
        accessors.append({'bufferView': view(struct.pack('<%df' % len(m.nrm), *m.nrm), 34962), 'componentType': 5126, 'count': corners, 'type': 'VEC3'})
        accessors.append({'bufferView': view(struct.pack('<%df' % len(m.uv), *m.uv), 34962), 'componentType': 5126, 'count': corners, 'type': 'VEC2'})
        accessors.append({'bufferView': view(struct.pack('<%dH' % len(m.idx), *m.idx), 34963), 'componentType': 5123, 'count': len(m.idx), 'type': 'SCALAR'})
        images.append({'bufferView': view(png), 'mimeType': 'image/png'})
        textures.append({'source': k, 'sampler': 0})
        material = {'name': name, 'pbrMetallicRoughness': {'baseColorTexture': {'index': k}, 'metallicFactor': 0.0, 'roughnessFactor': 1.0}}
        if both:
            material['doubleSided'] = True
        materials.append(material)
        primitives.append({'attributes': {'POSITION': a, 'NORMAL': a + 1, 'TEXCOORD_0': a + 2}, 'indices': a + 3, 'material': k})
    gltf = {
        'asset': {'version': '2.0', 'generator': 'kvetinac_hranaty.py (forclaude)'},
        'scene': 0,
        'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0, 'name': 'kvetinac_hranaty'}],
        'meshes': [{'name': 'kvetinac_hranaty', 'primitives': primitives}],
        'materials': materials,
        'textures': textures,
        'samplers': [{'magFilter': 9728, 'minFilter': 9728}],
        'images': images,
        'accessors': accessors,
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
    path = os.path.join(HERE, 'kvetinac_hranaty.glb')
    with open(path, 'wb') as f:
        f.write(out)
    tris = sum(len(m.idx) // 3 for _, m, _, _ in parts)
    print(f'{path}: {tris} triangles, {len(out)} bytes')


def main():
    walls, earth, blades = Mesh(), Mesh(), Mesh()
    box_walls(walls)
    soil_top(earth)
    grass(blades)
    write_glb([('beton', walls, concrete(), False), ('hlina', earth, soil(), False), ('trava', blades, grass_palette(), True)])


if __name__ == '__main__':
    main()
