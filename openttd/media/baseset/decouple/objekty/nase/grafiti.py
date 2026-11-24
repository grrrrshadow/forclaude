"""Our graffiti for the concrete retaining walls: drawn here, letter by letter, as a sprayer would.

The player (test2, agreed 6. 11.): drawn by us, not cut out of photos; a transparent background; various sizes and
colours; the writings "pajda" and "miluju karla máchu"; only on concrete, under the ivies. Each graffiti a board of
its own: a rectangle of its real size, its picture with no background (the game draws the texels of alpha < 128 as
none). The letters are strokes of a round nozzle along lines and arcs (our own font below); the bubble letters a
dark outline round a fill, a light shine and a drop shadow, with paint running down; the tags one thin stroke,
slanted, underlined. A little overspray about the strokes. The same every run.
  python3 -I grafiti.py <out dir>
"""
import io, json, math, struct, sys

import numpy as np
from PIL import Image, ImageDraw

OUT = sys.argv[1]
PX_PER_M = 160
SUPER = 3

# Our font: each letter strokes of points (x, y), y up, baseline 0, x-height 1; its advance. Arcs as point lists.


def arc(cx, cy, rx, ry, a0, a1, n=18):
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / n)), cy + ry * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


BOWL = arc(0.42, 0.5, 0.42, 0.5, 0, 360, 28)
GLYPHS = {
    "a": ([BOWL, [(0.84, 1.0), (0.84, 0.0)]], 1.05),
    "á": ([BOWL, [(0.84, 1.0), (0.84, 0.0)], [(0.38, 1.25), (0.7, 1.55)]], 1.05),
    "c": ([arc(0.45, 0.5, 0.45, 0.5, 45, 315, 24)], 0.95),
    "d": ([BOWL, [(0.84, 1.65), (0.84, 0.0)]], 1.05),
    "h": ([[(0.0, 1.65), (0.0, 0.0)], [(0.0, 0.55)] + arc(0.4, 0.6, 0.4, 0.4, 180, 0, 12)[1:] + [(0.8, 0.0)]], 1.0),
    "i": ([[(0.0, 1.0), (0.0, 0.0)], [(0.0, 1.38), (0.0, 1.42)]], 0.3),
    "j": ([[(0.25, 1.0), (0.25, -0.3)] + arc(0.0, -0.3, 0.25, 0.3, 0, -150, 8)[1:], [(0.25, 1.38), (0.25, 1.42)]], 0.55),
    "k": ([[(0.0, 1.65), (0.0, 0.0)], [(0.75, 1.0), (0.0, 0.42)], [(0.22, 0.6), (0.8, 0.0)]], 0.95),
    "l": ([[(0.0, 1.65), (0.0, 0.15), (0.15, 0.0)]], 0.35),
    "m": ([[(0.0, 1.0), (0.0, 0.0)], [(0.0, 0.6)] + arc(0.3, 0.65, 0.3, 0.35, 180, 0, 10)[1:] + [(0.6, 0.0)],
           [(0.6, 0.6)] + arc(0.9, 0.65, 0.3, 0.35, 180, 0, 10)[1:] + [(1.2, 0.0)]], 1.45),
    "p": ([[(0.0, 1.0), (0.0, -0.65)], arc(0.42, 0.5, 0.42, 0.5, 0, 360, 28)], 1.05),
    "r": ([[(0.0, 1.0), (0.0, 0.0)], [(0.0, 0.55)] + arc(0.38, 0.6, 0.38, 0.4, 180, 60, 8)[1:]], 0.75),
    "u": ([[(0.0, 1.0), (0.0, 0.42)] + arc(0.4, 0.42, 0.4, 0.42, 180, 360, 12)[1:], [(0.8, 1.0), (0.8, 0.0)]], 1.0),
    " ": ([], 0.55),
}


def layout(text, gap=0.22, slant=0.0):
    """The strokes of a line of text, letter after letter, each point slanted by its height (a tag leans)."""
    strokes, x = [], 0.0
    for ch in text:
        lines, adv = GLYPHS[ch]
        for s in lines:
            strokes.append([(x + px + slant * py, py) for px, py in s])
        x += adv + gap
    return strokes, x - gap


def heart(cx, cy, r):
    pts = []
    for i in range(41):
        t = math.pi * 2 * i / 40
        pts.append((cx + r * 16 * math.sin(t) ** 3 / 17, cy + r * (13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)) / 17))
    return pts


