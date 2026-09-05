# Hráčovy savy ke scénám rigu

Savy, na kterých se měří konkrétní situace (viz TEMATA.md). Rig je načítá
přes `-g` a scény jsou v baterii.

- `eka.sav` — odtahovka 28 v depu (125,78), dvě řady s mašinkami uvnitř na
  nástupištích naproti (120,78) a (120,79). TEMATA 4.13 (odtahovka držená
  nedosažitelnou poruchou) a Nedořešeno (porucha ve vratech depa odtahovky).
- `vlak31.sav` — vlak 31 s „ignorovat návěst" před vjezdem do depa (97,73),
  kam napůl vjíždí vlak 32; odtahovka 30 v depu (112,75). TEMATA 4.14.
  Scéna `protlacit`: `testporucha 32` — porucha ve vratech, odtahovka 30 ji
  protlačí dovnitř (TEMATA 4.15).
- `porucha.sav` — vlak 2 porouchaný ve vratech depa (97,46), ohnutý přes
  výhybku; odtahovky 3 a 4 zabrzděné. Pád „Disconnecting train" na #132
  (TEMATA 4.15, druhé kolo). Scéna `poruchavrata`.
- `porucha_nastupiste.sav` — vlak 2 porouchaný na nástupišti (98,54–56),
  ocas na návěstidle (98,53); odtahovka 3 v depu (100,48) „nenajde cestu".
  Scéna `poruchanastup` (TEMATA 4.16).
- `porucha_za_vlakem.sav` — vlak 2 porouchaný na nástupišti (98,54–56),
  za ním stojí mašinka 5 na (98,50); odtahovka 4 v depu (97,46) musí jet
  předem, kolem přes jednosměrku (98,57). TEMATA 4.17 (zeď), 4.18 (srážky
  bez odtahovky), 4.19 (orientace po složení). Scéna `poruchazavlakem`.
- `emu.sav`, `emu_reverz.sav` — dvě depa, osm dvouhlavých jednotek, čtyři
  čekají na spojení, čtyři jedou spojit (v `emu_reverz` reverzně), pak
  odpojit a do depa. TEMATA 2.35. Scény `emu`, `emujz`.
- `loko_obou_stran.sav` — vlaky 20+ mašinka–vůz–mašinka z obyčejných
  lokomotiv; plán „odpojit připojený vlak" (TEMATA §3, plán). Scéna:
  20–23 pustit, 24 naklonovat 3×.
