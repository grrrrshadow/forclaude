#!/usr/bin/env python3
"""
Přejezd AŽD (the player, 8. 11.: "semafory jeden český a přejezd"; "1 jo": the posts of the AŽD level crossing from the
player's model, the barriers our own so they go down): the pieces the game stands at a level crossing with our 3D track
(DrawThreeDCrossingPosts() of rail_cmd.cpp). Of the model "Přejezd AŽD Vranovice" (3D Warehouse, SketchUp 2021) its post
with the warning lights, the St Andrew's cross over them, the barrier's drive and the footing -- read by skp_cti.py
(grrrrf hra/temata/nastroje/) into an npz:

    python3 -I skp_cti.py AZD.skp azd.npz
    python3 azd.py azd.npz

Written beside this script, each also mirrored (_z: the road on its other side, for driving on the left):

  azd_sloupek.glb        the post, its lights out: its foot at the origin, its lights to +x (to the drivers coming), the
                         road at -y; its two red lenses and its white one always there, dull (the player, 9. 11.: "it
                         goes out but the light is still there, there must always be a red and a white circle")
  azd_svetlo_l.glb       its left red light lit alone, in the post's frame: a disc of its own light over the lens (glTF
                         emissiveFactor), drawn with the post (the player, 9. 11.: "only another colour of the light ... one
                         object": not the whole post again for each light)
  azd_svetlo_p.glb       its right red light lit (the crossing closed: the two in turn)
  azd_svetlo_b.glb       its white light lit (the crossing open: it blinks slowly)
  azd_zavora_dole.glb    the barrier down: from its drive over the road, red and white, 5 m
  azd_zavora_nahore.glb  the barrier up, straight up from its drive

The post's materials their colours (the model's textures of grey metal flat grey), a face's front and back each its own
(skp_cti.py's FM and FMZ; a face of two seen from each side as it is: two triangles, each seen from its side only, the
back's 5 mm behind -- in the same plane the two fought over which is nearer, the front's red only specks). Its
St Andrew's cross moved from behind the mast to the front of it (the player, 9. 11.: "the sign is turned, the cross":
its red and white face to the drivers, the mast hid it). Its lenses our own discs a little in
front of its black face (the model's red lenses lie in the face's very plane and its white one is not there at all), where
its hoods are: dull red and dull white, the lit one a disc of a deep red (the player: "darker, this is too orange") or of
white over it.
glTF's Y up: (x, z, -y) of the model's frame.
"""
import json
import math
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
# The model's groups (skp_cti.py's FG) of the post on the west: its mast, its head with the lights, its cross; its drive
# and its footing are of groups with the other post's (4, 5), taken where they are by it.
POST_GROUPS = (3, 6, 7)
MAST_GROUP, HEAD_GROUP, CROSS_GROUP = 3, 6, 7
BACK_GAP = 0.005                   # a face's back of its own colour behind its front, metres
SHARED_GROUPS = (4, 5)
POLE_X, POLE_Y = -7.69, -3.99      # the mast's middle in the model, metres
LIGHTS_FACE = -1.0                 # its lights face -x in the model: turned half round to +x
LENS_X = -7.843                    # the lenses' plane in the model (its black face's)
# y, z of each lens in the model: the red ones its own ([Color_A01], [Color_A09]), the white one under its hood between them
LENSES = {'l': (-4.326, 3.033), 'p': (-4.651, 3.033), 'bile': (-4.488, 2.707)}
LENS_R = 0.12
LENS_MATS = ('[Color_A01]', '[Color_A09]')  # the model's red lenses: in its face's plane, ours instead
PIVOT = (0.06, 0.25, 1.45)         # the barrier's pivot on its drive, in the post's frame (lights to +x)
BOOM_LEN, BOOM_HALF = 5.0, 0.045
STRIPE = 0.5
COLOURS = {
    'seda': [0.68, 0.69, 0.71, 1.0],
    'seda1': [0.68, 0.69, 0.71, 1.0],
    '[Color_003]': [0.67, 0.67, 0.67, 1.0],
    '[Color_A01]': [0.78, 0.05, 0.05, 1.0],   # the cross's red frame (the head's red lenses ours, LENS_MATS)
    '[Color_001]': [0.93, 0.93, 0.92, 1.0],
    '[Color_008]': [0.12, 0.12, 0.12, 1.0],
    'zluta': [0.92, 0.8, 0.12, 1.0],
    'vychozi': [0.92, 0.92, 0.92, 1.0],
}


def to_post(p):
    """A point of the model into the post's frame: its mast's foot the origin, its lights to +x."""
    x, y, z = p[..., 0] - POLE_X, p[..., 1] - POLE_Y, p[..., 2]
    return np.stack([x * LIGHTS_FACE, y * LIGHTS_FACE, z], axis=-1)


def box(x0, x1, y0, y1, z0, z1):
    """The six faces of a box, triangles counter-clockwise from outside."""
    c = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    tris = []
    for a, b, cc, d in quads:
        tris.append([c[a], c[b], c[cc]])
        tris.append([c[a], c[cc], c[d]])
    return np.array(tris, float)


