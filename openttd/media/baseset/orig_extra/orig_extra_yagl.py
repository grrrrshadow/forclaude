#!/usr/bin/env python3
"""
Builds orig_extra.grf, the game's additions to the original graphics, with
yagl -- the colleague's yagl, the one tool this game's sets are made with (the
player's rule: no grfcodec). The source is orig_extra.yagl beside this script
with its sprite sheets, the set as yagl reads and writes it. Writes the GRF
and orig_extra.grf.hash, the checksum the game lists the set by.

    python3 orig_extra_yagl.py

yagl is looked for on the path, or at $YAGL (grrrrf, yagl/yagl-main).
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASESET = HERE.parent
GRF = BASESET / "orig_extra.grf"

yagl = os.environ.get("YAGL") or shutil.which("yagl")
if yagl is None:
    sys.exit("yagl not found: put it on the path or in $YAGL (built from grrrrf yagl/yagl-main)")
subprocess.run([yagl, "-e", "-b", GRF.name, HERE.name], cwd=BASESET, check=True)
md5 = subprocess.run([yagl, "-m", GRF.name], cwd=BASESET, check=True, capture_output=True, text=True).stdout.strip().splitlines()[-1]
(BASESET / "orig_extra.grf.hash").write_text(md5.lower() + "\n")
print(f"orig_extra.grf: {GRF.stat().st_size} bytes, md5 {md5.lower()}")
