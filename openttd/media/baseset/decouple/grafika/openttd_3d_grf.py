#!/usr/bin/env python3
"""The game's own buildings from eight directions, with depth and normals, as
sets of the game's own graphics (baseset/decouple/grafika/*.grf) and the table
the game reads them by (src/table/three_d_models.h). The 3D of this game
(docs/decouple_three_d.md): in the turned view a building is drawn from the two
of the colleague's eight renders either side of the angle, every pixel of a
render being a world point (its depth) with a normal, so the picture between
two renders is exact.

The renders are the colleague's, in his graphics repository (grrrrf), each
direction d (the building turned d x 45 degrees clockwise seen from above) as
<prefix>_d<d>_zin16.png with _hloubka.png (16-bit depth, millimetres about
nula_m along the camera), _normaly.png (RGB = normal in the model's frame of
direction 0, alpha 255 measured, 128 filled in) and .json (the plot corners in
pixels, the camera: pocatek, vpravo_px, dolu_px, pohled -- world point of a
pixel = pocatek + (i + 0.5) vpravo_px + (j + 0.5) dolu_px + z pohled, Blender
metres, z up, the plot's north corner at the origin, the game's y along -y).

Every direction gives three sprites, 16x only (zin16, zoom code 7), each with
the plot's north corner as its anchor: the colour as it is, the depth as a
32bpp picture (R = high byte, G = low byte, A 255 where there is depth) and
the normals as they are. The sprites go in a block of the game's own
(SPR_DECOUPLE_3D_BASE in table/sprites.h) through Action A: a set of the
game's own graphics gives sprite numbers from 0x8000 on and the game puts them
at SPR_DECOUPLE_3D_BASE + (number - 0x8000) (SpriteReplace() in
src/newgrf/newgrf_acta.cpp). A model's 24 sprites are at first + d * 3 + kind.

The sets are packed under 90 MB each for GitHub. The sources (the PNG files)
stay in grrrrf and are not kept here; the .yagl of each set is, for reading.

    python3 openttd_3d_grf.py <grrrrf dir> [--jen-tabulka]
"""
import json, os, subprocess, sys
from pathlib import Path
import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
TABLE = HERE.parent.parent.parent.parent / "src" / "table" / "three_d_models.h"
#: raw bytes a set may hold (LIMIT_MB in the environment overrides): yagl's deflate makes 41 to 57 % of it, the
#: houses less, the statue, the machine and the hut more, so the sets in the repository were packed at 200 MB
#: and the last one, over 100 MB built, split again at 100 MB (--jen=... --sada-od=5)
LIMIT = int(os.environ.get("LIMIT_MB", "200")) * 1024 * 1024
GRF_LIMIT = 95 * 1000 * 1000 # what a built set may weigh (GitHub takes files under 100 MB)
YAGL = os.environ.get("YAGL", "yagl")

#: name, kind, id (HouseID or IndustryType name), folder in grrrrf, file prefix
MODELS = [
    ("dum_07_hotel", "house", "0x07", "domy/osm_smeru_zin16", "dum_07_hotel_1"),
    ("dum_09_socha", "house", "0x09", "domy/osm_smeru_zin16", "dum_09_statue_1"),
    ("dum_0a_fontana", "house", "0x0a", "domy/osm_smeru_zin16", "dum_0a_fountain_1"),
    ("dum_0b_park", "house", "0x0b", "domy/osm_smeru_zin16", "dum_0b_park_1"),
    ("dum_0c_park", "house", "0x0c", "domy/osm_smeru_zin16", "dum_0c_park_1"),
    ("dum_11_kancelare", "house", "0x11", "domy/osm_smeru_zin16", "dum_11_modern_office_building_1"),
    ("dum_13_blok", "house", "0x13", "domy/osm_smeru_zin16", "dum_13_office_block_3"),
    ("dum_1b_byty", "house", "0x1b", "domy/osm_smeru_zin16", "dum_1b_flats_1"),
    ("dum_1f_divadlo", "house", "0x1f", "domy/osm_smeru_zin16", "dum_1f_theater_1"),
    ("dum_28_stredisko", "house", "0x28", "domy/osm_smeru_zin16", "dum_28_shopping_mall_1"),
    ("gymnazium", "industry", "IT_GYMNASIUM", "gymnazium/osm_smeru_zin16", "gymnazium"),
    ("socha_kamen", "industry", "IT_STATUE", "socha/osm_smeru_zin16", "socha_kamen"),
    ("socha_bronz", "industry_variant", "IT_STATUE", "socha/osm_smeru_zin16", "socha_bronz"),
    ("automat", "industry", "IT_WEED_MACHINE", "automat/osm_smeru_zin16", "automat"),
    ("chatka", "industry", "IT_COFFEESHOP", "chatka/osm_smeru_zin16", "chatka"),
    # the marijuana plantation, a tile at a time: layers laid on one tile by the game (industry_cmd.cpp,
    # GetThreeDPlantationTile()); the folder holds a directory a direction, the file name has no direction
] + [
    (f"pole_{name}", "layer", "0", "pole/osm_smeru_zin16/d{d}", name) for name in (
        "mari_zaklad", "mari_male", "mari_vzrostle", "cesta", "bouda", "seno", "holky_sz", "holky_jv",
        "prace_real_f2", "prace_sedi_f2", "prace_punk_f2", "prace_pubg_f2", "prace_char16_f2", "prace_chill_f2",
        "prace_real_f3", "prace_sedi_f3", "prace_punk_f3", "prace_pubg_f3", "prace_char16_f3", "prace_chill_f3")
]

