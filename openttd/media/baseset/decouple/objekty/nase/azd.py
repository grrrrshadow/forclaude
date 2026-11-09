#!/usr/bin/env python3
"""
Přejezd AŽD (the player, 8. 11.: "semafory jeden český a přejezd"; 9. 11.: "make it yourself properly, so that it is
straight and of small MB, whole"): the pieces the game stands at a level crossing with our 3D track
(DrawThreeDCrossingPosts() of rail_cmd.cpp), all our own, of boxes, prisms and discs in flat colours -- the player's
SketchUp model of the AŽD crossing at Vranovice had its lights' head on an arm beside the mast and its cross behind it.

    python3 -I -B azd.py

Written beside this script, each also mirrored (_z: the road on its other side, for driving on the left):

  azd_sloupek.glb        the post (výstražník AŽD), its lights out: its foot at the origin, its lights to +x (to the
                         drivers coming), the road at -y. A round mast on a concrete footing; on it, in its middle,
                         the head: a black plate with a white edge, two red lenses side by side and a white one under
                         them between, each under its hood, always there, dull (the player: "it goes out but the light
                         is still there, there must always be a red and a white circle"); over it the St Andrew's cross
                         A32a, white with a red edge, in front of the mast, its back grey; beside the mast, away from the
                         road, the barrier's drive, its axle in front of it, out of the way of the mast and the head.
  azd_svetlo_l.glb       its left red light (as the drivers see it) lit alone, in the post's frame: a disc of its own
                         light over the lens (glTF emissiveFactor), drawn with the post (the player: "only another
                         colour of the light ... one object")
  azd_svetlo_p.glb       its right red light lit (the crossing closed: the two in turn)
  azd_svetlo_b.glb       its white light lit (the crossing open: it blinks slowly)
  azd_zavora_dole.glb    the barrier down: from its axle over the road, red and white, 5 m
  azd_zavora_nahore.glb  the barrier up, straight up from its axle

The lit red a deep one (the player: "darker, this is too orange"). A face of two colours, the cross's (white and red in
front, grey behind), is its front's triangles seen from the front only and its back's 5 mm behind (in one plane the two
fight over which is nearer). glTF's Y up: (x, z, -y) of the post's frame. The rail_cmd.cpp constants THREE_D_POST_*
and THREE_D_BARRIER_* are this script's measures.
"""
import json
import math
import os
import struct

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))

# The post's measures, metres, in its frame (+x to the drivers, +y to the lights' left: away from the road, z up).
MAST_R, MAST_TOP, MAST_SIDES = 0.05, 3.55, 12
FOOT = (-0.3, 0.3, -0.3, 0.95, -0.1, 0.08)           # the concrete footing, under the mast and the drive
PLATE = (0.06, 0.10, -0.32, 0.32, 2.32, 2.98)          # the head's black plate
EDGE = 0.03                                            # its white edge
HOLDER = (0.04, 0.06, -0.06, 0.06, 2.4, 2.9)           # the head's holder on the mast
LENS_X = 0.104                                         # the lenses, before the plate's white edge
LENSES = {'l': (-0.16, 2.80), 'p': (0.16, 2.80), 'b': (0.0, 2.50)}  # y, z: the drivers' left and right red, the white
LENS_R = 0.1
HOOD_R, HOOD_X = 0.12, 0.2                             # a lens's hood: a half tube over it, so far out
CROSS_Z, CROSS_LEN, CROSS_W, CROSS_RIM = 3.42, 1.2, 0.15, 0.03   # the cross A32a: its middle, each board, its red edge
CROSS_SLOPE = 30.0                                     # each board's slope, degrees
CROSS_X = (0.06, 0.08)                                 # the back board's back and front; the front board 2 cm before
DRIVE = (-0.16, 0.11, 0.48, 0.86, 0.08, 1.25)          # the barrier's drive
PIVOT = (0.18, 0.68, 1.05)                             # the barrier's axle: before the drive, up beside the cross's arms
BOOM_LEN, BOOM_HALF = 5.0, 0.045
STRIPE = 0.5
BACK_GAP = 0.005                                       # a face's back of its own colour behind its front

