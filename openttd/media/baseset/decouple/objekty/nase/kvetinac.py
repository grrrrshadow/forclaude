#!/usr/bin/env python3
"""
Květináč (the player, 21. 10.: "udělej květináč"): a clay pot with a rim and soil on top, our own geometry and texture.
Written as kvetinac.glb beside this script: glTF's Y up, the foot at the origin, 1 m high and 1 m across the rim (the
index's "meritko" gives its size in metres), a 32x32 texture -- clay on the left half, soil on the right.

    python3 kvetinac.py
"""
import io
import json
import math
import os
import struct

from PIL import Image

SEGMENTS = 20
FOOT_R, BODY_TOP_R, RIM_R, RIM_IN_R = 0.36, 0.46, 0.50, 0.44
BODY_TOP, RIM_BOTTOM, TOP, SOIL = 0.82, 0.82, 1.0, 0.92


def texture():
    im = Image.new('RGB', (32, 32))
    for y in range(32):
        for x in range(32):
            if x < 16:
                # Clay: lighter towards the top of the pot, a faint band.
                k = 1.0 - y / 64.0
                im.putpixel((x, y), (int(178 * k), int(92 * k), int(58 * k)))
            else:
                n = ((x * 7 + y * 13) % 5) * 4
                im.putpixel((x, y), (58 + n, 40 + n // 2, 28 + n // 2))
    out = io.BytesIO()
    im.save(out, format='PNG')
    return out.getvalue()


def main():
    pos, nrm, uv, idx = [], [], [], []

    def ring(r, y, ny, nr, v):
        """A ring of corners at height y, radius r, its normals leaning by (nr out, ny up); their first index."""
        first = len(pos) // 3
        for i in range(SEGMENTS + 1):
            a = 2 * math.pi * i / SEGMENTS
            c, s = math.cos(a), math.sin(a)
            pos.extend([r * c, y, r * s])
            l = math.hypot(nr, ny)
            nrm.extend([nr / l * c, ny / l, nr / l * s])
            uv.extend([0.02 + 0.46 * i / SEGMENTS, v])
        return first

    def band(lo, hi):
        for i in range(SEGMENTS):
            a, b, c, d = lo + i, lo + i + 1, hi + i + 1, hi + i
            idx.extend([a, c, b, a, d, c])

    def disc(r, y, up, u0):
        centre = len(pos) // 3
        pos.extend([0, y, 0])
        nrm.extend([0, 1 if up else -1, 0])
        uv.extend([u0 + 0.25, 0.5])
        first = len(pos) // 3
        for i in range(SEGMENTS + 1):
            a = 2 * math.pi * i / SEGMENTS
            pos.extend([r * math.cos(a), y, r * math.sin(a)])
            nrm.extend([0, 1 if up else -1, 0])
            uv.extend([u0 + 0.25 + 0.22 * math.cos(a), 0.5 + 0.22 * math.sin(a)])
        for i in range(SEGMENTS):
            if up:
                idx.extend([centre, first + i + 1, first + i])
            else:
                idx.extend([centre, first + i, first + i + 1])

    slope = (BODY_TOP_R - FOOT_R) / BODY_TOP
    # The body, leaning out as it rises.
    band(ring(FOOT_R, 0.0, slope, 1.0, 0.95), ring(BODY_TOP_R, BODY_TOP, slope, 1.0, 0.25))
    # The rim: out, up its face, over its top and in.
    band(ring(BODY_TOP_R, RIM_BOTTOM, -1.0, 0.0, 0.22), ring(RIM_R, RIM_BOTTOM, -1.0, 0.0, 0.2))
    band(ring(RIM_R, RIM_BOTTOM, 0.0, 1.0, 0.2), ring(RIM_R, TOP, 0.0, 1.0, 0.05))
    band(ring(RIM_R, TOP, 1.0, 0.0, 0.05), ring(RIM_IN_R, TOP, 1.0, 0.0, 0.04))
    band(ring(RIM_IN_R, TOP, 0.0, -1.0, 0.04), ring(RIM_IN_R, SOIL, 0.0, -1.0, 0.1))
    # The soil inside, the bottom under it.
    disc(RIM_IN_R, SOIL, True, 0.5)
    disc(FOOT_R, 0.0, False, 0.0)

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
        'asset': {'version': '2.0', 'generator': 'kvetinac.py (forclaude)'},
        'scene': 0,
        'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0, 'name': 'kvetinac'}],
        'meshes': [{'name': 'kvetinac', 'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2}, 'indices': 3, 'material': 0}]}],
        'materials': [{'name': 'hlina', 'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}, 'metallicFactor': 0.0, 'roughnessFactor': 1.0}}],
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
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'kvetinac.glb')
    with open(path, 'wb') as f:
        f.write(out)
    print(f'{path}: {len(idx) // 3} triangles, {corners} corners, {len(out)} bytes')


if __name__ == '__main__':
    main()
