#!/usr/bin/env python3
"""Generate the god's eye sprites: the button icon and its yellow crosshair cursor.

Both pictures are icons from Icons8 (https://icons8.com -- see CREDITS.md), as
the player brought them (24. 10.: "under the question mark the icon of the
god's eye, the first person view moved there, and the crosshair instead of
the cursor"): openttdgui_oko_src.png, an eye in a triangle, 48x48, and
openttdgui_zamerovac_oko_src.png, a yellow crosshair, 32x32. They go in as
32bpp, alpha and all (the player's word: no palette pictures of ours).

* the button icon at the size of the main toolbar's, 20x20, and at twice
  that, 40x40, for a bigger interface: both scaled down from the 48 pixels,
  never drawn small;
* the cursor as it is, 32x32, hung on the mouse by its middle (its offsets
  in openttd_yagl.py, GUI_ICONS_32).

    python3 openttdgui_oko.py
"""
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent


def main() -> None:
    eye = Image.open(HERE / "openttdgui_oko_src.png").convert("RGBA")
    for size, name in ((20, "openttdgui_oko.png"), (40, "openttdgui_oko_2x.png")):
        eye.resize((size, size), Image.Resampling.LANCZOS).save(HERE / name)
    cross = Image.open(HERE / "openttdgui_zamerovac_oko_src.png").convert("RGBA")
    if cross.size != (32, 32):
        raise SystemExit(f"the crosshair is {cross.size}, its block is 32x32")
    cross.save(HERE / "openttdgui_zamerovac_oko.png")


if __name__ == "__main__":
    main()
