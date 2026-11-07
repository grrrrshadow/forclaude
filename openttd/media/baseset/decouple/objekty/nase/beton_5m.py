"""Our retaining wall face of concrete at one texture tile a level (5 m), as zdi_5m.py makes the bricks and the stone.

The texture is two rows of the panels of OrphanRTG's "Dirty leaking concrete wall" (Sketchfab, CC BY 4.0), its base
colour (a photo scan: the panels cut out of its atlas), one row over the other for 5 x 5 m, seamless both ways,
lightened for the game's light. The places where the concrete has spalled off (the player, test2: "exposed
reinforcement, stones and dirt") are not in it -- on a bridge the same picture came again and again (the player, 7.
11.: "out of the texture, we lay them at random as the graffiti"): each a board of its own, beton_odprysk_N.glb, the
stones under it, the broken edge, the rusty bars across, the rust run down below -- our own drawing, the same every
run, laid by the game on the concrete at random.
  python3 -I beton_5m.py <dirty_leaking_concrete_wall.glb> <out dir> [<dir of previews>]
"""
import io, json, struct, sys

import numpy as np
from PIL import Image, ImageDraw

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


DECAL_PX_PER_M = 200
RUST = np.array([104.0, 50.0, 24.0])


def aggregate(rng, W, H):
    """The concrete's stones where its face has gone: our own, drawn stone by stone -- grey and brownish pebbles, each
    lit on its upper left and shaded on its lower right, packed in the dark cement between them."""
    img = Image.new("RGB", (W * 2, H * 2), (58, 57, 54))
    d = ImageDraw.Draw(img)
    for _ in range(int(W * H / 9)):
        x, y = rng.uniform(-6, W * 2 + 6), rng.uniform(-6, H * 2 + 6)
        r = rng.uniform(2.5, 9.0)
        e = rng.uniform(0.6, 1.0)
        grey = rng.uniform(78, 170)
        warm = rng.uniform(-6, 14)
        base = (grey + warm, grey + warm * 0.4, grey - warm * 0.3)
        box = (x - r, y - r * e, x + r, y + r * e)
        d.ellipse((box[0] + 1.5, box[1] + 1.5, box[2] + 1.5, box[3] + 1.5), fill=tuple(int(c * 0.45) for c in base))
        d.ellipse(box, fill=tuple(int(c) for c in base))
        d.ellipse((x - r * 0.55 - r * 0.25, y - r * e * 0.55 - r * e * 0.25, x + r * 0.3 - r * 0.25, y + r * e * 0.3 - r * e * 0.25),
                  fill=tuple(int(min(255, c * 1.18)) for c in base))
    return np.asarray(img.resize((W, H), Image.LANCZOS), dtype=np.float64)


