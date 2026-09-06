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
- `loko_obou_stran.sav` — mašinka–vůz–mašinka z obyčejných lokomotiv:
  17–20 čekají na spojení, 21–24 jsou sběračky (odpojit vše na stanici 3,
  pak depo). „Odpojit celý vlak" (TEMATA §3). Scény `lokonula` (nechat si
  0) a `lokocely` (celý vlak): 17–20 pustit, po 8 000 ticích 21 naklonovat
  3×.
- `odtah_peron.sav` — porucha 37 uprostřed nástupiště (108–111,72), vedle
  souběžné nástupiště téže stanice; odtahovka (v rigu vlak 36 přes
  `testodtahovka`) z depa (97,73). TEMATA 4.20. Scéna `odtahperon`.
- `new1.sav` — nádraží se dvěma nakládacími perony za nádražním
  směrováním „load" a třetím za „peron3", kde stavitelka (vlak 2) zakládá
  dlouhou řadu; klasické směrování „2" na slepé koleji, vlak 1 vozí vozy z
  depa (106,92). TEMATA 2.38 (založit za směrováním), 4.22 (puštěné držení
  bez záboru). Scéna `zalozsmer`. Otevřené: odtahovka na vagonky (§16).
- `obmena.sav` — čtyři vlaky čekají na nástupišti (151,166–169), dva
  couvají; mašinky mají nastavenou náhradu, depa (139,167) a (164,167),
  směrování „0" na slepé koleji (147,160). TEMATA 4.23 (směr přes obměnu:
  `testskip 1`, `testskip 2`) a §16 (crash obměny nad odtahovkou
  s poruchou: `testpostav 139 167 8 odtahovka`, `testporucha 1`).