class Canvas:
    """A picture w x h metres at PX_PER_M, drawn SUPER times finer and taken down; metres from its bottom left."""

    def __init__(self, w_m, h_m):
        self.w_m, self.h_m = w_m, h_m
        self.W, self.H = int(round(w_m * PX_PER_M)), int(round(h_m * PX_PER_M))
        self.img = Image.new("RGBA", (self.W * SUPER, self.H * SUPER), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.img)

    def p(self, x, y):
        return (x * PX_PER_M * SUPER, (self.h_m - y) * PX_PER_M * SUPER)

    def stroke(self, pts, width_m, colour, dx=0.0, dy=0.0):
        w = max(1, int(round(width_m * PX_PER_M * SUPER)))
        q = [self.p(x + dx, y + dy) for x, y in pts]
        if len(q) > 1:
            self.d.line(q, fill=colour, width=w, joint="curve")
        r = w / 2
        for x, y in q:
            self.d.ellipse((x - r, y - r, x + r, y + r), fill=colour)

    def polygon(self, pts, colour, dx=0.0, dy=0.0):
        self.d.polygon([self.p(x + dx, y + dy) for x, y in pts], fill=colour)

    def done(self, rng, overspray):
        small = self.img.resize((self.W, self.H), Image.LANCZOS)
        a = np.asarray(small).copy()
        if overspray > 0:
            # A few specks of the paint about its strokes, where the can was held too far.
            solid = a[..., 3] >= 128
            near = np.zeros_like(solid)
            for oy in range(-6, 7, 2):
                for ox in range(-6, 7, 2):
                    near |= np.roll(np.roll(solid, oy, 0), ox, 1)
            specks = near & ~solid & (rng.uniform(0, 1, solid.shape) < overspray)
            ys, xs = np.nonzero(specks)
            for y, x in zip(ys, xs):
                sy, sx = min(max(y + rng.integers(-6, 7), 0), a.shape[0] - 1), min(max(x + rng.integers(-6, 7), 0), a.shape[1] - 1)
                if a[sy, sx, 3] >= 128:
                    a[y, x] = a[sy, sx]
        a[..., 3] = np.where(a[..., 3] >= 128, 255, 0)
        return Image.fromarray(a)


def bubble(text, colours, w_m=None, h_letters=0.62, seed=1, second=None, with_heart=False):
    """Bubble letters: a drop shadow, a dark outline, the fill, a shine on the left top of the strokes; runs below."""
    fill, line, shine, shadow = colours
    rng = np.random.default_rng(seed)
    rows = [text] + ([second] if second else [])
    laid = [layout(t, gap=0.08, slant=0.12) for t in rows]
    s = h_letters
    widths = [lw * s for _, lw in laid]
    pad = 0.18
    line_h = 2.5 * s
    width = max(widths) + 2 * pad + (0.9 * s if with_heart else 0)
    height = line_h * len(rows) + 2 * pad
    c = Canvas(w_m or width, height)
    k = (w_m or width) / width
    thick = 0.36 * s * k

    def placed(strokes, row, lw):
        x0 = pad + (max(widths) - lw) / 2
        y0 = height - pad - line_h * (row + 1) + 0.75 * s
        return [[((x0 + px * s) * k, (y0 + py * s) * k) for px, py in st] for st in strokes]

    all_strokes = []
    for row, ((strokes, lw), wdt) in enumerate(zip(laid, widths)):
        all_strokes += placed(strokes, row, wdt)
    hearts = []
    if with_heart:
        hearts.append(heart((pad + max(widths) + 0.45 * s) * k, (height - pad - line_h * len(rows) + 1.0 * s) * k, 0.55 * s * k))
    # The shadow, the outline, the fill.
    for st in all_strokes:
        c.stroke(st, thick + 0.1 * s * k, shadow, 0.07 * s * k, -0.07 * s * k)
    for h in hearts:
        c.polygon(h, shadow, 0.07 * s * k, -0.07 * s * k)
    for st in all_strokes:
        c.stroke(st, thick + 0.1 * s * k, line)
    for h in hearts:
        c.polygon([(x, y) for x, y in h], line)
    # The paint run down from the lowest points of some strokes.
    for st in all_strokes:
        lo = min(st, key=lambda p: p[1])
        if rng.uniform() < 0.45:
            run = rng.uniform(0.06, 0.22) * k
            c.stroke([lo, (lo[0], max(0.02, lo[1] - run))], thick * 0.22, fill)
    for st in all_strokes:
        c.stroke(st, thick, fill)
    for h in hearts:
        cx = sum(x for x, _ in h) / len(h)
        cy = sum(y for _, y in h) / len(h)
        c.polygon([(cx + (x - cx) * 0.82, cy + (y - cy) * 0.82) for x, y in h], (225, 30, 50, 255))
    for st in all_strokes:
        c.stroke(st, thick * 0.28, shine, -0.06 * s * k, 0.06 * s * k)
    return c.done(rng, 0.004), c


