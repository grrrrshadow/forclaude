#!/usr/bin/env python3
"""
Vjezd tunelu pro lodě (the player, 10. 11.: "udělej vjezd do tunelu, ten beton. akvadukt je širší než silnice"): the
concrete mouth of a ship tunnel the canal's whole width, our own geometry, dressed in the concrete of zed_lic_beton.glb
(OrphanRTG, CC BY 4.0; beton_5m.py) as the lock (zdymadlo.py). The game puts them on a ship tunnel's end instead of the
road tunnel's sprites, the game's water under them (DrawThreeDShipTunnelEntrance() of tunnelbridge_cmd.cpp). Written
beside this script:

  tunel_lodni_zed_N.glb  the thirds of a wing wall along a side of the end tile, N 0 1 2 towards the portal: its inner
                         face from the water up, its outer face from the land beside, which rises with the end tile's
                         slope (N to N + 1 thirds of a level over the water), its top level with that land; its
                         inner face to the left: the wall on the right looking into the tunnel
  tunel_lodni_zed_N_z.glb  the same mirrored, for the wall on the left
  tunel_lodni_vrch_N.glb  only the top of the N-th third of a wall: drawn when the portal is seen from behind, over the
                         hill, where the walls' faces to the water are behind the portal and under the hill
  tunel_lodni_celo.glb   the portal's face across the end tile at the hill: its front from the water up to the hill's
                         top, the dark of the opening on it as wide and high as the tunnel's tube takes; no back (it
                         is under the hill)

Their faces one-sided ("jednostranne" in objekty.json), as glTF has them: seen from behind, under the land beside or
under the hill, nothing of them is drawn over the land (the player, 10. 11.: "again it draws in front what should be at
the back"; "higher than the terrain": their tops level with the land).

glTF's Y up. A wall's third: along +X a third of a tile long (4.95 m), centred on the origin along and across, 1 m thick
across (Z), its inner face at Z -0.5 (the water), its outer at +0.5. The face: along +X across the tile (14.84 m),
centred on the origin, 1 m thick (Z), its front at Z -0.5 (the canal), its foot at Y 0 (the water). Each hung from its
middle: the game sorts each by its own point, as the lock's walls.

The same for a road tunnel's mouth (the player, 11. 11.: "turning a little left or right changed the entrance's
picture by turns on both ends of the tunnel. here the road tunnel does it"): tunel_silnicni_zed_N(_z), _vrch_N and _celo
of the same shapes -- the portal from the eyes is the same, the tile's width round the tube's opening, walls at the
tile's sides up to the hill (CabDrawPortal() of viewport.cpp) -- in the stones the eyes dress it in (CabStoneTexture():
the same picture, made here by the same rule), the road's ground under them the game's (DrawThreeDTunnelEntrance() of
tunnelbridge_cmd.cpp).

    python3 tunel_lodni.py
"""
import io
import json
import math
import os
import struct

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
TILE_M = 14.83765                  # the game's tile, metres (OBJECT_UNITS_PER_METRE)
LEVEL_M = 3.0                      # a level of the game's height (OBJECT_HEIGHT_PER_METRE)
WALL_T = 1.0                       # a wall's and the face's thickness
OVER_M = 0.0                       # their top over the land beside, over the hill: level with it (the player: "higher than the terrain")
TEX_M = 5.0                        # the concrete's texture: 5 x 5 m, seamless
# The set being written: its material and how many metres its picture covers across (U) and up (V).
MAT, TEX_U, TEX_V = 'beton', TEX_M, TEX_M
# The opening on the face: as wide as the tube (12 of the tile's 16 units, CAB_TUNNEL_HALF of viewport.cpp), its walls
# and vault the tube's profile at the land's height of a level (_cab_tunnel_profile: walls 6, vault 12.3 of a 13.3 high
# portal from the eyes), so the opening is the same part of the face as from the eyes.
OPEN_HALF = 6.0 / 16.0 * TILE_M
OPEN_WALL = 6.0 / 13.3 * LEVEL_M
OPEN_TOP = 12.3 / 13.3 * LEVEL_M


# The stones' picture: 96 x 48 pixels, 4 to a world unit (16 to the tile) across, as from the eyes.
STONE_W_M = 96 / 4 * TILE_M / 16
STONE_H_M = 48 / 4 * TILE_M / 16


