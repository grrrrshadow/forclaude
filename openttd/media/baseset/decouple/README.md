# Vestavěné GRF hry (the game's own sets)

Každý `.grf` v této složce jde do každé nové hry, ať se jmenuje jakkoli a má
jakékoli GRF ID. Hráč ho v okně NewGRF vidí a může ho posunout, ale nemůže ho
odebrat ani vypnout; nevypne ho ani jiný GRF (Action E). Jsou tu proto, aby
vždycky něco odvezlo všech 128 nákladů velkého průmyslu hry.

- Přidat GRF = dát ho sem. Hra ani CMake se kvůli tomu nemění
  (`CMakeLists.txt` o složku výš bere `decouple/*.grf`).
- Ve hře se čte ze složky `baseset/decouple/`.
- Rozehrané savy se nemění: vestavěné GRF dostane jen nová hra.

Kód: `AppendBuiltinGRFs()` v `src/newgrf_config.cpp`.

## grafika/ – grafika hry (the game's own graphics)

Každý `.grf` v podsložce `grafika/` je něco jiného: **statický** GRF hry, tedy
jen obrázky. Je v každé hře včetně rozehraných savů, hráč ho v okně NewGRF
nevidí, nikam se neukládá a nesmí měnit stav hry (hra ho čte jako statický GRF,
stejně jako ty, které si hráč zapíše do `[newgrf-static]`).

- Obrázky hry (gymnázium, automat, socha, dlaždice marihuanové plantáže,
  chatka hulírny, holky na zastávce, ohňostroj, plakát) jsou od 8. 10. 2026
  rovnou v `openttd.grf` jako 32bpp zin4 a zin8, protože se staví kolegovým
  yaglem (`../openttd/openttd_yagl.py`, zdroj `openttd.yagl` a listy).
  `budovy.grf`, který tytéž obrázky nesl jako statický GRF, tím zanikl.
- **Od 12. 10. 2026 (3D) se `openttd.grf` nemění** (hráč) a 16× obrázky
  (zin16) jdou sem, každý GRF se zdroji v podsložce svého jména a postavený
  yaglem (`yagl -e <abs>/jmeno.grf <abs>/jmeno`), pod 100 MB kvůli GitHubu:
  `zastavka.grf` – holky na zastávce v 16× přes sprity `openttd.grf`
  (Action A, `zastavka/README.md`); `budovy3d_<sada>_V<verze>.grf` – rendery
  budov z osmi směrů s hloubkou a normálami pro otočený pohled
  (`docs/decouple_three_d.md`), staví je `openttd_3d_grf.py` z kolegových
  renderů (grrrrf), tabulku pro hru píše do `src/table/three_d_models.h`;
  zdrojové PNG se tu nedrží, jen `.yagl`. Sprity jdou do bloku hry
  `SPR_DECOUPLE_3D_BASE` přes Action A s čísly od 0x8000 (jen sady grafiky hry
  smějí). **Verze v názvu** (hráč): změněná sada dostane `_V2`, `_V3`… a stará
  z repa zmizí (`--verze=N` generátoru), takže hráč stáhne jen to, co se
  změnilo. Build Windows tyhle sady nebalí (`.github/workflows/build-windows.yml`),
  hráč je má v `baseset/decouple/grafika/` nainstalované hry.
- Nový obrázek od kolegy ve 4× a 8× do `openttd.grf`: PNG do `../openttd/`,
  pak `python3 openttd_budovy.py && python3 openttd_yagl.py` tamtéž
  (viz `COMPILING.md`).

Kód: `AppendStaticGRFConfigs()` v `src/newgrf_config.cpp`.

Every `.grf` here goes into every new game, whatever its name or GRF ID; the
player sees it in the NewGRF window and may move it, but can neither remove it
nor switch it off, and no other set can switch it off. A `.grf` under
`grafika/` is a static set of the game's own graphics instead: in every game
and savegame, unseen in the NewGRF window, never saved, and it may not change
game state.

## objekty/ – budovy jako skutečné 3D objekty

Každá podsložka `objekty/<sada>/` je sada budov jako 3D objektů (glTF `.glb`,
formát v grrrrf `hra/3d/OBJEKTY-3D.md`) se seznamem `objekty.json`, který říká,
který dům původní hry a které jeho barevné varianty každý objekt nahrazuje.
Hra je čte z `baseset/decouple/objekty/<sada>/` a kreslí je v otočeném pohledu
a v pohledu z kabiny; při pohledu na sever zůstávají sprity.

- `mirne/` – všech 21 domů mírného klimatu původní hry, 31 souborů (od kolegy,
  grrrrf `hra/3d/objekty/mirne/`, textury z OpenGFX 7.1, GPL-2.0).
- `subarkticke/` – 19 objektů, domy subarktického klimatu (od kolegy, grrrrf
  `hra/3d/objekty/subarkticke/`). Dům pod sněžnou čárou a tentýž nad ní je jeden
  objekt: `nahrazuje_house` je seznam `[pod sněhem, nad sněhem]` a `snih` jméno
  PNG vedle `.glb` se zasněženou texturou přes tytéž UV.
- `subtropicke/` – 15 objektů, domy subtropického klimatu (od kolegy, grrrrf
  `hra/3d/objekty/subtropicke/`).
- Domy se hledají podle čísla, v kterémkoli klimatu: i v tematickém městě
  mírného klimatu, které staví arktické nebo pouštní domy („Domy z“).
- Přidat sadu = dát sem složku se soubory a `objekty.json`; CMake ji zkopíruje
  (`.glb`, `.png` a `objekty.json`).

Kód: `src/three_d_objects.cpp`, popis v `docs/decouple_three_d.md`.