def disc(cx, cy, cz, r, n=12):
    """A disc facing +x at a point."""
    tris = []
    for k in range(n):
        a0, a1 = 2 * math.pi * k / n, 2 * math.pi * (k + 1) / n
        tris.append([(cx, cy, cz), (cx, cy + r * math.cos(a0), cz + r * math.sin(a0)), (cx, cy + r * math.cos(a1), cz + r * math.sin(a1))])
    return np.array(tris, float)


def boom(up):
    """The barrier from its pivot: down across the road (-y), or straight up; red and white stripes, a weight behind."""
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
    w = 0.08
    if up:
        parts.append(('zavazi', box(px - w, px + w, py - w, py + w, pz - 0.55, pz - 0.05)))
    else:
        parts.append(('zavazi', box(px - w, px + w, py + 0.05, py + 0.55, pz - w, pz + w)))
    return parts


MATS = {
    'cervena': {'baseColorFactor': [0.78, 0.08, 0.07, 1.0]},
    'bila': {'baseColorFactor': [0.93, 0.93, 0.92, 1.0]},
    'zavazi': {'baseColorFactor': [0.18, 0.18, 0.19, 1.0]},
    'cocka_cervena': {'baseColorFactor': [0.3, 0.02, 0.03, 1.0]},     # a red lens out
    'cocka_bila': {'baseColorFactor': [0.62, 0.62, 0.6, 1.0]},         # the white one out
    'svetlo_cervene': {'baseColorFactor': [0.78, 0.0, 0.02, 1.0], 'emissiveFactor': [0.78, 0.0, 0.02]},
    'svetlo_bile': {'baseColorFactor': [1.0, 0.98, 0.9, 1.0], 'emissiveFactor': [1.0, 0.98, 0.9]},
}


def write(path, name, parts, specs=None):
    """parts: (material, triangles (n, 3, 3) in the post's frame, z up); flat normals; glTF Y up (x, z, -y). specs: a
    material's glTF factors by its name (baseColorFactor, emissiveFactor) over MATS and COLOURS (navestidlo.py's own).
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
        spec = (specs or {}).get(base) or MATS.get(base, {'baseColorFactor': COLOURS.get(base, [0.8, 0.8, 0.8, 1.0])})
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
    d = np.load(sys.argv[1], allow_pickle=False)
    V, F, FM, FG = d['V'], d['F'], d['FM'], d['FG']
    FMZ = d['FMZ'] if 'FMZ' in d.files else FM   # the faces' backs (skp_cti.py of 9. 11.)
    names = [m['jmeno'] for m in json.loads(str(d['mats']))]
    T = V[F].astype(float)
    near = np.abs(T[:, :, 0].mean(1) - POLE_X) < 1.5
    keep = np.isin(FG, POST_GROUPS) | (np.isin(FG, SHARED_GROUPS) & near)
    # The cross before the mast, its back a centimetre from it (its lights' way -x in the model).
    cross = keep & (FG == CROSS_GROUP)
    mast_front = T[keep & (FG == MAST_GROUP)][..., 0].min()
    T[cross, :, 0] += mast_front - 0.01 - T[cross][..., 0].max()
    # The head's red lenses lie in its face's plane: ours instead (the cross's red frame is of the same material).
    lens_mats = [names.index(n) for n in LENS_MATS if n in names]
    keep &= ~((FG == HEAD_GROUP) & np.isin(FM, lens_mats))
    post = []
    for a, b in sorted(set(zip(FM[keep].tolist(), FMZ[keep].tolist()))):
        tris = to_post(T[keep & (FM == a) & (FMZ == b)])  # a half turn: the faces keep their winding
        if a == b:
            post.append((names[a], tris))
        else:
            n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
            n /= np.maximum(np.linalg.norm(n, axis=1), 1e-12)[:, None]
            post.append((names[a] + '|1', tris))
            post.append((names[b] + '|1', tris[:, ::-1] - BACK_GAP * n[:, None, :]))
    lights = {}
    for k, (ly, lz) in LENSES.items():
        p = to_post(np.array([LENS_X, ly, lz]))
        post.append(('cocka_bila' if k == 'bile' else 'cocka_cervena', disc(p[0] + 0.01, p[1], p[2], LENS_R)))
        lights[k] = [('svetlo_bile' if k == 'bile' else 'svetlo_cervene', disc(p[0] + 0.02, p[1], p[2], LENS_R + 0.005))]
        print('cocka', k, np.round(p, 3))
    pieces = {'azd_sloupek': post, 'azd_svetlo_l': lights['l'], 'azd_svetlo_p': lights['p'], 'azd_svetlo_b': lights['bile'],
              'azd_zavora_dole': boom(False), 'azd_zavora_nahore': boom(True)}
    for name, parts in pieces.items():
        write(os.path.join(HERE, name + '.glb'), name, parts)
        write(os.path.join(HERE, name + '_z.glb'), name + '_z', mirrored(parts))
    lo = np.concatenate([t.reshape(-1, 3) for _, t in post]).min(0)
    hi = np.concatenate([t.reshape(-1, 3) for _, t in post]).max(0)
    print('sloupek', np.round(lo, 2), np.round(hi, 2))


if __name__ == '__main__':
    main()
