#!/usr/bin/env python3
"""
Zdymadlo (the player, test3: "we need a 3D lock", 8. 11.: "only the 3D look"): the walls of a lock and the leaves of its
gates, our own geometry; the walls dressed in the concrete of zed_lic_beton.glb (OrphanRTG, CC BY 4.0; beton_5m.py),
the leaves in our own planks. The game puts them on the three tiles of a lock instead of its sprites (DrawWaterLock()
of water_cmd.cpp), the water of the game under them. Written beside this script:

  zdymadlo_zed_dolni.glb   a third of a wall of the lower tile: from the lower water up to a metre over the upper one (4 m)
  zdymadlo_zed_horni.glb   a third of a wall of the upper tile: a metre over the upper water
  zdymadlo_zed_svah_N.glb  the thirds of a wall of the middle tile, N 0 1 2 up it: the foot rising with the water's
                           slope (N to N + 1 m over the lower water), the top level with the others (4 m over it)
  zdymadlo_kridlo_dolni.glb  a leaf of the lower gate, 4 m high
  zdymadlo_kridlo_horni.glb  a leaf of the upper gate, 1 m high

glTF's Y up. A wall's third: along +X a third of a tile long (4.95 m; the game's tile 14.84 m), centred on the origin
along and across, 1.6 m thick across (Z), its foot at Y 0 (the slope's: the lower water's level; its uphill end +X). A
leaf: along +X from its hinge to its tip, centred on the origin along and across, 0.5 m thick, its foot at Y 0. Each
hung from its middle: the game sorts each by its own point, a third of a wall by the gate beside it behind or before
it as it stands (the player, test3: "the concrete at the back is drawn over the gate, on top"). Bollards on the walls'
tops. Nothing under the water: the game's water is drawn under all of it.

    python3 zdymadlo.py
"""
import io
import json
import math
import os
import struct

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
TILE_M = 16.0 / (16.0 / 14.83765)  # the game's tile, metres (OBJECT_UNITS_PER_METRE)
LEVEL_M = 3.0                      # a level of the game's height (OBJECT_HEIGHT_PER_METRE)
WALL_T = 1.6                       # a wall's thickness
OVER_M = 1.0                       # its top over the upper water
LEAF_T = 0.5
GATE_ANGLE = 18.0                  # a closed leaf's angle upstream off across (the lock's code knows it too)
CHAMBER_W = TILE_M - 2 * WALL_T
LEAF_L = CHAMBER_W / 2 / math.cos(math.radians(GATE_ANGLE))
TEX_M = 5.0                        # the concrete's texture: 5 x 5 m, seamless


def concrete_png():
    """The concrete of zed_lic_beton.glb (its one image)."""
    data = open(os.path.join(HERE, 'zed_lic_beton.glb'), 'rb').read()
    n = struct.unpack_from('<I', data, 12)[0]
    j = json.loads(data[20:20 + n])
    bv = j['bufferViews'][j['images'][0]['bufferView']]
    off = 20 + n + 8 + bv.get('byteOffset', 0)
    # Half its size: a centimetre and a half a pixel is enough for a lock's walls, and four of them stay small.
    im = Image.open(io.BytesIO(data[off:off + bv['byteLength']])).convert('RGB').resize((256, 256), Image.LANCZOS)
    out = io.BytesIO()
    im.save(out, format='PNG', optimize=True)
    return out.getvalue()


def planks_png():
    """Our planks: dark wet oak boards up the leaf, two steel bands across, 64 x 64."""
    im = Image.new('RGB', (64, 64))
    for y in range(64):
        for x in range(64):
            board = x // 8
            seam = x % 8 == 0
            grain = ((x * 13 + y * 3 + board * 29) % 7) - 3
            r, g, b = 84 + board % 3 * 6 + grain * 2, 60 + board % 3 * 4 + grain, 40 + grain
            if seam:
                r, g, b = r // 2, g // 2, b // 2
            if 14 <= y < 19 or 44 <= y < 49:
                n = (x * 5 + y) % 3
                r, g, b = 52 + n * 4, 54 + n * 4, 58 + n * 4
            # Wet and green towards its foot.
            k = max(0.0, (y - 52) / 12.0)
            r, g, b = int(r * (1 - 0.35 * k)), int(g * (1 - 0.15 * k)), int(b * (1 - 0.35 * k))
            im.putpixel((x, y), (r, g, b))
    out = io.BytesIO()
    im.save(out, format='PNG')
    return out.getvalue()


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