MATS = {
    'beton': {'baseColorFactor': [0.62, 0.62, 0.6, 1.0]},
    'stozar': {'baseColorFactor': [0.66, 0.67, 0.69, 1.0]},
    'pohon': {'baseColorFactor': [0.56, 0.58, 0.6, 1.0]},
    'cerna': {'baseColorFactor': [0.08, 0.08, 0.08, 1.0]},
    'cervena': {'baseColorFactor': [0.78, 0.08, 0.07, 1.0]},
    'bila': {'baseColorFactor': [0.93, 0.93, 0.92, 1.0]},
    'zavazi': {'baseColorFactor': [0.18, 0.18, 0.19, 1.0]},
    'cocka_cervena': {'baseColorFactor': [0.3, 0.02, 0.03, 1.0]},     # a red lens out
    'cocka_bila': {'baseColorFactor': [0.62, 0.62, 0.6, 1.0]},         # the white one out
    'svetlo_cervene': {'baseColorFactor': [0.78, 0.0, 0.02, 1.0], 'emissiveFactor': [0.78, 0.0, 0.02]},
    'svetlo_bile': {'baseColorFactor': [1.0, 0.98, 0.9, 1.0], 'emissiveFactor': [1.0, 0.98, 0.9]},
}


def box(x0, x1, y0, y1, z0, z1):
    """The six faces of a box, triangles counter-clockwise from outside."""
    c = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    tris = []
    for a, b, cc, d in quads:
        tris.append([c[a], c[b], c[cc]])
        tris.append([c[a], c[cc], c[d]])
    return np.array(tris, float)


def quad_x(x, y0, y1, z0, z1):
    """A rectangle facing +x."""
    return np.array([[(x, y0, z0), (x, y1, z0), (x, y1, z1)], [(x, y0, z0), (x, y1, z1), (x, y0, z1)]], float)


def disc(cx, cy, cz, r, n=12):
    """A disc facing +x at a point."""
    tris = []
    for k in range(n):
        a0, a1 = 2 * math.pi * k / n, 2 * math.pi * (k + 1) / n
        tris.append([(cx, cy, cz), (cx, cy + r * math.cos(a0), cz + r * math.sin(a0)), (cx, cy + r * math.cos(a1), cz + r * math.sin(a1))])
    return np.array(tris, float)


def prism(r, z0, z1, n):
    """A round mast: n sides and its top, its foot open (on the footing)."""
    tris = []
    for k in range(n):
        a0, a1 = 2 * math.pi * k / n, 2 * math.pi * (k + 1) / n
        p0, p1 = (r * math.cos(a0), r * math.sin(a0)), (r * math.cos(a1), r * math.sin(a1))
        tris.append([(*p0, z0), (*p1, z0), (*p1, z1)])
        tris.append([(*p0, z0), (*p1, z1), (*p0, z1)])
        tris.append([(0, 0, z1), (*p0, z1), (*p1, z1)])
    return np.array(tris, float)


def hood(cy, cz):
    """A lens's hood: the upper half of a tube round it, from the plate out."""
    tris, n = [], 6
    for k in range(n):
        a0, a1 = math.pi * k / n, math.pi * (k + 1) / n
        q0 = (cy + HOOD_R * math.cos(a0), cz + HOOD_R * math.sin(a0))
        q1 = (cy + HOOD_R * math.cos(a1), cz + HOOD_R * math.sin(a1))
        x0, x1 = PLATE[1], HOOD_X
        tris.append([(x0, *q0), (x1, *q0), (x1, *q1)])
        tris.append([(x0, *q0), (x1, *q1), (x0, *q1)])
    return np.array(tris, float)