KIND_ENUM = {"house": "ThreeDModelKind::House", "industry": "ThreeDModelKind::Industry", "industry_variant": "ThreeDModelKind::IndustryVariant", "layer": "ThreeDModelKind::Layer"}


def depth_png(src: Path, dst: Path) -> int:
    """The 16-bit depth as a 32bpp picture: R high byte, G low byte, A 255 where measured. Returns the opaque pixels."""
    d = np.array(Image.open(src))
    if d.dtype != np.uint16:
        raise SystemExit(f"{src}: depth is not 16 bit")
    out = np.zeros(d.shape + (4,), np.uint8)
    out[..., 0] = d >> 8
    out[..., 1] = d & 0xFF
    out[..., 3] = np.where(d > 0, 255, 0)
    Image.fromarray(out, "RGBA").save(dst)
    return int((d > 0).sum())


def read_model(grrrrf: Path, name, kind, ident, folder, prefix):
    views = []
    for d in range(8):
        base = grrrrf / folder.format(d=d) / (f"{prefix}_zin16" if "{d}" in folder else f"{prefix}_d{d}_zin16")
        j = json.loads((base.parent / (base.name + ".json")).read_text())
        h = j["hloubka"]
        if h["stred"] != 32768 or abs(h["jednotka_m"] - 0.001) > 1e-9:
            raise SystemExit(f"{base}: depth not in millimetres about 32768")
        im = Image.open(str(base) + ".png")
        for suffix in (h["soubor"], j["normaly"]["soubor"]):
            other = Image.open(base.parent / suffix)
            if other.size != im.size:
                raise SystemExit(f"{base}: {suffix} is {other.size}, the colour {im.size}")
        sever = j["rohy"]["sever"]
        views.append({
            "colour": Path(str(base) + ".png"), "depth": base.parent / h["soubor"], "normals": base.parent / j["normaly"]["soubor"],
            "w": im.width, "h": im.height, "north_x": round(sever[0]), "north_y": round(sever[1]),
            "pocatek": h["pocatek"], "vpravo": h["vpravo_px"], "dolu": h["dolu_px"], "pohled": h["pohled"],
            "nula_m": h["nula_m"],
        })
        if d == 0:
            # the plot from its corners: the east corner is tiles_y tile edges (512 px each) right of the north
            # corner, the west corner tiles_x edges left of it (the game's x runs from the north corner to the west)
            rohy = j["rohy"]
            edge = 512.0
            tiles = [round((rohy["sever"][0] - rohy["zapad"][0]) / edge), round((rohy["vychod"][0] - rohy["sever"][0]) / edge)]
            given = j.get("dlazdic")
            if given is not None and list(given) != tiles:
                raise SystemExit(f"{base}: the corners say {tiles} tiles, dlazdic says {given}")
            policko = j["policko_m"]
    return {"name": name, "kind": kind, "id": ident, "tiles": tiles, "policko_m": policko, "views": views}


def c_float(x):
    s = f"{float(x):.7g}"
    if "." not in s and "e" not in s: s += ".0"
    return s + "f"


def c_floats(v):
    return "{" + ", ".join(c_float(x) for x in v) + "}"