def concrete_box(m, x0, x1, z0, z1, foot0, foot1, top):
    """A wall's block along X from x0 to x1, across Z z0..z1, its foot rising from foot0 at x0 to foot1 at x1, its top
    level; cut along into pieces of the texture's 5 m, each its own u from 0 to 1 at most (the texture is seamless)."""
    pieces = max(1, math.ceil((x1 - x0) / TEX_M - 1e-6))
    step = (x1 - x0) / pieces

    def foot(x):
        return foot0 + (foot1 - foot0) * (x - x0) / (x1 - x0)

    for k in range(pieces):
        a, b = x0 + k * step, x0 + (k + 1) * step
        ua, ub = 0.0, step / TEX_M
        fa, fb = foot(a), foot(b)
        # The two long faces, the texture's v by height (1 at the top, as the walls of the game: v down from the top).
        for z, out in ((z1, 1), (z0, -1)):
            va, vb, vt = (top - fa) / TEX_M, (top - fb) / TEX_M, 0.0
            if out > 0:
                m.quad('beton', [(a, fa, z), (b, fb, z), (b, top, z), (a, top, z)], [(ua, va), (ub, vb), (ub, vt), (ua, vt)])
            else:
                m.quad('beton', [(b, fb, z), (a, fa, z), (a, top, z), (b, top, z)], [(ub, vb), (ua, va), (ua, vt), (ub, vt)])
        # The top, a strip of the texture along it.
        w = (z1 - z0) / TEX_M
        m.quad('beton', [(a, top, z1), (b, top, z1), (b, top, z0), (a, top, z0)], [(ua, 0.0), (ub, 0.0), (ub, w), (ua, w)])
    # The ends.
    for x, out in ((x1, 1), (x0, -1)):
        f = foot1 if x == x1 else foot0
        h = (top - f) / TEX_M
        w = (z1 - z0) / TEX_M
        if out > 0:
            m.quad('beton', [(x, f, z1), (x, f, z0), (x, top, z0), (x, top, z1)], [(0, h), (w, h), (w, 0), (0, 0)])
        else:
            m.quad('beton', [(x, f, z0), (x, f, z1), (x, top, z1), (x, top, z0)], [(0, h), (w, h), (w, 0), (0, 0)])


def bollard(m, x, z, y):
    """A black iron bollard on a wall's top: 0.3 m across, 0.45 m high, a wider cap."""
    seg = 8
    for r0, r1, y0, y1 in ((0.15, 0.15, y, y + 0.38), (0.21, 0.21, y + 0.38, y + 0.45)):
        for i in range(seg):
            a0, a1 = 2 * math.pi * i / seg, 2 * math.pi * (i + 1) / seg
            p0 = (x + r0 * math.cos(a0), y0, z + r0 * math.sin(a0))
            p1 = (x + r0 * math.cos(a1), y0, z + r0 * math.sin(a1))
            p2 = (x + r1 * math.cos(a1), y1, z + r1 * math.sin(a1))
            p3 = (x + r1 * math.cos(a0), y1, z + r1 * math.sin(a0))
            m.quad('zelezo', [p0, p3, p2, p1], [(0, 0)] * 4)
        cap = [(x + r1 * math.cos(2 * math.pi * i / seg), y1, z + r1 * math.sin(2 * math.pi * i / seg)) for i in range(seg)]
        for i in range(0, seg, 2):
            m.quad('zelezo', [(x, y1, z), cap[(i + 2) % seg], cap[(i + 1) % seg], cap[i]], [(0, 0)] * 4)


def wall(foot0, foot1, top):
    m = Mesh()
    third = TILE_M / 3
    concrete_box(m, -third / 2, third / 2, -WALL_T / 2, WALL_T / 2, foot0, foot1, top)
    bollard(m, 0.0, 0.0, top)
    return m


