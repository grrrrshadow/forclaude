"""Our retaining wall face of concrete at one texture tile a level (5 m), as zdi_5m.py makes the bricks and the stone.

The texture is two rows of the panels of OrphanRTG's "Dirty leaking concrete wall" (Sketchfab, CC BY 4.0), its base
colour (a photo scan: the panels cut out of its atlas), one row over the other for 5 x 5 m, seamless both ways,
lightened for the game's light. On it a few places where the concrete has spalled off (the player, test2: "exposed
reinforcement, stones and dirt"): the gravel under it (the scan's own kerb), the rusty bars across, the rust run down
the wall below them -- drawn here, the same every run.
  python3 -I beton_5m.py <dirty_leaking_concrete_wall.glb> <out dir>
"""
import io, json, struct, sys

import numpy as np
from PIL import Image, ImageFilter

SRC, OUT = sys.argv[1:3]
LIGHTNESS = 104.0
SIZE = 512
WORK = 1024
OVERLAP = 128


def glb_image(path, index):
    data = open(path, "rb").read()
    n = struct.unpack_from("<I", data, 12)[0]
    j = json.loads(data[20:20 + n])
    bv = j["bufferViews"][j["images"][index]["bufferView"]]
    off = 20 + n + 8 + bv.get("byteOffset", 0)
    return Image.open(io.BytesIO(data[off:off + bv["byteLength"]])).convert("RGB")


def seamless(a, axis):
    """a is OVERLAP longer than wanted along axis: its last OVERLAP blended into its first, so its two ends meet."""
    n = a.shape[axis] - OVERLAP
    w = np.linspace(0.0, 1.0, OVERLAP)
    shape = [1, 1, 1]
    shape[axis] = OVERLAP
    w = w.reshape(shape)
    head = np.take(a, range(0, OVERLAP), axis=axis)
    tail = np.take(a, range(n, n + OVERLAP), axis=axis)
    out = np.take(a, range(0, n), axis=axis).copy()
    idx = [slice(None)] * 3
    idx[axis] = slice(0, OVERLAP)
    out[tuple(idx)] = tail * (1 - w) + head * w
    return out


def band(img, box, height):
    part = img.crop(box).resize((WORK + OVERLAP, height), Image.LANCZOS)
    return seamless(np.asarray(part, dtype=np.float64), 1)