def main():
    grrrrf = Path(sys.argv[1]).resolve()
    only_table = "--jen-tabulka" in sys.argv
    only = []   # --jen=<name>,<name>: build only these models' sets (a trial, or one set packed again)
    for a in sys.argv:
        if a.startswith("--jen="): only += a.split("=", 1)[1].split(",")
    first_set = next((int(a.split("=", 1)[1]) for a in sys.argv if a.startswith("--sada-od=")), 1)   # the number of the first set written
    models = [read_model(grrrrf, *m) for m in MODELS]

    # the sets: models packed in order, each set under the limit (a model's size is its opaque pixels times 12 bytes)
    sets = []
    for i, m in enumerate(models):
        m["first"] = i * 24
        if only and m["name"] not in only: continue
        size = 0
        for v in m["views"]:
            a = np.array(Image.open(v["colour"]).convert("RGBA"))[..., 3]
            v["opaque"] = int((a > 0).sum())
            size += v["opaque"] * 12
        m["size"] = size
        if not sets or sets[-1]["size"] + size > LIMIT:
            sets.append({"name": f"budovy3d_{len(sets) + first_set}", "models": [], "size": 0})
        sets[-1]["models"].append(m)
        sets[-1]["size"] += size

    # the table for the game
    lines = ["/*",
             " * This file is part of OpenTTD.",
             " * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.",
             " * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.",
             " * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.",
             " */", "",
             "/**",
             " * @file three_d_models.h The game's own buildings from eight directions with depth and normals (three_d.h,",
             " * ThreeDModel): which thing of the game each is, its plot, and for every direction the camera that took the",
             " * render (ThreeDModelView) and where the plot's north corner is in the picture. The sprites are in",
             " * baseset/decouple/grafika/budovy3d_*.grf, at SPR_DECOUPLE_3D_BASE + first + direction * 3 + kind (the colour,",
             " * the depth, the normals). Generated by media/baseset/decouple/grafika/openttd_3d_grf.py from the colleague's",
             " * renders (grrrrf); edit that, not this.",
             " */", "",
             "static const ThreeDModel _three_d_models[] = {"]
    for m in models:
        lines.append(f"\t{{\"{m['name']}\", {KIND_ENUM[m['kind']]}, {m['id']}, {m['tiles'][0]}, {m['tiles'][1]}, {c_float(m['policko_m'])}, {m['first']}, {{")
        for d, v in enumerate(m["views"]):
            lines.append(f"\t\t{{{c_floats(v['pocatek'])}, {c_floats(v['vpravo'])}, {c_floats(v['dolu'])}, {c_floats(v['pohled'])}, {c_float(v['nula_m'])}, {v['north_x']}, {v['north_y']}, {v['w']}, {v['h']}}}, // d{d}")
        lines.append("\t}},")
    lines.append("};")
    lines.append("")
    TABLE.write_text("\n".join(lines))
    print(f"{len(models)} models -> {TABLE}")
    if only_table:
        return

    too_big = []
    for s in sets:
        folder = HERE / s["name"]
        folder.mkdir(exist_ok=True)
        yagl = ["// The game's own buildings from eight directions with depth and normals, 16x: see openttd_3d_grf.py.",
                "// Built from the colleague's renders in grrrrf; the PNG files are not kept here.",
                "yagl_version: \"\";", "grf_format: Container2;", "grf // Action08", "{",
                f"    grf_id: \"DC3{s['name'][-1]}\";", "    version: GRF8;",
                f"    name: \"Decouple: buildings in 3D ({s['name']})\";",
                "    description: \"The game's own buildings from eight directions with depth and normals, for the turned view (OpenTTD Decouple by Karel Macha)\";",
                "}", "replacement_sprite_sets // Action0A", "{"]
        for m in s["models"]:
            yagl.append(f"    replacement_sprite_set<0x{0x8000 + m['first']:04X}> // {m['name']}: {m['kind']} {m['id']}, {m['tiles'][0]}x{m['tiles'][1]} tiles")
            yagl.append("    {")
            for d, v in enumerate(m["views"]):
                files = []
                col = f"{m['name']}_d{d}_barva.png"; dep = f"{m['name']}_d{d}_hloubka.png"; nrm = f"{m['name']}_d{d}_normaly.png"
                Image.open(v["colour"]).convert("RGBA").save(folder / col)
                depth_png(v["depth"], folder / dep)
                Image.open(v["normals"]).convert("RGBA").save(folder / nrm)
                for k, f in enumerate((col, dep, nrm)):
                    yagl.append(f"        sprite_id<0x{m['first'] + d * 3 + k + 1:08X}> // d{d} {('colour', 'depth', 'normals')[k]}")
                    yagl.append("        {")
                    yagl.append(f"            [{v['w']}, {v['h']}, {-v['north_x']}, {-v['north_y']}], zin16, c32bpp | chunked, \"{f}\", [0, 0];")
                    yagl.append("        }")
            yagl.append("    }")
        yagl.append("}")
        (folder / f"{s['name']}.yagl").write_text("\n".join(yagl) + "\n")
        grf = HERE / f"{s['name']}.grf"
        print(f"{s['name']}: {len(s['models'])} models, about {s['size'] / 1e6:.0f} MB raw; yagl ...", flush=True)
        subprocess.run([YAGL, "-e", str(grf), str(folder)], check=True, stdout=subprocess.DEVNULL)
        print(f"  -> {grf} {grf.stat().st_size / 1e6:.1f} MB")
        for p in folder.glob("*.png"):
            p.unlink()
        if grf.stat().st_size > GRF_LIMIT:
            too_big.append(grf)
    if too_big:
        raise SystemExit("over " + f"{GRF_LIMIT / 1e6:.0f} MB, pack again with a lower LIMIT_MB: " + ", ".join(str(g) for g in too_big))


if __name__ == "__main__":
    main()