def leaf(height):
    m = Mesh()
    x0, x1, z0, z1, y0, y1 = -LEAF_L / 2, LEAF_L / 2, -LEAF_T / 2, LEAF_T / 2, 0.0, height
    v = 1.0
    m.quad('prkna', [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], [(0, v), (1, v), (1, 0), (0, 0)])
    m.quad('prkna', [(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)], [(0, v), (1, v), (1, 0), (0, 0)])
    m.quad('prkna', [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)], [(0, 0.02), (1, 0.02), (1, 0.0), (0, 0.0)])
    m.quad('prkna', [(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)], [(0, v), (0.05, v), (0.05, 0), (0, 0)])
    m.quad('prkna', [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)], [(0, v), (0.05, v), (0.05, 0), (0, 0)])
    # A handrail on its top: three posts and a bar.
    rail_y0, rail_y1 = y1 + 0.95, y1 + 1.0
    m.quad('zelezo', [(x0 + 0.3, rail_y0, 0.03), (x1 - 0.3, rail_y0, 0.03), (x1 - 0.3, rail_y1, 0.03), (x0 + 0.3, rail_y1, 0.03)], [(0, 0)] * 4)
    m.quad('zelezo', [(x1 - 0.3, rail_y0, -0.03), (x0 + 0.3, rail_y0, -0.03), (x0 + 0.3, rail_y1, -0.03), (x1 - 0.3, rail_y1, -0.03)], [(0, 0)] * 4)
    for x in (x0 + 0.3, 0.0, x1 - 0.3):
        m.quad('zelezo', [(x - 0.03, y1, 0.03), (x + 0.03, y1, 0.03), (x + 0.03, rail_y1, 0.03), (x - 0.03, rail_y1, 0.03)], [(0, 0)] * 4)
        m.quad('zelezo', [(x + 0.03, y1, -0.03), (x - 0.03, y1, -0.03), (x - 0.03, rail_y1, -0.03), (x + 0.03, rail_y1, -0.03)], [(0, 0)] * 4)
    return m


def write(path, mesh, name):
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
    tex_of = {}
    for mat, png in (('beton', concrete_png()), ('prkna', planks_png())):
        tex_of[mat] = png
    triangles = 0
    for mat, (pos, nrm, uv, idx) in mesh.prims.items():
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
        if mat in tex_of:
            images.append({'bufferView': view(tex_of[mat]), 'mimeType': 'image/png'})
            textures.append({'source': len(images) - 1, 'sampler': 0})
            m['pbrMetallicRoughness']['baseColorTexture'] = {'index': len(textures) - 1}
        else:
            m['pbrMetallicRoughness']['baseColorFactor'] = [0.09, 0.09, 0.10, 1.0]
        materials.append(m)
        primitives.append({'attributes': {'POSITION': a, 'NORMAL': a + 1, 'TEXCOORD_0': a + 2}, 'indices': a + 3, 'material': len(materials) - 1})
    gltf = {
        'asset': {'version': '2.0', 'generator': 'zdymadlo.py (forclaude)'},
        'scene': 0,
        'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0, 'name': name}],
        'meshes': [{'name': name, 'primitives': primitives}],
        'materials': materials,
        'samplers': [{'magFilter': 9729, 'minFilter': 9987, 'wrapS': 10497, 'wrapT': 10497}],
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


def main():
    top = LEVEL_M + OVER_M
    write(os.path.join(HERE, 'zdymadlo_zed_dolni.glb'), wall(0.0, 0.0, top), 'zdymadlo_zed_dolni')
    write(os.path.join(HERE, 'zdymadlo_zed_horni.glb'), wall(0.0, 0.0, OVER_M), 'zdymadlo_zed_horni')
    for k in range(3):
        write(os.path.join(HERE, f'zdymadlo_zed_svah_{k}.glb'), wall(k * LEVEL_M / 3, (k + 1) * LEVEL_M / 3, top), f'zdymadlo_zed_svah_{k}')
    write(os.path.join(HERE, 'zdymadlo_kridlo_dolni.glb'), leaf(top), 'zdymadlo_kridlo_dolni')
    write(os.path.join(HERE, 'zdymadlo_kridlo_horni.glb'), leaf(OVER_M), 'zdymadlo_kridlo_horni')
    print(f'komora {CHAMBER_W:.2f} m, kridlo {LEAF_L:.2f} m')


if __name__ == '__main__':
    main()