def board(slope, xb, xf):
    """One board of the cross, sloping so many degrees, from xb to xf: its front red with a white middle 2.5 mm before it
    (seen from the front only), its sides and back grey 5 mm behind (seen from behind only)."""
    a = math.radians(slope)
    u, v = np.array([0.0, math.cos(a), math.sin(a)]), np.array([0.0, -math.sin(a), math.cos(a)])
    mid = np.array([0.0, 0.0, CROSS_Z])

    def rect(x, hl, hw):
        c = [mid + s * hl * u + t * hw * v for s, t in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        c = [(x, p[1], p[2]) for p in c]
        return np.array([[c[0], c[1], c[2]], [c[0], c[2], c[3]]], float)
    hl, hw = CROSS_LEN / 2, CROSS_W / 2
    front = rect(xf, hl, hw)
    if np.cross(front[0, 1] - front[0, 0], front[0, 2] - front[0, 0])[0] < 0:
        front = front[:, ::-1]
    white = rect(xf + 0.0025, hl - CROSS_RIM, hw - CROSS_RIM)
    if np.cross(white[0, 1] - white[0, 0], white[0, 2] - white[0, 0])[0] < 0:
        white = white[:, ::-1]
    # its back and its four edges: a box round the board, faces turned out, then pulled 5 mm in at its front
    back = rect(xb, hl, hw)[:, ::-1]
    if np.cross(back[0, 1] - back[0, 0], back[0, 2] - back[0, 0])[0] > 0:
        back = back[:, ::-1]
    corners = [mid + s * hl * u + t * hw * v for s, t in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
    edges = []
    for i in range(4):
        p, q = corners[i], corners[(i + 1) % 4]
        e = np.array([[(xb, p[1], p[2]), (xf - BACK_GAP, p[1], p[2]), (xf - BACK_GAP, q[1], q[2])],
                      [(xb, p[1], p[2]), (xf - BACK_GAP, q[1], q[2]), (xb, q[1], q[2])]], float)
        out = (p + q) / 2 - mid
        n = np.cross(e[0, 1] - e[0, 0], e[0, 2] - e[0, 0])
        if np.dot(n[1:], out[1:]) < 0:
            e = e[:, ::-1]
        edges.append(e)
    return [('cervena|1', front), ('bila|1', white), ('stozar|1', np.concatenate([back] + edges))]


def post():
    parts = [('beton', box(*FOOT)), ('stozar', prism(MAST_R, FOOT[5], MAST_TOP, MAST_SIDES)),
             ('pohon', box(*DRIVE)), ('pohon', box(DRIVE[1], PIVOT[0] - BOOM_HALF, PIVOT[1] - 0.03, PIVOT[1] + 0.03, PIVOT[2] - 0.03, PIVOT[2] + 0.03)),
             ('stozar', box(*HOLDER)), ('cerna', box(*PLATE))]
    x0, x1, y0, y1, z0, z1 = PLATE
    xe = x1 + 0.001
    parts += [('bila', quad_x(xe, y0, y1, z1 - EDGE, z1)), ('bila', quad_x(xe, y0, y1, z0, z0 + EDGE)),
              ('bila', quad_x(xe, y0, y0 + EDGE, z0 + EDGE, z1 - EDGE)), ('bila', quad_x(xe, y1 - EDGE, y1, z0 + EDGE, z1 - EDGE))]
    for k, (ly, lz) in LENSES.items():
        parts.append(('cocka_bila' if k == 'b' else 'cocka_cervena', disc(LENS_X, ly, lz, LENS_R)))
        parts.append(('cerna', hood(ly, lz)))
    parts += board(CROSS_SLOPE, *CROSS_X)
    parts += board(-CROSS_SLOPE, CROSS_X[1] + 0.002, CROSS_X[1] + 0.02)
    return parts


def boom(up):
    """The barrier from its axle: down across the road (-y), or straight up; red and white stripes, a weight behind."""
    parts = []
    px, py, pz = PIVOT
    h = BOOM_HALF
    n = int(round(BOOM_LEN / STRIPE))
    for k in range(n):
        a, b = k * STRIPE, (k + 1) * STRIPE
        if up:
            parts.append(('cervena' if k % 2 == 0 else 'bila', box(px - h, px + h, py - h, py + h, pz + a, pz + b)))
        else:
            parts.append(('cervena' if k % 2 == 0 else 'bila', box(px - h, px + h, py - b, py - a, pz - h, pz + h)))
    w = 0.06
    if up:
        parts.append(('zavazi', box(px - w, px + w, py - w, py + w, pz - 0.55, pz - 0.05)))
    else:
        parts.append(('zavazi', box(px - w, px + w, py + 0.05, py + 0.55, pz - w, pz + w)))
    return parts


def write(path, name, parts, specs=None):
    """parts: (material, triangles (n, 3, 3) in the post's frame, z up); flat normals; glTF Y up (x, z, -y). specs: a
    material's glTF factors by its name (baseColorFactor, emissiveFactor) over MATS (navestidlo.py's own).
    A material 'name|1' is that one seen from the front of its triangles only (a face's side of its own colour)."""
    blob = bytearray()
    views, accessors, materials, primitives = [], [], [], []

    def view(data, target):
        while len(blob) % 4:
            blob.append(0)
        views.append({'buffer': 0, 'byteOffset': len(blob), 'byteLength': len(data), 'target': target})
        blob.extend(data)
        return len(views) - 1
    by_mat = {}
    for mat, tris in parts:
        by_mat.setdefault(mat, []).append(tris)
    for mat, chunks in by_mat.items():
        t = np.concatenate(chunks)
        n = np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0])
        ln = np.linalg.norm(n, axis=1)
        keep = ln > 1e-12
        t, n = t[keep], n[keep] / ln[keep][:, None]
        pos = t.reshape(-1, 3)
        nrm = np.repeat(n, 3, axis=0)
        g_pos = np.stack([pos[:, 0], pos[:, 2], -pos[:, 1]], 1).astype(np.float32)
        g_nrm = np.stack([nrm[:, 0], nrm[:, 2], -nrm[:, 1]], 1).astype(np.float32)
        # The corners shared where their place and their normal are the same (a flat face's triangles).
        rows = np.concatenate([g_pos, g_nrm], 1)
        rows, idx = np.unique(np.round(rows, 5), axis=0, return_inverse=True)
        g_pos, g_nrm = rows[:, :3].astype(np.float32).copy(), rows[:, 3:].astype(np.float32).copy()
        idx = idx.ravel().astype(np.uint32)
        a = len(accessors)
        accessors += [
            {'bufferView': view(g_pos.tobytes(), 34962), 'componentType': 5126, 'count': len(g_pos), 'type': 'VEC3',
             'min': g_pos.min(0).tolist(), 'max': g_pos.max(0).tolist()},
            {'bufferView': view(g_nrm.tobytes(), 34962), 'componentType': 5126, 'count': len(g_nrm), 'type': 'VEC3'},
            {'bufferView': view(idx.tobytes(), 34963), 'componentType': 5125, 'count': len(idx), 'type': 'SCALAR'},
        ]
        base, one_side = (mat[:-2], True) if mat.endswith('|1') else (mat, False)
        spec = (specs or {}).get(base) or MATS.get(base, {'baseColorFactor': [0.8, 0.8, 0.8, 1.0]})
        m = {'name': f'{name}_{mat}', 'pbrMetallicRoughness': {'metallicFactor': 0.0, 'roughnessFactor': 1.0, 'baseColorFactor': spec['baseColorFactor']},
             'doubleSided': not one_side}
        if 'emissiveFactor' in spec:
            m['emissiveFactor'] = spec['emissiveFactor']
        materials.append(m)
        primitives.append({'attributes': {'POSITION': a, 'NORMAL': a + 1}, 'indices': a + 2, 'material': len(materials) - 1})
    while len(blob) % 4:
        blob.append(0)
    gltf = {'asset': {'version': '2.0', 'generator': 'azd.py (forclaude)'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
            'nodes': [{'mesh': 0, 'name': name}], 'meshes': [{'name': name, 'primitives': primitives}],
            'materials': materials, 'accessors': accessors, 'bufferViews': views, 'buffers': [{'byteLength': len(blob)}]}
    js = json.dumps(gltf, separators=(',', ':')).encode()
    while len(js) % 4:
        js += b' '
    out = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob))
    out += struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(blob), 0x004E4942) + bytes(blob)
    with open(path, 'wb') as f:
        f.write(out)
    print(f'{os.path.basename(path)}: {sum(len(c) for _, c in parts)} triangles, {len(out)} bytes')


def mirrored(parts):
    """The same with y the other way (the road on the other side): its triangles turned to keep their faces out."""
    out = []
    for mat, t in parts:
        m = t.copy()
        m[..., 1] *= -1
        out.append((mat, m[:, ::-1]))
    return out


def main():
    lights = {k: [('svetlo_bile' if k == 'b' else 'svetlo_cervene', disc(LENS_X + 0.004, ly, lz, LENS_R + 0.005))] for k, (ly, lz) in LENSES.items()}
    pieces = {'azd_sloupek': post(), 'azd_svetlo_l': lights['l'], 'azd_svetlo_p': lights['p'], 'azd_svetlo_b': lights['b'],
              'azd_zavora_dole': boom(False), 'azd_zavora_nahore': boom(True)}
    for name, parts in pieces.items():
        write(os.path.join(HERE, name + '.glb'), name, parts)
        write(os.path.join(HERE, name + '_z.glb'), name + '_z', mirrored(parts))
    p = np.concatenate([t.reshape(-1, 3) for _, t in pieces['azd_sloupek']])
    print('sloupek', np.round(p.min(0), 3), np.round(p.max(0), 3))


if __name__ == '__main__':
    main()
