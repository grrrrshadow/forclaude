# zastavka.grf: the girls at the bus stop in 16x

The first set of the game's own graphics under `baseset/decouple/grafika/`
(static in every game, savegames included; `AppendStaticGRFConfigs()` in
`src/newgrf_config.cpp`). It lays the 16x level (zin16, zoom code 7) over the
three girls of openttd.grf's drive-through bus stop with Action A, keeping
openttd.grf as it is -- the player's rule for all the 3D-era graphics
(`docs/decouple_three_d.md`).

- `zastavka.yagl`: the set, in yagl (the colleague's tool; no grfcodec).
- `zastavka_*_32bpp.png`, `zastavka_*_8x_32bpp.png`: the 4x and 8x levels,
  copies of openttd.grf's pictures (`media/baseset/openttd`), with offsets from
  `budovy_manifest.json`.
- `zastavka_*_16x_32bpp.png`: the 16x level, cut from the colleague's renders of
  the same girls from eight directions (grrrrf `postavy/osm_smeru_zin16*`,
  48.8 px/m, scale 2, no shadow), placed by the feet: offset = feet offset in
  the render + four times the 4x table point (`openttd_budovy.py`).
- Matylda (`SPR_BUS_STOP_GIRL_Y_NEAR`, 5165) is a second replacement set, from
  the colleague's render of her from eight directions (direction d7, the pose
  of the 4x and 8x). Model "Matilda" by nicolekeane, licence CC BY-NC-SA 4.0:
  this set and its pictures carry that licence and her name (`CREDITS.md`).

Build, from the repository root:

    yagl -e "$PWD/openttd/media/baseset/decouple/grafika/zastavka.grf" "$PWD/openttd/media/baseset/decouple/grafika/zastavka"

The built `zastavka.grf` is kept in the repository next to this folder; the
game's build copies every `grafika/*.grf` to `baseset/decouple/grafika/`.
