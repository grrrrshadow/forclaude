#!/usr/bin/env python3
"""
Zarážedlo (the player, 8. 11.: "the buffer stop on the track", "jo zarážedlo"): the Štůrc buffer stop the game stands at
each dead end of our 3D track (DrawThreeDBufferStop() of rail_cmd.cpp). Of the player's model by Petr Š. (3D Warehouse,
a SketchUp Collada) only the stop -- its frame of bent rails, its wooden beam, its two buffers, the board on it; not the
model's land, ballast, its two rails 26 m long nor SketchUp's figure -- read by dae_cti.py (grrrrf osobacky/) into an npz:

    python3 -I dae_cti.py model.dae petr.npz
    python3 zarazedlo.py petr.npz

Written beside this script:

  zarazedlo.glb         the stop on the end of the track: the track along x, its buffers to +x (to the trains coming),
                        their fronts at the origin on the track's middle, the stop behind them (1.52 m); raised so that
                        it stands on our rails (their heads 0.372 m, koleje.py RAIL_TOP_M; the model's 0.29 m), its
                        buffers 1.04 m over them
  zarazedlo_kolej.glb   the same on its own piece of our track 1.8 m long behind the buffers (embankment, three wooden
                        sleepers, rails): where the game's track ends at a tile's edge and the train's front comes to it,
                        the stop stands beyond the edge on that piece

Its materials their colours (the model's textures of rusted metal, cherry wood and rough metal flat, a little darker).
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from azd import box, write  # noqa: E402  (the glb writer and a box of the AŽD posts)

STOP_GROUPS = ('SketchUp/group_1',)   # the stop and its parts (their names start so)
TRACK_X = 12.985                      # the track's middle in the model, its end (the stop's back) at y
BACK_Y = 21.97
RAISE = 0.372 - 0.29                  # onto our rails' heads
SPECS = {
    'Metal_Rusted': {'baseColorFactor': [0.47, 0.29, 0.2, 1.0]},
    'Wood_Cherry_Original': {'baseColorFactor': [0.42, 0.26, 0.14, 1.0]},
    'Metal_Rough': {'baseColorFactor': [0.5, 0.51, 0.52, 1.0]},
    'Color_008': {'baseColorFactor': [0.12, 0.12, 0.12, 1.0]},
    'Color_005': {'baseColorFactor': [0.45, 0.45, 0.45, 1.0]},
    'Color_001': {'baseColorFactor': [0.89, 0.89, 0.89, 1.0]},
    'Color_I02': {'baseColorFactor': [0.2, 0.2, 1.0, 1.0]},
    'nasep': {'baseColorFactor': [0.42, 0.4, 0.37, 1.0]},
    'prazec': {'baseColorFactor': [0.25, 0.19, 0.145, 1.0]},
    'kolejnice': {'baseColorFactor': [0.38, 0.26, 0.19, 1.0]},
    'hlava': {'baseColorFactor': [0.7, 0.71, 0.74, 1.0]},
}
STUB = 1.8                            # its piece of track behind the buffers, metres


def stub():
    """A piece of our track (koleje.py's measures) from -STUB to 0 along x: its embankment, sleepers, rails, heads."""
    gauge, rail_w, top = 1.435, 0.072, 0.372
    parts = [('nasep', box(-STUB, 0.0, -1.7, 1.7, 0.0, 0.12))]
    for c in (-0.3, -0.9, -1.5):
        parts.append(('prazec', box(c - 0.13, c + 0.13, -1.3, 1.3, 0.11, 0.18)))
    for side in (-1, 1):
        mid = side * (gauge / 2 + rail_w / 2)
        parts.append(('kolejnice', box(-STUB, 0.0, mid - rail_w / 2, mid + rail_w / 2, 0.18, top - 0.004)))
        parts.append(('hlava', box(-STUB, 0.0, mid - rail_w / 2, mid + rail_w / 2, top - 0.004, top)))
    return parts


def main():
    d = np.load(sys.argv[1], allow_pickle=False)
    V, F, FM, FG = d['V'], d['F'], d['FM'], d['FG']
    names = [m['jmeno'] for m in json.loads(str(d['mats']))]
    groups = json.loads(str(d['skupiny']))
    keep = np.array([any(groups[g].startswith(s) for s in STOP_GROUPS) for g in FG])
    T = V[F[keep]].astype(float)
    M = FM[keep]
    # The model's -y (to the trains) our +x: a quarter turn, its buffers' fronts the origin on the track's middle.
    x, y, z = T[..., 0] - TRACK_X, T[..., 1] - BACK_Y, T[..., 2] + RAISE
    T = np.stack([-y, x, z], axis=-1)
    T[..., 0] -= T[..., 0].max()
    parts = [(names[mi], T[M == mi]) for mi in sorted(set(M.tolist()))]
    lo, hi = T.reshape(-1, 3).min(0), T.reshape(-1, 3).max(0)
    print('trojuhelniku', len(T), 'box', np.round(lo, 3), np.round(hi, 3))
    write(os.path.join(HERE, 'zarazedlo.glb'), 'zarazedlo', parts, SPECS)
    write(os.path.join(HERE, 'zarazedlo_kolej.glb'), 'zarazedlo_kolej', parts + stub(), SPECS)


if __name__ == '__main__':
    main()