def tag(text, colour, h_letters=0.22, seed=2, underline=True, second=None):
    """A tag: one thin stroke, slanted, its letters close; a swoosh under it."""
    rng = np.random.default_rng(seed)
    rows = [text] + ([second] if second else [])
    laid = [layout(t, gap=0.1, slant=0.35) for t in rows]
    s = h_letters
    pad = 0.12
    line_h = 2.6 * s
    width = max(lw for _, lw in laid) * s + 2 * pad + 0.4 * s
    height = line_h * len(rows) + 2 * pad + (0.3 * s if underline else 0)
    c = Canvas(width, height)
    thick = max(0.035, 0.2 * s)
    lowest = height
    for row, (strokes, lw) in enumerate(laid):
        x0 = pad
        y0 = height - pad - line_h * (row + 1) + 0.85 * s
        lowest = min(lowest, y0)
        for st in strokes:
            c.stroke([(x0 + px * s, y0 + py * s) for px, py in st], thick, colour)
    if underline:
        # Under the descenders of the last line: its ends a little higher than its middle.
        sw = arc(width / 2, lowest - 0.6 * s, width / 2 - pad, 0.25 * s, 200, 340, 20)
        c.stroke(sw, thick * 0.8, colour)
    return c.done(rng, 0.006), c


def glb(path, w_m, h_m, png_bytes, name):
    """One board w x h metres, as glTF has it (+y up): from the origin along +x, up +y, its face to +z, the picture
    over it (UV 0 to 1). The game reads a figure so: its height up, its width along."""
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
        "asset": {"version": "2.0", "generator": "forclaude grafiti.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": name}],
        "meshes": [{"name": name, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
        "materials": [{"name": name, "alphaMode": "MASK", "alphaCutoff": 0.5, "doubleSided": False,
                       "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 1.0}}],
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


PINK = ((236, 64, 150, 255), (35, 18, 55, 255), (255, 200, 230, 255), (15, 10, 25, 255))
LIME = ((140, 220, 40, 255), (18, 55, 25, 255), (225, 255, 175, 255), (10, 25, 10, 255))
CHROME = ((205, 207, 212, 255), (18, 18, 22, 255), (255, 255, 255, 255), (60, 30, 110, 255))
ORANGE = ((250, 140, 25, 255), (70, 22, 12, 255), (255, 225, 160, 255), (25, 10, 5, 255))
BLUE = ((45, 140, 235, 255), (10, 22, 65, 255), (190, 228, 255, 255), (5, 10, 30, 255))
RED = ((215, 30, 35, 255), (250, 250, 250, 255), (255, 170, 160, 255), (30, 5, 5, 255))

PIECES = [
    ("grafiti_pajda_ruzova", lambda: bubble("pajda", PINK, seed=11, h_letters=0.45)),
    ("grafiti_pajda_zelena", lambda: bubble("pajda", LIME, seed=12, h_letters=0.38)),
    ("grafiti_pajda_chrom", lambda: bubble("pajda", CHROME, seed=13, h_letters=0.55)),
    ("grafiti_pajda_cervena", lambda: bubble("pajda", RED, seed=14, h_letters=0.32)),
    ("grafiti_pajda_podpis_cerny", lambda: tag("pajda", (24, 24, 26, 255), seed=21, h_letters=0.18)),
    ("grafiti_pajda_podpis_cerveny", lambda: tag("pajda", (200, 25, 30, 255), seed=22, h_letters=0.24)),
    ("grafiti_miluju_podpis_cerny", lambda: tag("miluju karla máchu", (24, 24, 26, 255), seed=23, h_letters=0.14)),
    ("grafiti_miluju_podpis_bily", lambda: tag("miluju", (240, 240, 236, 255), seed=24, h_letters=0.17, second="karla máchu")),
    ("grafiti_miluju_oranzova", lambda: bubble("miluju", ORANGE, seed=31, h_letters=0.28, second="karla máchu", with_heart=True)),
    ("grafiti_miluju_modra", lambda: bubble("miluju", BLUE, seed=32, h_letters=0.24, second="karla máchu", with_heart=True)),
]

listing = []
for name, make in PIECES:
    img, c = make()
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    n = glb(f"{OUT}/{name}.glb", c.w_m, c.h_m, buf.getvalue(), name)
    listing.append((name, round(c.w_m, 2), round(c.h_m, 2), img.size, n))
    if len(sys.argv) > 2:
        img.save(f"{sys.argv[2]}/{name}.png")
for row in listing:
    print(*row)
