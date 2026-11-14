"""Our own pieces of rust for the sides and the ends of old vehicles (the player, 7. 11.: "rust and graffiti, we will
do them"; the plan: our own drawing, transparent round, eight kinds 0.2 to 1 m: spots, runs under an edge, blisters,
round the rivets). Each a board as the graffiti of the walls are (grafiti.py, beton_5m.py): a rectangle of its true
size as glTF has it, from the origin at its left foot, +x along, +y up, its face to +z, UV 0 to 1, its picture with
no background (a texel of alpha below 128 is drawn as none). The game lays them on the flat upright faces of an old
vehicle's sides and ends, more as it ages (the plan: the first at 8 years, more every 2 years, many at 20).
Drawn the same every run (each kind its seed).
  python3 -I rez_kusy.py <out dir> [<dir of previews>]
"""
import io, json, struct, sys

import numpy as np
from PIL import Image

OUT = sys.argv[1]
PX_PER_M = 200

# Rust from the darkest (old, pitted) to the lightest (fresh, powdery), RGB.
DARK = np.array([52.0, 30.0, 20.0])
BROWN = np.array([98.0, 50.0, 28.0])
ORANGE = np.array([146.0, 72.0, 34.0])
LIGHT = np.array([172.0, 102.0, 54.0])


def noise(rng, H, W, cell):
    """Smooth value noise, 0 to 1: a grid of random values a cell apart, made as big as the picture."""
    gh, gw = max(2, int(H / cell) + 2), max(2, int(W / cell) + 2)
    grid = Image.fromarray((rng.random((gh, gw)) * 255).astype(np.uint8))
    big = grid.resize((int(gw * cell), int(gh * cell)), Image.BICUBIC)
    return np.asarray(big, dtype=np.float64)[:H, :W] / 255.0


def fractal(rng, H, W, cells=(48, 20, 8, 3)):
    """Noise of several sizes, the big ones most, 0 to 1."""
    out, total = np.zeros((H, W)), 0.0
    for k, cell in enumerate(cells):
        w = 0.6 ** k
        out += w * noise(rng, H, W, cell)
        total += w
    return out / total


def rust_of(t):
    """The colour of rust at t, 0 the darkest to 1 the lightest."""
    t = np.clip(t, 0.0, 1.0)[..., None]
    a = np.where(t < 0.35, DARK + (BROWN - DARK) * (t / 0.35), 0)
    b = np.where((t >= 0.35) & (t < 0.7), BROWN + (ORANGE - BROWN) * ((t - 0.35) / 0.35), 0)
    c = np.where(t >= 0.7, ORANGE + (LIGHT - ORANGE) * ((t - 0.7) / 0.3), 0)
    return a + b + c


def pits(rng, H, W, solid, share):
    """Small dark pits where the rust has eaten in, about share of it: a mask of the lowest of fine noise."""
    n = fractal(rng, H, W, (5, 2.5, 1.5))
    if not solid.any():
        return solid
    return solid & (n < np.quantile(n[solid], share))


def grain(rng, H, W, amount=0.12):
    """The rust's grain: fine noise and a little of each texel's own."""
    return amount * (fractal(rng, H, W, (3, 1.5)) - 0.5) * 2 + rng.normal(0, amount / 3, (H, W))