def blob(rng, cx, cy, rx, ry):
    """An irregular patch: an ellipse with a ragged edge, as a mask 0..1 over the work picture."""
    yy, xx = np.mgrid[0:WORK, 0:WORK]
    ang = np.arctan2(yy - cy, xx - cx)
    edge = 1.0
    for k in range(2, 9):
        edge = edge + rng.uniform(-0.12, 0.12) * np.cos(k * ang + rng.uniform(0, 2 * np.pi))
    d = np.sqrt(((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2) / edge
    return np.clip((1.0 - d) * 6.0, 0.0, 1.0)


def spalls(img, wall):
    rng = np.random.default_rng(7)
    gravel = np.asarray(img.crop((40, 700, 980, 760)).resize((WORK, 128), Image.LANCZOS), dtype=np.float64)
    gravel = np.tile(gravel, (WORK // 128 + 1, 1, 1))[:WORK]
    # Grey, its weeds gone, its stones sharper.
    lum = gravel @ np.array([0.299, 0.587, 0.114])
    lum = lum.mean() + (lum - lum.mean()) * 1.7
    gravel = lum[..., None] * np.array([1.03, 1.0, 0.95])
    rust = np.array([122.0, 58.0, 24.0])
    yy, xx = np.mgrid[0:WORK, 0:WORK]
    for cx, cy, rx, ry in ((250, 290, 130, 78), (720, 660, 150, 88), (460, 885, 92, 58)):
        m = blob(rng, cx, cy, rx, ry)[..., None]
        # The gravel set back: darker towards the edge, a shadow under its upper rim.
        shade = 0.4 + 0.3 * m
        wall[:] = wall * (1 - m) + gravel * shade * m
        rim = np.clip(blob(rng, cx, cy - 4, rx * 1.02, ry * 1.03)[..., None] - m, 0, 1)
        wall[:] = wall * (1 - 0.3 * rim)
        # The bars, 20 cm apart: along, and across in the bigger ones; rusty, uneven, lit on top.
        bars = [("h", cy + d) for d in (-22, 22)]
        if rx > 100:
            bars += [("v", cx + d) for d in (-40, 40)]
        for kind, at in bars:
            along = xx if kind == "h" else yy
            across = yy if kind == "h" else xx
            wobble = 1.5 * np.sin(along / 17.0 + rng.uniform(0, 6))
            dist = np.abs(across - at - wobble)
            bar = np.clip(1.0 - dist / 4.5, 0, 1)[..., None] * m
            lit = np.clip(1.0 - np.abs(across - at - wobble + 2) / 1.6, 0, 1)[..., None] * m
            tone = (0.75 + 0.3 * np.sin(along / 9.0 + rng.uniform(0, 6)))[..., None]
            wall[:] = wall * (1 - bar) + rust * (tone + 0.35 * lit) * bar
        # The rust run down below it, fading, in streaks.
        x0, x1 = int(cx - rx * 0.7), int(cx + rx * 0.7)
        streak = np.zeros((WORK, WORK))
        cols = rng.uniform(0.3, 1.0, x1 - x0) * (rng.uniform(0, 1, x1 - x0) > 0.4)
        y0 = int(cy + ry * 0.5)
        for y in range(y0, min(WORK, y0 + 300)):
            f = 1.0 - (y - y0) / 300.0
            streak[y, x0:x1] = cols * f ** 1.5
        streak = np.asarray(Image.fromarray((streak * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(2.5)), dtype=np.float64)[..., None] / 255.0
        a = 0.8 * streak * (1 - m)
        wall[:] = wall * (1 - a) + (wall * 0.45 + rust * 0.55) * a


def texture():
    img = glb_image(SRC, 0)
    top = band(img, (0, 22, 690, 342), (WORK + OVERLAP) // 2)
    bottom = band(img, (127, 368, 973, 693), (WORK + OVERLAP) - (WORK + OVERLAP) // 2)
    wall = seamless(np.concatenate([top, bottom], axis=0), 0)
    spalls(img, wall)
    wall *= LIGHTNESS / (wall @ np.array([0.299, 0.587, 0.114])).mean()
    rgb = Image.fromarray(np.clip(wall, 0, 255).astype(np.uint8)).resize((SIZE, SIZE), Image.LANCZOS)
    return rgb.convert("RGBA")


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
    blob_bytes = bytearray()
    views = []

    def view(data, target=None):
        while len(blob_bytes) % 4:
            blob_bytes.append(0)
        off = len(blob_bytes)
        blob_bytes.extend(data)
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target:
            v["target"] = target
        views.append(v)
        return len(views) - 1

    p = np.array(pos, dtype=np.float32)
    acc = [
        {"bufferView": view(p.tobytes(), 34962), "componentType": 5126, "count": len(pos), "type": "VEC3", "min": p.min(axis=0).tolist(), "max": p.max(axis=0).tolist()},
        {"bufferView": view(np.array(nrm, dtype=np.float32).tobytes(), 34962), "componentType": 5126, "count": len(nrm), "type": "VEC3"},
        {"bufferView": view(np.array(uv, dtype=np.float32).tobytes(), 34962), "componentType": 5126, "count": len(uv), "type": "VEC2"},
        {"bufferView": view(np.array(idx, dtype=np.uint16).tobytes(), 34963), "componentType": 5123, "count": len(idx), "type": "SCALAR"},
    ]
    image_view = view(png_bytes)
    j = {
        "asset": {"version": "2.0", "generator": "forclaude beton_5m.py"},
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
        "buffers": [{"byteLength": len(blob_bytes)}],
    }
    js = json.dumps(j, separators=(",", ":")).encode()
    while len(js) % 4:
        js += b" "
    while len(blob_bytes) % 4:
        blob_bytes.append(0)
    out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob_bytes))
    out += struct.pack("<II", len(js), 0x4E4F534A) + js
    out += struct.pack("<II", len(blob_bytes), 0x004E4942) + bytes(blob_bytes)
    open(path, "wb").write(out)
    return len(out)


s = 5.0
quads = [([(0, 0, 0), (s, 0, 0), (s, 0, s), (0, 0, s)], (0, -1, 0), [(0, 1), (1, 1), (1, 0), (0, 0)])]
t = texture()
buf = io.BytesIO()
t.save(buf, format="PNG", optimize=True)
png = buf.getvalue()
n = glb(f"{OUT}/zed_lic_beton.glb", quads, png, "zed_lic_beton")
print("beton png", len(png), "face glb", n)
