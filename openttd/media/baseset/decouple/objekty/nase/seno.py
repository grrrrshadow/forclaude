#!/usr/bin/env python3
"""
Seno (the player, 21. 10.: "in a year there will be marijuana hay"): a heap of dried marijuana, our own geometry and
texture. Written as seno.glb beside this script: glTF's Y up, the foot at the origin, 1 m high and 1 m across (the
index's "meritko" gives its size in metres), a 32x32 texture of dry straw, stalks and a few dark leaves.

    python3 seno.py
"""
import io
import json
import math
import os
import struct

from PIL import Image

SEGMENTS = 24
RINGS = 8


def texture():
    im = Image.new('RGB', (32, 32))
    for y in range(32):
        for x in range(32):
            # Straw: stripes along v, lighter and darker stalks, here and there a dried brown leaf.
            n = ((x * 13 + y * 7) % 11) - 5
            stalk = 1.0 if (x % 4) < 2 else 0.82
            r, g, b = 196 * stalk + n * 3, 168 * stalk + n * 3, 92 * stalk + n * 2
            if (x * 5 + y * 3) % 17 == 0:
                r, g, b = 122, 98, 52
            im.putpixel((x, y), (int(r), int(g), int(b)))
    out = io.BytesIO()
    im.save(out, format='PNG')
    return out.getvalue()


def main():
    pos, nrm, uv, idx = [], [], [], []
    # A heap: a dome a little flattened, its rim bulging out, the top a bit off the middle -- rings from the foot up.
    for j in range(RINGS + 1):
        t = j / RINGS
        a_up = t * math.pi / 2
        for i in range(SEGMENTS + 1):
            a = 2 * math.pi * i / SEGMENTS
            wobble = 1.0 + 0.06 * math.sin(3 * a + 1.3) + 0.04 * math.sin(7 * a)
            r = 0.5 * math.cos(a_up) * wobble * (1.0 + 0.08 * math.sin(a_up * 2))
            y = 1.0 * math.sin(a_up)
            ox = 0.06 * t
            pos.extend([r * math.cos(a) + ox, y, r * math.sin(a)])
            nx, ny, nz = math.cos(a_up) * math.cos(a), math.sin(a_up) * 1.6, math.cos(a_up) * math.sin(a)
            l = math.sqrt(nx * nx + ny * ny + nz * nz)
            nrm.extend([nx / l, ny / l, nz / l])
            uv.extend([i / SEGMENTS * 0.98 + 0.01, t * 0.98 + 0.01])
    for j in range(RINGS):
        lo, hi = j * (SEGMENTS + 1), (j + 1) * (SEGMENTS + 1)
        for i in range(SEGMENTS):
            a, b, c, d = lo + i, lo + i + 1, hi + i + 1, hi + i
            idx.extend([a, c, b, a, d, c])

    png = texture()
    blob = bytearray()

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

    views = []
    corners = len(pos) // 3
    p_view = view(struct.pack('<%df' % len(pos), *pos), 34962)
    n_view = view(struct.pack('<%df' % len(nrm), *nrm), 34962)
    t_view = view(struct.pack('<%df' % len(uv), *uv), 34962)
    i_view = view(struct.pack('<%dH' % len(idx), *idx), 34963)
    img_view = view(png)
    xs, ys, zs = pos[0::3], pos[1::3], pos[2::3]
    gltf = {
        'asset': {'version': '2.0', 'generator': 'seno.py (forclaude)'},
        'scene': 0,
        'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0, 'name': 'seno'}],
        'meshes': [{'name': 'seno', 'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2}, 'indices': 3, 'material': 0}]}],
        'materials': [{'name': 'slama', 'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}, 'metallicFactor': 0.0, 'roughnessFactor': 1.0}}],
        'textures': [{'source': 0, 'sampler': 0}],
        'samplers': [{'magFilter': 9728, 'minFilter': 9728}],
        'images': [{'bufferView': img_view, 'mimeType': 'image/png'}],
        'accessors': [
            {'bufferView': p_view, 'componentType': 5126, 'count': corners, 'type': 'VEC3', 'min': [min(xs), min(ys), min(zs)], 'max': [max(xs), max(ys), max(zs)]},
            {'bufferView': n_view, 'componentType': 5126, 'count': corners, 'type': 'VEC3'},
            {'bufferView': t_view, 'componentType': 5126, 'count': corners, 'type': 'VEC2'},
            {'bufferView': i_view, 'componentType': 5123, 'count': len(idx), 'type': 'SCALAR'},
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
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'seno.glb')
    with open(path, 'wb') as f:
        f.write(out)
    print(f'{path}: {len(idx) // 3} triangles, {corners} corners, {len(out)} bytes')


if __name__ == '__main__':
    main()
