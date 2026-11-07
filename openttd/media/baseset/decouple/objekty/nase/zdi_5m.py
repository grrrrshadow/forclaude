"""Our retaining wall faces at one texture tile a level (5 m), of bricks and of stone.

The texture is one tile of DJMaesen's "brick textures" (Sketchfab, CC BY 4.0), its base colour, its big shading over
the rows taken out, lightened for the game's light (as the colleague's walls of objekty/vyplne were), 512 x 512 px for
5 x 5 m. The player, 4. 11.: "one row of the texture one level of the game's height, then it cannot even repeat upwards";
"the bricks to go on at the corner": the game lays it three times along a tile's edge, so it goes on round the corners
of two walls, no pier between (DrawEyeRetainingWalls() of viewport.cpp).
  python3 -I zdi_5m.py <brick_textures.glb of grrrrf modely-zipy/2026-11-04-zdi-popinavky-lampy> <out dir>
"""
import io, json, struct, sys

import numpy as np
from PIL import Image

SRC, OUT = sys.argv[1:3]
# The mean lightness of each material's tile (0-255).
LIGHTNESS = {"cihly": 87.0, "kamen": 93.0}
TILE_M = 5.0
SIZE = 512


def glb_image(path, index):
    data = open(path, "rb").read()
    n = struct.unpack_from("<I", data, 12)[0]
    j = json.loads(data[20:20 + n])
    bv = j["bufferViews"][j["images"][index]["bufferView"]]
    off = 20 + n + 8 + bv.get("byteOffset", 0)
    return Image.open(io.BytesIO(data[off:off + bv["byteLength"]])).convert("RGB")


def tile(src_index, lightness):
    img = np.asarray(glb_image(SRC, src_index), dtype=np.float64)
    lum = img @ np.array([0.299, 0.587, 0.114])
    # The rows' big shading out: each row by the mean lightness about it (a few rows of bricks), wrapped round so the
    # tile stays seamless.
    rows = lum.mean(axis=1)
    k = 40
    padded = np.concatenate([rows[-3 * k:], rows, rows[:3 * k]])
    x = np.arange(-3 * k, 3 * k + 1)
    g = np.exp(-0.5 * (x / k) ** 2)
    g /= g.sum()
    smooth = np.convolve(padded, g, mode="same")[3 * k:-3 * k]
    flat = img * (rows.mean() / smooth)[:, None, None]
    # Lightened for the game's light.
    flat *= lightness / (flat @ np.array([0.299, 0.587, 0.114])).mean()
    out = Image.fromarray(np.clip(flat, 0, 255).astype(np.uint8)).resize((SIZE, SIZE), Image.LANCZOS)
    return out


def glb(path, quads, png_bytes, name):
    """quads: lists of 4 corners (x, y, z metres) counter-clockwise seen from outside, their normal, their u, v."""
    pos, nrm, uv, idx = [], [], [], []
    for corners, normal, uvs in quads:
        base = len(pos)
        for c, t in zip(corners, uvs):
            pos.append(c)
            nrm.append(normal)
            uv.append(t)
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    blob = bytearray()

    def view(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        off = len(blob)
        blob.extend(data)
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target:
            v["target"] = target
        views.append(v)
        return len(views) - 1

    views = []
    p = np.array(pos, dtype=np.float32)
    acc = [
        {"bufferView": view(p.tobytes(), 34962), "componentType": 5126, "count": len(pos), "type": "VEC3", "min": p.min(axis=0).tolist(), "max": p.max(axis=0).tolist()},
        {"bufferView": view(np.array(nrm, dtype=np.float32).tobytes(), 34962), "componentType": 5126, "count": len(nrm), "type": "VEC3"},
        {"bufferView": view(np.array(uv, dtype=np.float32).tobytes(), 34962), "componentType": 5126, "count": len(uv), "type": "VEC2"},
        {"bufferView": view(np.array(idx, dtype=np.uint16).tobytes(), 34963), "componentType": 5123, "count": len(idx), "type": "SCALAR"},
    ]
    image_view = view(png_bytes)
    j = {
        "asset": {"version": "2.0", "generator": "forclaude zdi_5m.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": name}],
        "meshes": [{"name": name, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
        "materials": [{"name": name, "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 1.0}}],
        "textures": [{"sampler": 0, "source": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "images": [{"bufferView": image_view, "mimeType": "image/png"}],
        "accessors": acc,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob)}],
    }
    js = json.dumps(j, separators=(",", ":")).encode()
    while len(js) % 4:
        js += b" "
    while len(blob) % 4:
        blob.append(0)
    out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob))
    out += struct.pack("<II", len(js), 0x4E4F534A) + js
    out += struct.pack("<II", len(blob), 0x004E4942) + bytes(blob)
    open(path, "wb").write(out)
    return len(out)


def face_quads():
    # x along the wall 0..5 m, the face to -y, z up; u from the left as one sees it, v from the top.
    s = TILE_M
    return [([(0, 0, 0), (s, 0, 0), (s, 0, s), (0, 0, s)], (0, -1, 0), [(0, 1), (1, 1), (1, 0), (0, 0)])]


for material, src_index in (("cihly", 3), ("kamen", 6)):
    t = tile(src_index, LIGHTNESS[material])
    buf = io.BytesIO()
    t.save(buf, format="PNG", optimize=True)
    png = buf.getvalue()
    n1 = glb(f"{OUT}/zed_lic_{material}.glb", face_quads(), png, f"zed_lic_{material}")
    print(material, "png", len(png), "face glb", n1)
