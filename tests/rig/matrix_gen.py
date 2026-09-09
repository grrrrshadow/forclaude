#!/usr/bin/env python3
"""Generate the crossed/same-side coupling matrix on rig.sav.

Each scene: four waiters (or deliverers dropping rakes) released slowly from
one side, then one collector plus three clones released one by one from the
left or from the right. Four spacing variants per combination, so every
combination yields 16 couplings and the whole matrix 64 per family.
Families: waiters (engine+2 wagons standing with CEKAT), wagons (rakes dropped
by ODPOJIT:vse deliverers), EMU (dual-headed waiters and collectors).
"""
import sys
import os
S = os.environ.get("RIG_DIR", os.path.dirname(os.path.abspath(__file__)))
_lines = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "battery.sh")).read().split("\n")
HEADER = "\n".join(_lines[:_lines.index("}") + 1]) + "\n"

# family -> (side -> units to release), (side -> collector original)
FAM = {
    "mx": ({"P": [17, 18, 19, 20], "L": [25, 26, 27, 28]}, {"L": 5, "P": 14}),
    "mw": ({"P": [9, 10, 11, 12], "L": [1, 2, 3, 4]}, {"L": 5, "P": 14}),
    "me": ({"P": [57, 58, 59, 60], "L": [33, 34, 35, 36]}, {"L": 37, "P": 61}),
    "ms": ({"P": [57, 58, 59, 60], "L": [33, 34, 35, 36]}, {"L": 5, "P": 14}),
    "mv": ({"P": [9, 10, 11, 12], "L": [1, 2, 3, 4]}, {"L": 37, "P": 61}),
}
# (waiter spacing, collector start, collector spacing)
VAR = [(700, 6000, 1200), (700, 6000, 800), (700, 6000, 2000), (1500, 8000, 1000)]
TICKS = 24000

def scene(fam, wside, cside, n):
    units, coll = FAM[fam]
    ws, t0, d = VAR[n - 1]
    c = coll[cside]
    lines = ["vlak123 on", "testpauza"]
    for i, u in enumerate(units[wside]):
        lines.append(f"testzatik {10 + i * ws} testbrzda {u}")
    lines.append(f"testzatik 20 testklon {c} 3 stoj")
    for i, u in enumerate([c, 77, 78, 79]):
        lines.append(f"testzatik {t0 + i * d} testbrzda {u}")
    name = f"{fam}_{wside}{cside}_{n}"
    body = "\n".join(lines)
    return f'run_scene {name} "{body}" {TICKS} -g $S/rig.sav\n'

fams = sys.argv[1:] or ["mx"]
scenes = [scene(f, w, c, n) for f in fams for w in "PL" for c in "LP" for n in (1, 2, 3, 4)]
lanes = {"ttdhome": [], "h2": [], "h3": []}
for i, sc in enumerate(scenes):
    list(lanes.values())[i % 3].append(sc)
for home, lst in lanes.items():
    with open(f"{S}/mx_{home}.sh", "w") as f:
        f.write(HEADER.replace("H=$S/ttdhome", f"H=$S/{home}"))
        f.write("".join(lst))
print(len(scenes), "scenes")
