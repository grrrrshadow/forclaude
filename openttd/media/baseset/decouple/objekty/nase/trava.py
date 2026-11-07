#!/usr/bin/env python3
"""
Trava (the player, 3. 11.: "a grassy surface for the temperate climate, lower growth, like the farm's fields ... low,
sparse grass, but it hides much of the chequered original ground"): our own picture of short grass, 512 x 512 pixels
over 2 x 2 tiles of the game, the same at every edge (laid over the whole map without a seam). Its colours about
their mean only: the game tints it to the mean of its own grass sprite, so it stays the colour of the land round it.
Written as trava_mirna.png beside this script, 32bpp.

    python3 trava.py
"""
import os

import numpy as np
from PIL import Image

SIZE = 512
SEED = 20261103


def periodic_noise(rng, size, cutoff):
    """White noise low-passed on the torus (in the frequencies, so its edges meet), -1 to 1."""
    white = rng.standard_normal((size, size))
    f = np.fft.fftfreq(size)
    k = np.sqrt(f[None, :] ** 2 + f[:, None] ** 2) * size
    smooth = np.real(np.fft.ifft2(np.fft.fft2(white) * np.exp(-(k / cutoff) ** 2)))
    smooth -= smooth.mean()
    return smooth / np.abs(smooth).max()


def main():
    rng = np.random.default_rng(SEED)
    # Patches of lighter and darker grass a few metres across, clumps of a few decimetres, single blades.
    patches = periodic_noise(rng, SIZE, 6.0)
    clumps = periodic_noise(rng, SIZE, 40.0)
    fine = periodic_noise(rng, SIZE, 160.0)
    shade = 0.16 * patches + 0.10 * clumps + 0.07 * fine
    # Dryer, yellower places here and there.
    dry = np.clip(periodic_noise(rng, SIZE, 9.0) * 1.6 - 0.55, 0.0, 1.0)

    base = np.array([86.0, 126.0, 50.0])
    yellow = np.array([128.0, 136.0, 62.0])
    img = base[None, None, :] * (1.0 + shade[:, :, None])
    img = img * (1.0 - 0.45 * dry[:, :, None]) + yellow[None, None, :] * (0.45 * dry[:, :, None]) * (1.0 + shade[:, :, None])

    # Blades: short strokes leaning every way, lighter tips and darker roots, drawn round the edges (modulo the size).
    light = np.array([132.0, 172.0, 74.0])
    dark = np.array([50.0, 82.0, 30.0])
    for _ in range(26000):
        x0, y0 = rng.uniform(0, SIZE, 2)
        a = rng.uniform(0, 2 * np.pi)
        length = rng.uniform(2.0, 6.0)
        colour = light if rng.random() < 0.55 else dark
        strength = rng.uniform(0.25, 0.55)
        steps = int(length * 2) + 1
        for s in range(steps):
            t = s / max(1, steps - 1)
            x = int(x0 + np.cos(a) * length * t) % SIZE
            y = int(y0 + np.sin(a) * length * t) % SIZE
            w = strength * (0.5 + 0.5 * t)
            img[y, x] = img[y, x] * (1.0 - w) + colour * w

    rgba = np.zeros((SIZE, SIZE, 4), dtype=np.uint8)
    rgba[:, :, :3] = np.clip(img, 0, 255).astype(np.uint8)
    rgba[:, :, 3] = 255
    Image.fromarray(rgba, 'RGBA').save(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'trava_mirna.png'), optimize=True)


if __name__ == '__main__':
    main()