def spall(seed, rx_m, ry_m, run_m):
    """A place where the concrete has spalled off, as a board of its own (no background): rx_m, ry_m its half width and
    height, run_m how far the rust has run down below it. Its stones set back, darker in, a shadow under its upper
    edge; round it the broken edge, rough, of varying width; the bars 20 cm apart across it, rusty, going into the
    concrete before its edge; under it the rust run down in a few thin streaks, each tapering. Opaque all (the game
    draws a texel of alpha below 128 as none)."""
    rng = np.random.default_rng(seed)
    pad = 0.05
    w_m, h_m = 2 * (rx_m + pad), 2 * (ry_m + pad) + run_m
    W, H = int(round(w_m * DECAL_PX_PER_M)), int(round(h_m * DECAL_PX_PER_M))
    cx, cy = W / 2.0, (pad + ry_m) * DECAL_PX_PER_M
    rx, ry = rx_m * DECAL_PX_PER_M, ry_m * DECAL_PX_PER_M
    yy, xx = np.mgrid[0:H, 0:W]
    ang = np.arctan2(yy - cy, xx - cx)
    # A ragged outline: many waves, the short ones small.
    edge = 1.0
    for k in range(2, 33):
        edge = edge + rng.uniform(-0.16, 0.16) / k ** 0.75 * np.cos(k * ang + rng.uniform(0, 2 * np.pi))
    d = np.sqrt(((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2) / np.maximum(edge, 0.4)
    inside = d < 1.0
    rim_w = (3.0 + 4.0 * (0.5 + 0.5 * np.cos(ang * rng.integers(3, 7) + rng.uniform(0, 6)))) / min(rx, ry)
    rim = (d >= 1.0) & (d < 1.0 + rim_w)
    out = aggregate(rng, W, H)
    shade = 0.5 + 0.4 * np.clip(1.0 - d, 0, 1)
    # A shadow under its upper edge: set back from the face.
    shade = shade * np.where((yy < cy) & (d > 0.7), 0.7, 1.0)
    out *= shade[..., None]
    # The broken edge: fresh concrete, rough, a dark crack along its inner side.
    fresh = 138 + rng.normal(0, 12, (H, W))
    crack = (d >= 0.96) & (d < 1.0)
    for k in range(3):
        out[..., k] = np.where(rim, fresh * (1.0, 0.98, 0.94)[k], out[..., k])
        out[..., k] = np.where(crack, 40.0, out[..., k])
    solid = inside | rim
    # The bars: along it, and across in the bigger ones; each into the concrete short of the edge.
    bars = [("h", cy + s * 0.1 * DECAL_PX_PER_M) for s in (-1, 1)]
    if rx_m > 0.45:
        bars += [("v", cx + s * 0.2 * DECAL_PX_PER_M) for s in (-1, 1)]
    for kind, at in bars:
        along = xx if kind == "h" else yy
        across = yy if kind == "h" else xx
        wobble = 1.2 * np.sin(along / 21.0 + rng.uniform(0, 6))
        dist = np.abs(across - at - wobble)
        bar = (dist < 2.6) & (d < rng.uniform(0.82, 0.95))
        lit = np.abs(across - at - wobble + 1.3) < 0.9
        tone = 0.7 + 0.25 * np.sin(along / 7.0 + rng.uniform(0, 6)) + rng.normal(0, 0.08, (H, W))
        for k in range(3):
            out[..., k] = np.where(bar, RUST[k] * (tone + 0.3 * lit), out[..., k])
    # The rust run down below it: a few thin streaks from its lower edge, tapering, wavering a little.
    if run_m > 0:
        run_px = run_m * DECAL_PX_PER_M
        lowest = np.where(inside.any(axis=0), H - 1 - np.argmax(inside[::-1], axis=0), -1)
        x = int(cx - rx * 0.7)
        while x < cx + rx * 0.7:
            if lowest[x] >= 0 and rng.uniform() < 0.4:
                length = run_px * rng.uniform(0.1, 1.0) ** 2
                width = rng.uniform(1.0, 2.6)
                tone = rng.uniform(0.8, 1.1)
                colour = (RUST * 0.45 + 92.0 * 0.55) * tone
                y0, phase = lowest[x], rng.uniform(0, 6)
                for y in range(y0, min(H, int(y0 + length))):
                    f = (y - y0) / max(length, 1.0)
                    half = width * (1.0 - 0.8 * f) / 2.0
                    xc = x + 0.8 * np.sin(y / 13.0 + phase)
                    for xi in range(max(0, int(round(xc - half))), min(W, int(round(xc + half)) + 1)):
                        if not solid[y, xi]:
                            out[y, xi] = colour * (1.0 - 0.15 * f)
                            solid[y, xi] = True
            x += int(rng.integers(4, 12))
    rgba = np.zeros((H, W, 4), dtype=np.uint8)
    rgba[..., :3] = np.clip(out, 0, 255).astype(np.uint8)
    rgba[..., 3] = np.where(solid, 255, 0)
    return Image.fromarray(rgba), w_m, h_m


def texture():
    img = glb_image(SRC, 0)
    top = band(img, (0, 22, 690, 342), (WORK + OVERLAP) // 2)
    bottom = band(img, (127, 368, 973, 693), (WORK + OVERLAP) - (WORK + OVERLAP) // 2)
    wall = seamless(np.concatenate([top, bottom], axis=0), 0)
    wall *= LIGHTNESS / (wall @ np.array([0.299, 0.587, 0.114])).mean()
    rgb = Image.fromarray(np.clip(wall, 0, 255).astype(np.uint8)).resize((SIZE, SIZE), Image.LANCZOS)
    return rgb.convert("RGBA")


def glb(path, quads, png_bytes, name, decal=False):
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
        "materials": [dict({"name": name, "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 1.0}},
                           **({"alphaMode": "MASK", "alphaCutoff": 0.5} if decal else {}))],
        "textures": [{"sampler": 0, "source": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071 if decal else 10497, "wrapT": 33071 if decal else 10497}],
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


def png_of(image):
    buf = io.BytesIO()
    image.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


s = 5.0
quads = [([(0, 0, 0), (s, 0, 0), (s, 0, s), (0, 0, s)], (0, -1, 0), [(0, 1), (1, 1), (1, 0), (0, 0)])]
png = png_of(texture())
n = glb(f"{OUT}/zed_lic_beton.glb", quads, png, "zed_lic_beton")
print("beton png", len(png), "face glb", n)
# The spalled places, each a board as glTF has it (+y up, its face to +z), from the origin at its left foot.
SPALLS = [(71, 0.45, 0.26, 0.6), (72, 0.7, 0.38, 0.9), (73, 0.3, 0.2, 0.35), (74, 0.6, 0.3, 0.0)]
for k, (seed, rx_m, ry_m, run_m) in enumerate(SPALLS, 1):
    image, w_m, h_m = spall(seed, rx_m, ry_m, run_m)
    board = [([(0, 0, 0), (w_m, 0, 0), (w_m, h_m, 0), (0, h_m, 0)], (0, 0, 1), [(0, 1), (1, 1), (1, 0), (0, 0)])]
    name = f"beton_odprysk_{k}"
    n = glb(f"{OUT}/{name}.glb", board, png_of(image), name, decal=True)
    if len(sys.argv) > 3:
        image.save(f"{sys.argv[3]}/{name}.png")
    print(name, round(w_m, 2), round(h_m, 2), image.size, n)
