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