def blotch(rng, H, W, cx, cy, rx, ry, rough=0.35):
    """A blotch with a ragged edge, its distance from the middle where 1 is its edge (below 1 inside)."""
    yy, xx = np.mgrid[0:H, 0:W]
    d = np.sqrt(((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2)
    return d / (1.0 - rough + 2 * rough * fractal(rng, H, W, (max(6, rx / 2), max(3, rx / 6), 2)))


def finish(colour, solid):
    rgba = np.zeros(solid.shape + (4,), dtype=np.uint8)
    rgba[..., :3] = np.clip(colour, 0, 255).astype(np.uint8)
    rgba[..., 3] = np.where(solid, 255, 0)
    return Image.fromarray(rgba)


def spot(seed, w_m, h_m, specks):
    """A spot of rust where the paint has gone: dark and pitted in the middle, orange to its ragged edge, a dark line
    round it where the paint is lifted; a few small specks round it."""
    rng = np.random.default_rng(seed)
    W, H = int(w_m * PX_PER_M), int(h_m * PX_PER_M)
    cx, cy = W * rng.uniform(0.45, 0.55), H * rng.uniform(0.45, 0.55)
    d = blotch(rng, H, W, cx, cy, W * 0.36, H * 0.36)
    solid = d < 1.0
    for _ in range(specks):
        sx, sy = rng.uniform(0.1, 0.9) * W, rng.uniform(0.1, 0.9) * H
        r = rng.uniform(2.0, 7.0)
        solid |= blotch(rng, H, W, sx, sy, r, r * rng.uniform(0.6, 1.2), 0.3) < 1.0
    t = 0.15 + 0.6 * np.clip(d, 0, 1) + 0.6 * (fractal(rng, H, W) - 0.5) + grain(rng, H, W)
    colour = rust_of(t)
    colour[(d > 0.92) & (d < 1.0)] = DARK * 0.85
    colour[pits(rng, H, W, solid & (d < 0.7), 0.12)] = DARK * 0.6
    return finish(colour, solid)


def runs(seed, w_m, h_m, streaks):
    """Rust run down under an edge: a rusty seam along its top, from it thin streaks hanging down, each tapering and
    breaking up into drops at its end."""
    rng = np.random.default_rng(seed)
    W, H = int(w_m * PX_PER_M), int(h_m * PX_PER_M)
    yy, xx = np.mgrid[0:H, 0:W]
    seam_h = rng.uniform(8, 14)
    seam = yy < seam_h * (0.55 + 0.9 * noise(rng, H, W, 30))
    solid = seam.copy()
    colour = rust_of(0.2 + 0.6 * fractal(rng, H, W) + grain(rng, H, W))
    along = fractal(rng, H, W, (24, 8))
    for _ in range(streaks):
        x0 = rng.uniform(0.04, 0.96) * W
        length = H * rng.uniform(0.2, 1.0) ** 1.6
        width = rng.uniform(1.5, 5.5)
        tone = rng.uniform(0.3, 0.7)
        phase = rng.uniform(0, 6)
        for y in range(int(seam_h * 0.5), min(H, int(length))):
            f = y / max(length, 1.0)
            half = width * (1.0 - 0.85 * f) / 2.0
            xc = x0 + 1.5 * np.sin(y / 17.0 + phase)
            # Near its end it breaks up: fewer and fewer rows drawn.
            if f > 0.7 and rng.uniform() < (f - 0.7) / 0.3:
                continue
            lo, hi = max(0, int(round(xc - half))), min(W, int(round(xc + half)) + 1)
            solid[y, lo:hi] = True
            colour[y, lo:hi] = rust_of(tone * (1.0 - 0.25 * f) + 0.35 * (along[y, lo:hi] - 0.5))
    colour[pits(rng, H, W, seam, 0.15)] = DARK * 0.6
    return finish(colour, solid)


def blisters(seed, w_m, h_m, count):
    """Blisters: round bubbles where the rust lifts the paint, each its cap lit on its upper left and its shadow on its
    lower right, some burst, their rust dark in them; a rusty stain under them all."""
    rng = np.random.default_rng(seed)
    W, H = int(w_m * PX_PER_M), int(h_m * PX_PER_M)
    yy, xx = np.mgrid[0:H, 0:W]
    stain = blotch(rng, H, W, W / 2, H / 2, W * 0.42, H * 0.38, 0.4) < 1.0
    solid = stain.copy()
    colour = rust_of(0.35 + 0.6 * (fractal(rng, H, W) - 0.5) + grain(rng, H, W))
    for _ in range(count):
        r = rng.uniform(2.0, 6.5)
        bx, by = rng.uniform(0.15, 0.85) * W, rng.uniform(0.15, 0.85) * H
        dd = np.sqrt((xx - bx) ** 2 + (yy - by) ** 2) / r
        cap = dd < 1.0
        solid |= cap
        # Its shadow on its lower right only, its cap lit on its upper left.
        shadow = (dd >= 0.75) & cap & ((xx - bx) + (yy - by) > 0)
        lit = 1.0 - 0.35 * ((xx - bx) + (yy - by)) / (2 * r)
        cap_colour = rust_of(np.full((H, W), 0.55)) * np.clip(lit, 0.75, 1.2)[..., None]
        colour[cap] = cap_colour[cap]
        colour[shadow] = DARK * 0.9
        if rng.uniform() < 0.25:
            colour[dd < 0.45] = DARK * 0.6
    colour[pits(rng, H, W, stain, 0.06)] = DARK * 0.6
    return finish(colour, solid)


def rivets(seed, w_m, h_m, columns, rows, pitch_m):
    """Rivets in rows, each its head lit on its upper left, rust round it and a short run down from it."""
    rng = np.random.default_rng(seed)
    W, H = int(w_m * PX_PER_M), int(h_m * PX_PER_M)
    yy, xx = np.mgrid[0:H, 0:W]
    solid = np.zeros((H, W), dtype=bool)
    colour = rust_of(0.4 + 0.6 * (fractal(rng, H, W) - 0.5) + grain(rng, H, W))
    pitch = pitch_m * PX_PER_M
    x0 = (W - (columns - 1) * pitch) / 2
    y0 = min(H * 0.3, (H - (rows - 1) * pitch) / 2)
    for i in range(columns):
        for j in range(rows):
            rx, ry = x0 + i * pitch + rng.normal(0, 1.0), y0 + j * pitch + rng.normal(0, 1.0)
            halo = blotch(rng, H, W, rx, ry, rng.uniform(13, 22), rng.uniform(11, 18), 0.4) < 1.0
            solid |= halo
            # Its run down, short.
            length = rng.uniform(0.06, 0.2) * PX_PER_M
            width = rng.uniform(3.0, 7.0)
            run = (np.abs(xx - rx - 1.2 * np.sin(yy / 9.0)) < width * (1 - (yy - ry) / length).clip(0, 1) / 2) & (yy > ry) & (yy < ry + length)
            solid |= run
            colour[run] = rust_of(np.full(int(run.sum()), 0.5))
            dr = np.sqrt((xx - rx) ** 2 + (yy - ry) ** 2)
            head = dr < 4.5
            lit = 1.0 - ((xx - rx) + (yy - ry)) / 10.0
            rusty = rust_of(np.full((H, W), 0.3)) * np.clip(lit, 0.6, 1.4)[..., None]
            colour[head] = rusty[head]
            ring = (dr >= 4.5) & (dr < 5.5) & ((xx - rx) + (yy - ry) > -2)
            colour[ring] = DARK * 0.7
            solid |= head | ring
    colour[pits(rng, H, W, solid, 0.08)] = DARK * 0.6
    return finish(colour, solid)


def glb(path, w_m, h_m, png_bytes, name):
    """A board w_m by h_m, as glTF has it: from the origin at its left foot, +x along, +y up, its face to +z."""
    pos = [(0, 0, 0), (w_m, 0, 0), (w_m, h_m, 0), (0, h_m, 0)]
    nrm = [(0, 0, 1)] * 4
    uv = [(0, 1), (1, 1), (1, 0), (0, 0)]
    idx = [0, 1, 2, 0, 2, 3]
    blob = bytearray()
    views = []

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

    p = np.array(pos, dtype=np.float32)
    acc = [
        {"bufferView": view(p.tobytes(), 34962), "componentType": 5126, "count": 4, "type": "VEC3", "min": p.min(axis=0).tolist(), "max": p.max(axis=0).tolist()},
        {"bufferView": view(np.array(nrm, dtype=np.float32).tobytes(), 34962), "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": view(np.array(uv, dtype=np.float32).tobytes(), 34962), "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": view(np.array(idx, dtype=np.uint16).tobytes(), 34963), "componentType": 5123, "count": 6, "type": "SCALAR"},
    ]
    image_view = view(png_bytes)
    j = {
        "asset": {"version": "2.0", "generator": "forclaude rez_kusy.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": name}],
        "meshes": [{"name": name, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
        "materials": [{"name": name, "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 1.0},
                       "alphaMode": "MASK", "alphaCutoff": 0.5}],
        "textures": [{"sampler": 0, "source": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}],
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


def png_of(image):
    buf = io.BytesIO()
    image.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


PIECES = [
    ("rez_skvrna_1", 0.26, 0.22, lambda: spot(81, 0.26, 0.22, 3)),
    ("rez_skvrna_2", 0.48, 0.36, lambda: spot(82, 0.48, 0.36, 9)),
    ("rez_stekance_1", 0.62, 0.72, lambda: runs(83, 0.62, 0.72, 14)),
    ("rez_stekance_2", 0.34, 0.5, lambda: runs(84, 0.34, 0.5, 7)),
    ("rez_puchyre_1", 0.32, 0.24, lambda: blisters(85, 0.32, 0.24, 9)),
    ("rez_puchyre_2", 0.56, 0.3, lambda: blisters(86, 0.56, 0.3, 16)),
    ("rez_nyty_1", 0.74, 0.3, lambda: rivets(87, 0.74, 0.3, 5, 1, 0.14)),
    ("rez_nyty_2", 0.26, 0.52, lambda: rivets(88, 0.26, 0.52, 1, 3, 0.13)),
]
for name, w_m, h_m, draw in PIECES:
    image = draw()
    n = glb(f"{OUT}/{name}.glb", w_m, h_m, png_of(image), name)
    if len(sys.argv) > 2:
        image.save(f"{sys.argv[2]}/{name}.png")
    print(name, w_m, h_m, image.size, n)
