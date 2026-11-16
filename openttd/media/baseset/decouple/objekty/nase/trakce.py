#!/usr/bin/env python3
"""
Trakce (the player, 8. 11.: "such masts for the electrified track, for the catenary, but simple; the wires as the
fishing rod's line"; "faithful to the view from above, so one on each tile"; "2 may be over both"): the masts of our
catenary, laid by the game beside our 3D track (DrawThreeDCatenary() of rail_cmd.cpp), our own geometry and our own
pictures. The wires the game draws itself (thin lines). Written beside this script:

  trakce_stozar.glb            a lattice mast on its concrete foot, 8.8 m high: its foot's middle at the origin
  trakce_vyloznik.glb          its arm out over the track along +X, 7.42 m (half a tile: a mast at a tile's corner, the
                               track at the middle of its edge), from the mast's axis: the tube at the carrier wire's
                               height, the stay from the mast's top, brown insulators by the mast, the registration down
                               to the contact wire at its end
  trakce_vyloznik_sikmy.glb    the same 5.25 m long (to the middle of a piece across a tile's corner)

glTF's Y up. The heights over the ground: the rails' tops 0.37 m (koleje.py), the contact wire 5.5 m over them
(5.87 m), the carrier wire 1.4 m over it (7.27 m). The lattice of the mast a picture with holes (transparent), 32 bpp.

    python3 trakce.py
"""
import io
import json
import math
import os
import struct

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
TILE_M = 14.83765
RAIL_TOP = 0.372
CONTACT = RAIL_TOP + 5.5          # the contact wire over the ground
CARRIER = CONTACT + 1.4           # the carrier wire
MAST_W = 0.36
MAST_TOP = 8.8
FOOT_W, FOOT_H = 0.7, 0.3
ARM_W = 0.09
STAY_AT = 8.55                    # where the stay leaves the mast
INSULATOR_L, INSULATOR_W = 0.32, 0.14
REG_W = 0.05
TEX_W, TEX_H = 16, 384            # the lattice: 0.36 m across, the mast's height along


def png(im):
    out = io.BytesIO()
    im.save(out, format='PNG', optimize=True)
    return out.getvalue()


def lattice_png():
    """The lattice of a mast's face: two chords at its sides, a zigzag of diagonals between them, a batten now and then;
    the rest transparent (0, 0, 0, 0: cut out)."""
    im = Image.new('RGBA', (TEX_W, TEX_H), (0, 0, 0, 0))
    steel = (88, 98, 92, 255)
    dark = (60, 68, 64, 255)
    px = im.load()
    for y in range(TEX_H):
        for x in (0, 1, TEX_W - 2, TEX_W - 1):
            px[x, y] = steel if x in (0, TEX_W - 1) else dark
    period = 24
    for y in range(TEX_H):
        phase = y % (2 * period)
        f = phase / period if phase < period else 2 - phase / period
        cx = 1 + f * (TEX_W - 3)
        for x in range(int(cx) - 1, int(cx) + 2):
            if 0 <= x < TEX_W:
                px[x, y] = steel
        if y % (2 * period) in (0, 1):
            for x in range(TEX_W):
                px[x, y] = dark
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


def box(m, mat, x0, x1, y0, y1, z0, z1, bottom=False, uv=None):
    """An upright box (glTF: Y up), x0..x1, y0..y1 high, z0..z1 across: its four sides, its top (and its bottom)."""
    u = uv or (lambda face: [(0, 1), (1, 1), (1, 0), (0, 0)])
    m.quad(mat, [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], u('z1'))
    m.quad(mat, [(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)], u('z0'))
    m.quad(mat, [(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)], u('x1'))
    m.quad(mat, [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)], u('x0'))
    m.quad(mat, [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)], [(0, 0)] * 4)
    if bottom:
        m.quad(mat, [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)], [(0, 0)] * 4)


