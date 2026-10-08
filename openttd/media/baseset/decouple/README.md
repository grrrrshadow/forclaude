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

- Teď je prázdná. Obrázky hry (gymnázium, automat, socha, dlaždice
  marihuanové plantáže, chatka hulírny, holky na zastávce, ohňostroj, plakát)
  jsou od 8. 10. 2026 rovnou v `openttd.grf` jako 32bpp zin8 (zin16, až budou
  rendery), protože se staví kolegovým yaglem, který 8× i 16× zná
  (`../openttd/openttd_yagl.py`, zdroj `openttd.yagl` a listy). `budovy.grf`,
  který tytéž obrázky nesl jako statický GRF, tím zanikl.
- Nový obrázek od kolegy: PNG do `../openttd/`, pak
  `python3 openttd_budovy.py && python3 openttd_yagl.py` tamtéž
  (viz `COMPILING.md`).

Kód: `AppendStaticGRFConfigs()` v `src/newgrf_config.cpp`.

Every `.grf` here goes into every new game, whatever its name or GRF ID; the
player sees it in the NewGRF window and may move it, but can neither remove it
nor switch it off, and no other set can switch it off. A `.grf` under
`grafika/` is a static set of the game's own graphics instead: in every game
and savegame, unseen in the NewGRF window, never saved, and it may not change
game state.
