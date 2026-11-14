# Vendorovaný zdroj OpenTTD

Tenhle adresář je kopie zdrojového kódu OpenTTD verze **16.0-beta2**
(https://github.com/OpenTTD/OpenTTD, tag `16.0-beta2`, commit
`e4620d5832de45b5e0a4c4c1d4581684364ea082`), vložená přímo do tohoto
repozitáře 2026-08-15 (bez `.git` historie OpenTTD — další vývoj/úpravy
sleduje git tohoto repozitáře, `grrrrshadow/forclaude`).

**Nahradila předchozí vendorovanou verzi 15.3.** Důvody přechodu:
skutečné couvání vlaků (`DrivingBackwards`, ne magic-flip — uživatel to
výslovně vyžadoval) a NewGRF kompatibilita s CZTR grafikami, které cílí
na stejné schéma příznaků (`HasCab`, bit 11 v proměnné `0xFE`) jako
beta16. Celá práce na 15.3 (vendorovaný zdroj + naše úpravy pro
couple/decouple) zůstává zachovaná na větvi `backup-15.3-decouple` —
kdybychom se rozhodli vrátit, nic není ztraceno.

Důvod a rozhodnutí viz `../FEATURE_DESIGN_COUPLING_TOW.md` (sekce
"KONEC AKTIVNÍHO VÝVOJE NA 15.3 — přechod na OpenTTD 16.0-beta2") a
`../BUILD_NOTES.md` pro to, jak se z tohohle adresáře staví Windows
`.exe` přes GitHub Actions.

Originál je vždy dostupný na GitHubu — tohle není fork udržovaný pro
zpětné sledování upstreamu, je to jednorázová kopie k přímé úpravě.
