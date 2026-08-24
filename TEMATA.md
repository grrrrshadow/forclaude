# Co už víme, po tématech

Tenhle soubor není deník. Je to **rejstřík**: podle tématu se dá najít, co je
o něm zjištěno a co je o něm rozhodnuto, aniž by se to muselo znovu vyvozovat
nebo znovu ptát. Zápisky z jednotlivých kol testů patří do `TEST_LOG.md`,
návrh funkce do `FEATURE_DESIGN_COUPLING_TOW.md`. Sem patří závěry, které
platí dál.

Pravidlo: **než se na něco zeptám podruhé, kouknu sem.**

---

## Čudlík posunu mapy (zaseknuté pravé tlačítko)

**Není to zařízením, na kterém se hraje.** Řečeno třikrát. Chová se to
stejně na počítači a je to tak roky. Každé vysvětlení, které začíná u
dotykového displeje, obalu nebo ovladače, je slepá ulice a už se tam
nehledá.

Co bylo zkoušeno a nezabralo:
- „levý je pán" — stisk levého ukončí tažení pravým. Ošetření následku,
  odstraněno.
- číst stav tlačítek každý snímek od systému. Nezabralo, a v jedné podobě
  to zhoršilo (zahazoval se úchop okna, na kterém stálo druhé tlačítko).

Kde je skutečná příčina: **ukončení tažení mapy stojí ve frontě za jinými
režimy.** `HandleViewportScroll()` je poslední z pěti obsluh v `MouseLoop()`
a každá předchozí smí událost zabrat a vrátit se dřív. Všechny čtyři
předchozí jsou režimy levého tlačítka (tažení okna, tažení v seznamu,
stavba kolejí). Dokud kterýkoliv z nich běží, **nikdo se nezeptá, jestli
má tažení mapy pokračovat** — a ono pokračuje. Proto to zdánlivě vyléčí
klepnutí levým: ukončí ten druhý režim.

Poučení, které platí i jinde: **ukončení režimu nesmí být schované za
předčasným návratem jiného režimu.**

## Směr jízdy, couvání, otáčení

Dvě různé věci, které se pletou:
- **otočení hlavy** (flip) — přehození pořadí vozů, mašinka se objeví na
  druhém konci. Nastavení `train_flip_reverse_allowed`. **Máme zamčeno na
  „Nikde"** a zůstává to tak. Vlaky se nikdy neotáčejí na trati.
- **vedoucí konec** — který konec jede napřed. Tohle řešíme.

Rozhodnuto: **kabina vede.** Vlak vyjíždí ze stanice mašinkou napřed
vždycky. Ne podle toho, co se zrovna vyplatí — to je nepředvídatelné a
hráč si podle toho nemůže nic naplánovat. Jediná výjimka je hlavové
nádraží, kde dopředu nevede vůbec žádná cesta; tam vlak vycouvá a hráč to
tak chtěl, protože ten slepý konec postavil.

Snížená rychlost při jízdě bez kabiny vpředu (32 km/h) **zůstává**.

Depo je zatáčka: vjezd a výjezd nemění, který konec vede, takže vlak vjede
mašinkou napřed a vyjede mašinkou napřed, jen míří na druhou stranu.

## Co beta 16 přinesla (a proto v ní děláme)

- `VehicleFlag::DrivingBackwards` + `GetMovingFront()` / `GetMovingBack()` /
  `GetMovingDirection()` — jízda pozpátku bez přehazování vozů.
- `ExtraEngineFlag::HasCab` — GRF příznak „tohle vozidlo má řídicí kabinu".
  Umí udělat vedoucí i z nemotorového vozu, tedy **řídicí vůz**.
- `CanLeadTrain()` = má kabinu **nebo** je to mašinka **nebo** zadní půlka
  dvojité mašinky. Tohle je ta správná otázka, ne starý příznak `RailIsMU`.
- Ořez rychlosti na 32 km/h, když vedoucí konec neumí vést.
- Nastavení `train_flip_reverse_allowed` (Kdekoliv / Jen na konci tratě /
  Nikde).

## Zkušební nádraží

Náčrtek a pravidlo zrcadla jsou v `FEATURE_DESIGN_COUPLING_TOW.md`,
kapitola „Zkušební sestava nádraží". **Bez něj se úkoly o spojování číst
nedají** — půlka nástupišť je zrcadlově obrácená a každé pravidlo, které
pojmenuje konec, je na jedné půlce správně a na druhé naopak.

## Nastavení, která hra zamyká

- `difficulty.train_flip_reverse_allowed` → vždy „Nikde"
- `pf.reverse_at_signals` → vždy vypnuto

Přepisují se při každém načtení, takže „zamčeno" opravdu znamená zamčeno i
pro starý soubor nastavení.

## Popisek bez místa ve stromu je půl práce

Nastavení má popisek (`str`, `strhelp`) **a** musí být přidané do stromu
stránek v `settingentry_gui.cpp`. Bez popisku se nezobrazí vůbec, bez
zařazení do stromu se najde jen přes vyhledávací pole. Stalo se u
`order.improved_load`.

## Fond objektů (pool) se musí ptát předem

`OrderList::CanAllocateItem()` není rada, je to povolení, které si sama
alokace kontroluje (`assert(this->checked != 0)`). Kdo si vezme seznam
příkazů bez zeptání, položí hru. Platí pro každý objekt z fondu.
