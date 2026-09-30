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

Every `.grf` here goes into every new game, whatever its name or GRF ID; the
player sees it in the NewGRF window and may move it, but can neither remove it
nor switch it off, and no other set can switch it off.