def stone_png():
    """The stones of the portals from the eyes (CabStoneTexture() of viewport.cpp), pixel for pixel by its rule."""
    w, h = 96, 48
    im = Image.new('RGB', (w, h))
    seed = 12345
    for y in range(h):
        course = y // 6
        for x in range(w):
            seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
            noise = ((seed >> 16) % 21) - 10
            joint = (y % 6) == 5 or ((x + (course & 1) * 6) % 12) == 11
            base = 82 if joint else 128 + ((course * 7 + (x + (course & 1) * 6) // 12 * 13) % 17) - 8
            im.putpixel((x, y), (max(0, min(255, base + noise + 6)), max(0, min(255, base + noise)), max(0, min(255, base + noise - 10))))
    out = io.BytesIO()
    im.save(out, format='PNG', optimize=True)
    return out.getvalue()


def concrete_png():
    """The concrete of zed_lic_beton.glb (its one image), half its size as the lock's."""
    data = open(os.path.join(HERE, 'zed_lic_beton.glb'), 'rb').read()
    n = struct.unpack_from('<I', data, 12)[0]
    j = json.loads(data[20:20 + n])
    bv = j['bufferViews'][j['images'][0]['bufferView']]
    off = 20 + n + 8 + bv.get('byteOffset', 0)
    im = Image.open(io.BytesIO(data[off:off + bv['byteLength']])).convert('RGB').resize((256, 256), Image.LANCZOS)
    out = io.BytesIO()
    im.save(out, format='PNG', optimize=True)
    return out.getvalue()


class Mesh:
    def __init__(self):
        self.prims = {}  # material -> (pos, nrm, uv, idx)

    def poly(self, mat, corners, uvs):
        """A flat face: its corners counter-clockwise seen from outside (a fan from the first), their u, v."""
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
        for k in range(1, len(corners) - 1):
            idx += [base, base + k, base + k + 1]


def mirrored(m):
    """The mesh mirrored across Z: each face's Z negated and its corners the other way round, so it faces out still."""
    out = Mesh()
    for mat, (pos, nrm, uv, idx) in m.prims.items():
        out.prims[mat] = ([(x, y, -z) for x, y, z in pos], [(x, y, -z) for x, y, z in nrm], list(uv),
                          [i for t in range(0, len(idx), 3) for i in (idx[t], idx[t + 2], idx[t + 1])])
    return out


def wall(k):
    """A third of a wing wall, the k-th towards the portal: the land beside rising along +X from k to k + 1 thirds of a level."""
    m = Mesh()
    x0, x1 = -TILE_M / 6, TILE_M / 6
    zi, zo = -WALL_T / 2, WALL_T / 2
    land0, land1 = k * LEVEL_M / 3, (k + 1) * LEVEL_M / 3
    top0, top1 = land0 + OVER_M, land1 + OVER_M
    u0, u1 = (k * TILE_M / 3) / TEX_U, ((k + 1) * TILE_M / 3) / TEX_U

    def v(y):
        return (LEVEL_M + OVER_M - y) / TEX_V
    # The inner face, to the water: from the water (Y 0) up; seen from -Z.
    m.poly(MAT, [(x1, 0.0, zi), (x0, 0.0, zi), (x0, top0, zi), (x1, top1, zi)], [(u1, v(0)), (u0, v(0)), (u0, v(top0)), (u1, v(top1))])
    # The outer face, to the land beside: from the land up; seen from +Z (none when the top is the land's).
    if OVER_M > 0:
        m.poly(MAT, [(x0, land0, zo), (x1, land1, zo), (x1, top1, zo), (x0, top0, zo)], [(u0, v(land0)), (u1, v(land1)), (u1, v(top1)), (u0, v(top0))])
    # The top, a strip along it.
    w = WALL_T / TEX_V
    m.poly(MAT, [(x0, top0, zo), (x1, top1, zo), (x1, top1, zi), (x0, top0, zi)], [(u0, 0.0), (u1, 0.0), (u1, w), (u0, w)])
    # No ends: the thirds meet each other, the canal's end is the water's height and the portal's under the face; one
    # between two thirds hung below the land beside it (the player: "again it draws in front what should be at the back").
    return m


def wall_top(k):
    """Only the top of a third of a wing wall (wall()): the portal seen from behind."""
    m = Mesh()
    x0, x1 = -TILE_M / 6, TILE_M / 6
    zi, zo = -WALL_T / 2, WALL_T / 2
    top0, top1 = k * LEVEL_M / 3 + OVER_M, (k + 1) * LEVEL_M / 3 + OVER_M
    u0, u1 = (k * TILE_M / 3) / TEX_U, ((k + 1) * TILE_M / 3) / TEX_U
    w = WALL_T / TEX_V
    m.poly(MAT, [(x0, top0, zo), (x1, top1, zo), (x1, top1, zi), (x0, top0, zi)], [(u0, 0.0), (u1, 0.0), (u1, w), (u0, w)])
    return m


def opening():
    """The outline of the opening on the face, from its left foot over the vault to its right foot: across X, up Y."""
    pts = [(-OPEN_HALF, 0.0), (-OPEN_HALF, OPEN_WALL)]
    seg = 12
    for i in range(1, seg):
        a = math.pi - math.pi * i / seg
        pts.append((OPEN_HALF * math.cos(a), OPEN_WALL + (OPEN_TOP - OPEN_WALL) * math.sin(a)))
    pts += [(OPEN_HALF, OPEN_WALL), (OPEN_HALF, 0.0)]
    return pts


def face():
    m = Mesh()
    x0, x1 = -TILE_M / 2, TILE_M / 2
    zf, zb = -WALL_T / 2, WALL_T / 2
    top = LEVEL_M + OVER_M

    def u(x):
        return (x - x0) / TEX_U

    def v(y):
        return (top - y) / TEX_V
    # The front, round the opening: its outline and the face's edge joined in strips from the left foot over to the right.
    hole = opening()
    # The face's edge as seen from the front (-Z): its left foot, its left top, ... its right foot, matched to the hole's
    # points by where they are across (each strip a quad from a point of the hole up or out to the edge).
    for i in range(len(hole) - 1):
        (ax, ay), (bx, by) = hole[i], hole[i + 1]
        # Each piece of the outline out to the face's edge: straight up to the top, and the first and last also sideways.
        m.poly(MAT, [(bx, by, zf), (ax, ay, zf), (ax, top, zf), (bx, top, zf)], [(u(bx), v(by)), (u(ax), v(ay)), (u(ax), v(top)), (u(bx), v(top))])
    for side, (hx, hy0, hy1) in ((-1, (-OPEN_HALF, 0.0, top)), (1, (OPEN_HALF, 0.0, top))):
        ex = x0 if side < 0 else x1
        if side < 0:
            m.poly(MAT, [(hx, 0.0, zf), (ex, 0.0, zf), (ex, top, zf), (hx, top, zf)], [(u(hx), v(0)), (u(ex), v(0)), (u(ex), v(top)), (u(hx), v(top))])
        else:
            m.poly(MAT, [(ex, 0.0, zf), (hx, 0.0, zf), (hx, top, zf), (ex, top, zf)], [(u(ex), v(0)), (u(hx), v(0)), (u(hx), v(top)), (u(ex), v(top))])
    # The dark of the opening, a hair before the front so it is drawn on it.
    dark = [(x, y, zf - 0.02) for x, y in hole]
    m.poly('tma', dark, [(0, 0)] * len(dark))
    # The back, only over the hill.
    if OVER_M > 0:
        m.poly(MAT, [(x0, LEVEL_M, zb), (x1, LEVEL_M, zb), (x1, top, zb), (x0, top, zb)], [(u(x1), v(LEVEL_M)), (u(x0), v(LEVEL_M)), (u(x0), v(top)), (u(x1), v(top))])
    # The top.
    w = WALL_T / TEX_V
    m.poly(MAT, [(x0, top, zb), (x1, top, zb), (x1, top, zf), (x0, top, zf)], [(u(x0), 0.0), (u(x1), 0.0), (u(x1), w), (u(x0), w)])
    # No ends: beside the face the wing walls and the hill.
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
    tex_of = {MAT: concrete_png() if MAT == 'beton' else stone_png()}
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
            # The dark of the tunnel seen through its opening.
            m['pbrMetallicRoughness']['baseColorFactor'] = [0.035, 0.035, 0.04, 1.0]
        materials.append(m)
        primitives.append({'attributes': {'POSITION': a, 'NORMAL': a + 1, 'TEXCOORD_0': a + 2}, 'indices': a + 3, 'material': len(materials) - 1})
    gltf = {
        'asset': {'version': '2.0', 'generator': 'tunel_lodni.py (forclaude)'},
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
    global MAT, TEX_U, TEX_V
    for prefix, MAT, TEX_U, TEX_V in (('tunel_lodni', 'beton', TEX_M, TEX_M), ('tunel_silnicni', 'kamen', STONE_W_M, STONE_H_M)):
        for k in range(3):
            write(os.path.join(HERE, f'{prefix}_zed_{k}.glb'), wall(k), f'{prefix}_zed_{k}')
            write(os.path.join(HERE, f'{prefix}_zed_{k}_z.glb'), mirrored(wall(k)), f'{prefix}_zed_{k}_z')
            write(os.path.join(HERE, f'{prefix}_vrch_{k}.glb'), wall_top(k), f'{prefix}_vrch_{k}')
        write(os.path.join(HERE, f'{prefix}_celo.glb'), face(), f'{prefix}_celo')
    print(f'otvor {2 * OPEN_HALF:.2f} m siroky, {OPEN_TOP:.2f} m vysoky')


if __name__ == '__main__':
    main()
