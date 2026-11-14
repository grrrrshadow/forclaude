#!/usr/bin/env python3
"""
České návěstidlo (the player, 8. 11.: "semafory jeden český"; on the plan "2 jo"): the light signal the game stands at a
signal on our 3D track instead of its sprites (DrawThreeDSignal() of rail_cmd.cpp), of whatever kind and variant. Of the
player's model "Světelné návěstidlo" (maschinqa, 3D Warehouse, a SketchUp Collada) -- read by dae_cti.py (grrrrf
osobacky/) into an npz:

    python3 -I dae_cti.py model.dae cz.npz
    python3 navestidlo.py cz.npz

Written beside this script:

  cz_navestidlo.glb       the signal, its lights out: its mast's foot at the origin, its lights to +x (to the trains
                          coming), the head on the mast's top: top down yellow, green, red, white
  cz_svetlo_cervena.glb   its red light lit alone, in the signal's frame: a disc of its own light over the red lens (stop:
                          the game's red), drawn with the signal (the player, 9. 11.: "it is only another colour of the
                          light ... one object": not the whole signal again for each light)
  cz_svetlo_zelena.glb    its green lit (clear)
  cz_svetlo_zluta.glb     its top yellow lit (caution: the next signal at stop, the game's warning aspect)

The model's faces twice over (SketchUp's front and back): of two on the same three points the one with its own colour
kept. Its materials their colours (its one texture, the mast's light grey, flat); its domed lenses flat discs, each of its
own colour dull, always there (the player, 9. 11., of the AŽD posts: "it goes out but the light is still there"), the
lit one a disc of its own light over it (glTF emissiveFactor; its red a deep one: "darker, this is too orange").
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from azd import disc, write  # noqa: E402  (the glb writer and the lit disc of the AŽD posts)

MAST_X = -0.30                     # the mast's middle in the model (its foot the origin)
LENS_X = 0.245                     # the lenses' fronts
LENS_R = 0.085
LAMPS = {'zluta': 4.85, 'zelena': 4.51, 'cervena': 4.25, 'bila': 3.91}
LIT = {'zluta': [1.0, 0.78, 0.05], 'zelena': [0.1, 1.0, 0.35], 'cervena': [0.78, 0.0, 0.02]}
OUT = {'zluta': [0.36, 0.27, 0.03], 'zelena': [0.03, 0.28, 0.1], 'cervena': [0.3, 0.02, 0.03], 'bila': [0.55, 0.55, 0.53]}
SPECS = {
    'cerna': {'baseColorFactor': [0.08, 0.08, 0.08, 1.0]},
    'material_1': {'baseColorFactor': [0.86, 0.86, 0.86, 1.0]},   # its texture's mean, a light grey
    'material': {'baseColorFactor': [0.89, 0.88, 0.87, 1.0]},
    'seda': {'baseColorFactor': [0.6, 0.6, 0.6, 1.0]},
    'cervena': {'baseColorFactor': [0.8, 0.06, 0.05, 1.0]},
    'bila': {'baseColorFactor': [0.95, 0.95, 0.95, 1.0]},
    'Color_E01': {'baseColorFactor': [0.95, 0.85, 0.1, 1.0]},
}
for k, c in LIT.items():
    SPECS['svetlo_' + k] = {'baseColorFactor': c + [1.0], 'emissiveFactor': c}
for k, c in OUT.items():
    SPECS['cocka_' + k] = {'baseColorFactor': c + [1.0]}      # a lens out, of its colour


def without_twins(T, FM, has_colour):
    """Of two triangles on the same three points the one of a material with its own colour (SketchUp's back face goes)."""
    key = np.round(np.sort(T, axis=1).reshape(len(T), 9) * 1e4).astype(np.int64)
    _, inv = np.unique(key, axis=0, return_inverse=True)
    inv = inv.ravel()
    score = np.array([1 if has_colour[m] else 0 for m in FM])
    order = np.lexsort((-score, inv))
    first = np.r_[True, inv[order][1:] != inv[order][:-1]]
    keep = np.zeros(len(T), bool)
    keep[order[first]] = True
    return keep


def main():
    d = np.load(sys.argv[1], allow_pickle=False)
    V, F, FM = d['V'], d['F'], d['FM']
    mats = json.loads(str(d['mats']))
    names = [m['jmeno'] for m in mats]
    T = V[F].astype(float)
    keep = without_twins(T, FM, [m['jmeno'] != 'material' or m['barva'] is not None for m in mats])
    T, FM = T[keep], FM[keep]
    T[..., 0] -= MAST_X
    # Its domed lenses (a thousand triangles) flat discs, each of its colour, dull till lit.
    parts = [(names[mi], T[FM == mi]) for mi in sorted(set(FM.tolist())) if names[mi] != 'svetlo']
    parts += [('cocka_' + k, disc(LENS_X - MAST_X - 0.01, 0.0, z, LENS_R)) for k, z in LAMPS.items()]
    print('trojuhelniku', len(T), 'box', np.round(T.reshape(-1, 3).min(0), 2), np.round(T.reshape(-1, 3).max(0), 2))
    lens_x = LENS_X - MAST_X
    write(os.path.join(HERE, 'cz_navestidlo.glb'), 'cz_navestidlo', parts, SPECS)
    for k in LIT:
        write(os.path.join(HERE, f'cz_svetlo_{k}.glb'), f'cz_svetlo_{k}', [('svetlo_' + k, disc(lens_x, 0.0, LAMPS[k], LENS_R))], SPECS)


if __name__ == '__main__':
    main()