def beam(m, mat, a, b, w):
    """A square bar from a to b (x, y up) in the plane z 0, w across: its four long sides, its ends."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    ln = math.hypot(dx, dy) or 1.0
    nx, ny = -dy / ln * w / 2, dx / ln * w / 2
    h = w / 2

    def p(q, s, z):
        return (q[0] + nx * s, q[1] + ny * s, z)
    uv = [(0, 0)] * 4
    m.quad(mat, [p(a, 1, -h), p(b, 1, -h), p(b, 1, h), p(a, 1, h)], uv)      # the upper side
    m.quad(mat, [p(b, -1, -h), p(a, -1, -h), p(a, -1, h), p(b, -1, h)], uv)  # the lower side
    m.quad(mat, [p(a, -1, h), p(a, 1, h), p(b, 1, h), p(b, -1, h)], uv)      # the +z side
    m.quad(mat, [p(a, 1, -h), p(a, -1, -h), p(b, -1, -h), p(b, 1, -h)], uv)  # the -z side
    m.quad(mat, [p(a, -1, -h), p(a, 1, -h), p(a, 1, h), p(a, -1, h)], uv)
    m.quad(mat, [p(b, 1, -h), p(b, -1, -h), p(b, -1, h), p(b, 1, h)], uv)


def mast():
    m = Mesh()
    box(m, 'beton', -FOOT_W / 2, FOOT_W / 2, 0.0, FOOT_H, -FOOT_W / 2, FOOT_W / 2)
    h = MAST_W / 2
    box(m, 'mrizka', -h, h, FOOT_H, MAST_TOP, -h, h, uv=lambda face: [(0, 1), (1, 1), (1, 0), (0, 0)])
    # Its top capped.
    m.quad('ocel', [(-h - 0.03, MAST_TOP + 0.02, h + 0.03), (h + 0.03, MAST_TOP + 0.02, h + 0.03), (h + 0.03, MAST_TOP + 0.02, -h - 0.03), (-h - 0.03, MAST_TOP + 0.02, -h - 0.03)], [(0, 0)] * 4)
    return m


def arm(length):
    """The arm out along +X from the mast's axis, the track's middle at its end."""
    m = Mesh()
    x0 = MAST_W / 2
    # The tube at the carrier wire's height, the stay from the mast's top to near its end.
    beam(m, 'ocel', (x0 + INSULATOR_L, CARRIER), (length + 0.15, CARRIER), ARM_W)
    beam(m, 'ocel', (x0 + INSULATOR_L * 0.8, STAY_AT - 0.06), (length - 0.6, CARRIER + 0.05), ARM_W * 0.8)
    # The brown insulators by the mast.
    beam(m, 'izolator', (x0, CARRIER), (x0 + INSULATOR_L, CARRIER), INSULATOR_W)
    beam(m, 'izolator', (x0, STAY_AT), (x0 + INSULATOR_L * 0.8, STAY_AT - 0.06), INSULATOR_W)
    # The registration down to the contact wire at its end, a clamp on the carrier wire.
    beam(m, 'ocel', (length, CARRIER), (length, CONTACT - 0.05), REG_W)
    beam(m, 'ocel', (length - 0.4, CONTACT + 0.25), (length, CONTACT), REG_W)
    box(m, 'ocel', length - 0.08, length + 0.08, CARRIER - 0.08, CARRIER + 0.05, -0.08, 0.08, bottom=True)
    return m


MATERIALS = {
    'beton': ('colour', [0.62, 0.61, 0.58, 1.0]),
    'ocel': ('colour', [0.42, 0.45, 0.44, 1.0]),
    'izolator': ('colour', [0.45, 0.20, 0.10, 1.0]),
    'mrizka': ('texture', lattice_png),
}


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
        how, what = MATERIALS[mat]
        if how == 'texture':
            images.append({'bufferView': view(what()), 'mimeType': 'image/png'})
            textures.append({'source': len(images) - 1, 'sampler': 0})
            m['pbrMetallicRoughness']['baseColorTexture'] = {'index': len(textures) - 1}
            m['alphaMode'] = 'MASK'
            m['doubleSided'] = True
        else:
            m['pbrMetallicRoughness']['baseColorFactor'] = what
        materials.append(m)
        primitives.append({'attributes': {'POSITION': a, 'NORMAL': a + 1, 'TEXCOORD_0': a + 2}, 'indices': a + 3, 'material': len(materials) - 1})
    gltf = {
        'asset': {'version': '2.0', 'generator': 'trakce.py (forclaude)'},
        'scene': 0,
        'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0, 'name': name}],
        'meshes': [{'name': name, 'primitives': primitives}],
        'materials': materials,
        'samplers': [{'magFilter': 9728, 'minFilter': 9984, 'wrapS': 33071, 'wrapT': 33071}],
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
    write(os.path.join(HERE, 'trakce_stozar.glb'), mast(), 'trakce_stozar')
    write(os.path.join(HERE, 'trakce_vyloznik.glb'), arm(TILE_M / 2), 'trakce_vyloznik')
    write(os.path.join(HERE, 'trakce_vyloznik_sikmy.glb'), arm(TILE_M / 2 / math.sqrt(2)), 'trakce_vyloznik_sikmy')
    print(f'trolej {CONTACT:.2f} m, nosne lano {CARRIER:.2f} m nad zemi, vylozniky {TILE_M / 2:.2f} a {TILE_M / 2 / math.sqrt(2):.2f} m')


if __name__ == '__main__':
    main()
