# Návrh: Couple/Decouple za jízdy + Odtah porouchaných vlaků (tow)

Status: **návrh k diskuzi, žádný herní kód zatím nepsán.** Cílem tohoto
dokumentu je zapsat, co jsem nastudoval ve starém referenčním patchi a v
aktuálním 15.3, jaké konkrétní chyby ve starém patchi vznikly a proč, a
navrhnout architekturu, která se jim vyhne už v návrhu — ne až při ladění.

## Zdroje, ze kterých jsem vycházel

- `Palo123/OpenTTD-YPS`, branch `Decouple-wip` — naklonováno lokálně,
  prošel jsem `git log`, diff proti bodu odbočení od masteru a konkrétní
  "Fix: crash..." commity (to jsou nejcennější zdroj — autor sám
  pojmenoval, co přesně opravoval).
- Aktuální zdrojový strom OpenTTD 15.3 (`OpenTTD/OpenTTD`, tag `15.3`) —
  `src/train_cmd.cpp`, `src/train.h`, `src/vehicle.cpp`,
  `src/vehicle_base.h`, `src/command_type.h`, `src/order_type.h`,
  `src/order_base.h`.

## Zásadní zjištění č. 1: referenční patch je z prosince 2020

Branch `Decouple-wip` odbočuje od masteru na commitu z **2020-12-22** —
to je zhruba OpenTTD verze v éře 1.10.x/12.0, tedy **5+ let starý kód**.
Mezitím OpenTTD prošlo velkým refaktoringem command systému (viz níže) —
proto to "nejde použít na nové 15.3" doslova ve smyslu, že se to ani
nezkompiluje, ne že by jen chybělo pár drobností.

## Zásadní zjištění č. 2: v 15.3 je jiný command pattern

Starý patch používá starý styl:
```cpp
CommandCost CmdSellRailWagon(DoCommandFlag flags, Vehicle *t, uint16 data, uint32 p2)
```
Aktuální 15.3 používá typované, šablonové příkazy:
```cpp
Command<CMD_MOVE_RAIL_VEHICLE>::Do(DoCommandFlag::Execute, v->index, w->Last()->index, true)
```
s `DoCommandFlags` jako scoped-enum bitovou třídou (`flags.Test(DoCommandFlag::Execute)`),
typovanými ID (`VehicleID::Invalid()` místo `INVALID_VEHICLE`), atd. Každý
nový příkaz, který napíšeme, musí být od začátku v tomhle novém stylu —
copy-paste ze starého patche by se ani nezkompiloval.

## Co už v 15.3 existuje a na čem stavět

`CmdMoveRailVehicle` (`src/train_cmd.cpp:1221`) je **vanilla mechanismus
pro coupling/decoupling v depu** (přetahování vagonků v okně depa). Uvnitř
používá interní primitiva, která už řeší přesně tu "atomicitu", která ve
starém patchi chyběla:

- `MakeTrainBackup(original_src, src_head)` **a** `MakeTrainBackup(original_dst, dst_head)`
  — zálohuje **oba** vlaky, vždy, bez výjimky.
- `ArrangeTrains(...)` — fyzicky přeuspořádá spojové seznamy vozidel.
- `ValidateTrains(...)` — zkontroluje výsledek (nekompatibilní vozidla,
  příliš dlouhý vlak, atd.).
- Když validace selže → `RestoreTrainBackup(original_src); RestoreTrainBackup(original_dst);`
  — obnoví **oba** vlaky do původního stavu.
- Teprve když validace projde, provedou se viditelné vedlejší efekty
  (zavření oken, přepočet skupin, GUI invalidace).

Tohle je přesně ten vzor "zkusit nanečisto, ověřit, teprve pak
potvrdit/vrátit zpět", který chceme použít i pro couple/decouple **za
jízdy** (mimo depo) a pro odtah. `CmdMoveRailVehicle` navíc explicitně
vyžaduje `src_head->IsStoppedInDepot()` — to je přesně ta podmínka, kterou
naše nová logika nahradí jinou podmínkou (fyzická blízkost + zastavení +
správná orientace na trati), ale **zbytek vzoru (backup→arrange→validate→
commit/rollback) recyklujeme beze změny.**

## Zásadní zjištění č. 3: konkrétní chyby ve starém patchi (a jak se jim vyhnout)

### Bug A — funkce na vykreslování měnila herní stav (nejzávažnější nález)

Commit `1652c2545` ("Fix: do not change direction of train during
UpdateDeltaXY"). Původní kód:

```cpp
void Train::UpdateDeltaXY()
{
    if (HasBit(this->flags, VRF_REVERSE_DIRECTION)) direction = ReverseDir(direction);
    ...
```

`UpdateDeltaXY()` má být čistě výpočet bounding-boxu pro **vykreslení**
sprite. Ale tahle verze přímo přepisovala `this->direction` — což je
**autoritativní herní stav** používaný pohybem, pathfindingem, kolizemi.
Cokoliv, co zavolalo `UpdateDeltaXY()` (a to se volá běžně, i mimo
coupling), tak vedlejším efektem nečekaně otočilo skutečný směr jízdy
vozidla. Tohle je přesně ten typ chyby, co odpovídá hlášeným "explozím" —
ne bug přímo v couplingu, ale v tom, že vykreslovací kód nedopatřením
korumpoval fyzikální stav vlaku.

**Pravidlo pro náš návrh:** funkce, které jen *počítají* zobrazovací/
odvozené hodnoty (bounding box, sprite, offset), musí být `const` a brát
vozidlo přes `const*`, kdykoliv to architektura dovolí — ať to compiler
sám vynutí, ne spoléhat na disciplínu. Kdekoliv potřebujeme "otočený
pohled" pro vykreslení, spočítáme si lokální proměnnou (`display_direction`),
nikdy nepřepisujeme `this->direction`.

### Bug B — nesymetrická záloha/rollback při decouplingu

`TryTrainDecouple` (starý patch, `train_cmd.cpp:2172`):
```cpp
TrainList original_src;
TrainList original_dst;
MakeTrainBackup(original_src, v);
//MakeTrainBackup(original_dst, u);          <-- zakomentováno!
...
ArrangeTrains(&first_param, nullptr, &v, u, true);   // u už je fyzicky pozměněné
...
if (!ok) {
    RestoreTrainBackup(original_src);   // obnoví jen v, ne u!
    ...
}
```//
Deklarovaná proměnná `original_dst` se nikdy nenaplní a `u` (druhý vlak)
se při selhání validace vůbec neobnoví, i když `ArrangeTrains` do něj už
zasáhl. Pro srovnání, `TryTrainCouple` o pár set řádků výš **správně**
zálohuje obě strany — v jednom souboru tedy koexistují dvě różné úrovně
pečlivosti pro velmi podobnou operaci. To je přesně recept na "občas se to
podělá" chyby.

**Pravidlo pro náš návrh:** nebudeme mít pro couple, decouple a
tow-hookup/tow-detach čtyři různé kopie backup/arrange/validate/rollback
logiky. Uděláme **jedno sdílené interní primitivum** (např.
`TryConsistSplice(...)`), které vždy zálohuje obě strany, vždy validuje
před commitnutím, vždy symetricky obnoví při chybě — a couple, decouple i
tow ho jen volají s jinými vstupními parametry. Jedna cesta kódu = jedno
místo, kde to musí být správně, ne čtyři.

### Bug C — křehká heuristika "jsou vozidla vedle sebe" založená na pixelech

`GetCouplePosition()` (starý patch) určuje, jestli se dva vlaky mají
spojit, porovnáním **pixelové vzdálenosti** (`x_diff`, `y_diff`) s
očekávaným součtem polovin délek vozidel:
```cpp
if (diff == ((v_length + 1) / 2 + (u_length + 1) / 2)) { return u; }
```
Přesná rovnost (`==`) na součtu celočíselných zaokrouhlení je křehká —
u zakřivených tratí, článkovaných vozidel (articulated), nebo jen jiného
zaokrouhlení o 1 pixel to nesedí, coupling se prostě nespustí. To
odpovídá popsanému chování "občas to nechce couple".

**Pravidlo pro náš návrh:** adjacency nebudeme počítat z pixelové
vzdálenosti. Použijeme topologickou informaci, kterou hra už má
přesně (dlaždice + trackdir + track reservation, přes
`FollowTrainReservation`, které patch ostatně taky používá) — tedy "je
druhý vlak na sousední rezervované pozici na téže trati", ne "je jeho
střed v pixelech na správné vzdálenosti". Přesnost na dlaždici/trackdir
je diskrétní a deterministická, ne náchylná na zaokrouhlovací chyby.

### Bug D — sdílené bity ve struktuře `Order` pro různé typy příkazů

V `order_base.h` starý patch přidal:
```cpp
inline uint8 GetNumDecouple() const { return GB(this->decouple_flags, 1, 7); }
...
inline uint8 GetNumCouple() const   { return GB(this->decouple_flags, 1, 7); }   // STEJNÉ bity!
```
a podobně `GetCoupleLoad()` a `GetDecoupleFirstOrdersType()` čtou stejný
rozsah bitů (`this->flags, 0, 3`) pro dva sémanticky různé účely. Pokud se
typ příkazu v GUI změní (např. z "couple" na "decouple") a stará hodnota
bitů se nevynuluje, čte se "smetí" z předchozího typu příkazu jako platná
hodnota nového typu. To je přesně ten typ chyby, co vysvětluje "někdy to
vezme špatný počet vagonků / špatný druh nákladu".

**Pravidlo pro náš návrh:** žádné sdílení bitového rozsahu mezi
sémanticky odlišnými typy příkazů bez explicitní záruky (statická
kontrola, nebo prostě oddělené úložiště). Při každé změně typu příkazu v
GUI se všechny typo-specifické bity/pole exeplicitně vynulují, ne že se
jen přepíše `type` a zbytek se nechá "jak bylo".

### Bug E — automatické odhadování dělicího bodu bez potvrzení

`GetDecoupleVehicleAuto()` prochází vlak a heuristicky hádá, kde ho
rozdělit (podle pozic motorů/multiheaded jednotek), pokud hráč nezadal
explicitní počet. Funguje to jako tichý fallback — hráč nevidí, že se
použila heuristika, ani jaký výsledek dala, dokud se vlak nerozdělí jinak,
než čekal.

**Pravidlo pro náš návrh:** "auto" režim je v pořádku jako pomůcka, ale
vypočtená hodnota se **vždy zobrazí v UI k potvrzení** (jako předvyplněné
číslo, ne jako tichá skrytá logika), nikdy se neaplikuje bez zpětné vazby
hráči.

### Bug F — rezervace tratě (PBS) není součástí backup/rollbacku

Commit `f37a5219e` ("Fix: wrong reservation when reversing during
coupling") ukazuje, že coupling operace dělala vedlejší efekty na track
reservation (`ReverseTrainDirection`, které mění rezervace na trati) ještě
**před** finální validací/rozhodnutím, jestli se má coupling vůbec
provést. Rezervace trati je ale samostatný subsystém (mřížka na
dlaždicích), který `MakeTrainBackup`/`RestoreTrainBackup` vůbec
nezachycuje — takže i kdyby se vozidlový řetězec správně vrátil zpět při
selhání, rezervace na trati zůstane pozměněná.

**Pravidlo pro náš návrh:** místo aby se to řešilo zálohováním dalšího
subsystému, **přeuspořádáme pořadí operací tak, aby nic nevratného (včetně
změn rezervace) neproběhlo dřív, než je operace 100% validovaná.**
Nejdřív pouze **read-only** kontrola (jsou oba vlaky ve správné pozici,
orientaci, rychlosti nula, kompatibilní), teprve po jejím úspěchu
provedeme mutace (vozidlový řetězec i rezervaci) v jednom kroku. Tím
odpadá nutnost umět vracet rezervaci zpět, protože se k mutaci rezervace
vůbec nedostaneme, dokud nevíme jistě, že operace uspěje.

## "4 strany" mašinky / "2 strany" vagonku — přesný technický popis

V `train.h` jsem našel přesný mechanismus, na který jsi narážel: existuje
bit `VRF_REVERSE_DIRECTION` (per-vozidlo — je TATO konkrétní jednotka v
rámci soupravy "otočená") nezávislý na tom, jestli se **celá souprava**
pohybuje "dopředu" nebo "dozadu" po trati (to je jiná vlastnost —
`v->direction`/pohyb). Kombinace:

| souprava jede | vozidlo má `VRF_REVERSE_DIRECTION` | výsledná orientace |
|---|---|---|
| dopředu | ne | normální |
| dopředu | ano | otočená |
| dozadu (couvá/byla reversnutá) | ne | normální vzhledem ke směru couvání |
| dozadu | ano | otočená vzhledem ke směru couvání |

To je těch **"4 strany"** pro lokomotivu (má smysluplně odlišné chování
podle orientace — výkon, zvuk, atd. — proto se to musí řešit explicitně).
Vagonek je vůči těmto účinkům symetrický, takže mu **efektivně** stačí
řešit jen 2 stavy (otočený/neotočený vůči směru soupravy), ne kombinaci se
směrem jízdy.

**Návrh pro couple/decouple:** místo implicitního odvozování orientace z
`DirDifference()` dvou vektorů pohybu (jak dělal starý patch), uděláme
**explicitní, vyčíslitelnou tabulku kompatibility** — malý enum se 4 stavy
pro lokomotivu (Forward-Normal, Forward-Reversed, Backward-Normal,
Backward-Reversed) a funkci, která pro dvě soupravy na sousedních
dlaždicích **jednoznačně** určí, jestli/jak jde spojit, bez šedé zóny.
Něco, co jde napsat jako čistou pravdivostní tabulku a **otestovat
izolovaně** (unit-test-like, přes `regression/` sadu, kterou OpenTTD má),
ne jen "vyzkoušet ve hře a doufat".

## Návrh architektury (vysoká úroveň, zatím bez kódu)

### Sdílený základ (použije couple, decouple i tow)

1. **`TryConsistSplice`** — jediné interní primitivum pro "vzít dvě
   soupravy/části, přeuspořádat, validovat, buď commitnout, nebo vrátit
   obě zpět". Staví na existujícím `ArrangeTrains`/`ValidateTrains`/
   `MakeTrainBackup`+`RestoreTrainBackup` z `train_cmd.cpp`, ale
   parametrizované tak, aby šlo použít i mimo depo.
2. **Orientation compatibility tabulka** — čistá, testovatelná funkce,
   žádná pixelová heuristika.
3. **Adjacency check přes dlaždici/trackdir/rezervaci**, ne přes pixely.
4. **Read-only-check-first, mutate-only-after-success** pořadí operací
   všude, aby nebyl potřeba rollback stavu rezervace.

### Feature A — Couple/Decouple za jízdy (existuje ve starém patchi, přepisujeme od nuly)

- Nové příkazy v novém `Command<CMD_...>` stylu: něco jako
  `CMD_COUPLE_TRAINS`, `CMD_DECOUPLE_TRAIN`.
- Nové typy orderů (analogicky ke starému `OT_GOTO_COUPLE`/`OT_WAIT_COUPLE`/
  `OT_DECOUPLE`, ale s odděleným úložištěm parametrů — viz Bug D výše).
- Parametry v menu příkazů (jak jsi popsal): plný/prázdný, druh nákladu,
  počet vagonků — necháme koncept, ale hodnoty budou mít **vlastní**
  storage per order-type, ne sdílené bity.
- "Auto" počet vagonků k odpojení: spočítá se, ale ukáže se k potvrzení
  (Bug E).

### Feature B — Odtah porouchaných vlaků (nová, ve starém patchi vůbec není)

Navrhovaná state-machine (na vysoké úrovni, k diskuzi):

1. Vlak dostane breakdown (`Vehicle::HandleBreakdown()`, v 15.3 dnes:
   `breakdown_ctr` 2→1, vlak zastaví, po `breakdown_delay` tiků se
   `breakdown_ctr` vrátí na 0 a vlak jede dál **sám** — to je přesně to
   vanilla chování, které chceš zachovat jako fallback).
2. **Hák do tohoto přechodu**: v okamžiku `breakdown_ctr` 2→1 se ověří,
   jestli je pro tenhle vlak k dispozici "odtahová služba" (nová
   vlastnost/nastavení — přesná podoba k diskuzi, např. nový příkaz v menu
   "Service" podobně jak popisuješ).
   - **Není k dispozici** → nic se nemění, `HandleBreakdown()` běží přesně
     jak dnes (vanilla chování zachováno 1:1).
   - **Je k dispozici** → vlak přejde do nového stavu "čeká na odtah"
     (nezávislý na `breakdown_delay` countdownu — ten se pro tenhle vlak
     přestane odpočítávat/je irelevantní, dokud nepřijede odtah).
3. Odtahová mašinka (stojící/čekající podle vlastního "Service" příkazu v
   pořadí příkazů) dostane cíl = poloha porouchaného vlaku, dojede k němu,
   **spojí se s ním stejným sdíleným `TryConsistSplice` primitivem jako
   Feature A** (žádná duplicitní coupling logika).
4. Odtahová souprava (tow + porouchaný vlak) jede do nejbližšího/vhodného
   depa.
5. V depu: porouchaná část se **rozpojí** (opět stejné sdílené
   primitivum), opraví se (standardní servisní mechanismus depa, který v
   OpenTTD už existuje), a pokračuje podle svých **původních příkazů
   přesně od místa, kde skončily** (potřebujeme si při vzniku "čeká na
   odtah" stavu zapamatovat aktuální pozici v pořadí příkazů, aby se po
   opravě nezačínalo od začátku).
6. Odtahová mašinka se vrátí do svého "Service" čekacího režimu (nebo
   pokračuje dál podle vlastních příkazů, pokud takhle bude navrženo — k
   diskuzi).

## Datový model — co bude potřeba přidat (přehled, ne finální)

- `Train`/`Vehicle`: nový stav "čeká na odtah" + reference na
  přiřazenou odtahovou mašinku (nebo `INVALID` dokud není přiřazená) +
  uložená pozice v pořadí příkazů pro návrat po opravě.
- Nový/rozšířený typ příkazu v pořadí ("Service"/odtahová pohotovost) pro
  odtahové mašinky.
- Nové `Order` typy a jejich **vlastní** (ne sdílené) parametry pro
  couple/decouple.
- Savegame verzování (`saveload/*.cpp`) pro nová pole — 15.3 má vlastní
  aktuální chunk verzování, starý patch měnil `saveload.h`
  starým způsobem, to se bude muset udělat znovu podle dnešních zvyklostí
  15.3 (nekopírovat starý přístup).
- Nové řetězce do `lang/english.txt` pro GUI.

## Rozhodnutí padlá v diskuzi (2026-08-13)

### 1. Patch vs. přímá úprava zdrojáku → **přímá úprava**

Originál je vždy dostupný na GitHubu (OpenTTD/OpenTTD), takže není důvod
udržovat samostatný .patch/.diff — přímá úprava forkovaného zdrojového
stromu je jednodušší na vývoj i ladění a je to "o jednu problematickou
operaci míň" (aplikace patche na měnící se strom). Náš build pipeline
(`build-windows.yml`) to zvládne stavět z libovolného zdroje/branch beze
změny.

### 2. Multiplayer determinismus → **ano, musí být multiplayer-safe**

Hráno i v síti (sdílené koleje mezi hráči plánované jako další krok), takže
nový kód musí být deterministický napříč klienty stejně jako zbytek hry.
K tomu jsem při studiu zdrojáku 15.3 našel důležitou, konkrétní věc,
kterou musíme respektovat — **další konkrétní past k tomu, čemu se
vyhnout** (v `src/core/random_func.hpp`):

```cpp
extern Randomizer _random;              // "Random used in the game state calculations"
extern Randomizer _interactive_random;  // "Random used everywhere else, where it does
                                         //  not (directly) influence the game state"
```

OpenTTD má **dva** generátory náhodných čísel: `Random()` (synchronizovaný
napříč klienty, bezpečný pro cokoliv, co ovlivňuje herní stav — vanilla
`CheckVehicleBreakdown()` ho takhle používá při rozhodování o poruše) a
`InteractiveRandom()` (lokální, NENÍ synchronizovaný, používat jen pro
věci co neovlivňují herní stav, např. čistě kosmetické efekty na
klientovi). Záměna těchto dvou je klasický zdroj desyncu v multiplayeru u
patchů/modů. **Pravidlo pro náš kód:** cokoliv, co rozhoduje o dostupnosti
odtahu, výběru odtahové mašinky, atd., musí používat `Random()`
(synchronizovaný), nikdy `InteractiveRandom()`.

Zároveň to neznamená, že úplně všechno musí jít přes `Command`: pravidelná
per-tick simulace (jako dnešní `HandleBreakdown()`/pohyb vlaků) běží
identicky na všech klientech už dnes, aniž by to byl `Command` — je to
jen deterministický kód nad synchronizovaným stavem + synchronizovaným
RNG. Nová logika odtahu (kontrola dostupnosti, výběr nejbližší volné
mašinky, atd.) může fungovat stejně — jako součást běžné per-tick
simulace, ne jako zvláštní `Command`. `Command` potřebujeme jen pro věci,
které iniciuje hráč kliknutím (dát příkaz "Service", ruční couple/decouple).

### 3. Spouštěč odtahové služby → **vyhrazený příkaz v pořadí, odtahová mašinka čeká v depu**

Nový typ příkazu (pracovně "Service"/"Wait for breakdown") v seznamu
příkazů odtahové mašinky. Mašinka s tímhle příkazem **stojí v depu** a
čeká — to je hezké, protože depo je zároveň i cíl, kam se nakonec odtažená
porucha odveze, takže odtahová mašinka má vždy jasně definovaný "domov".

### 4. Porucha odtahové mašinky cestou k poruše → **vanilla fallback pro odtah, ale se zásadní podmínkou pro tu PŮVODNÍ poruchu**

Když odtah T1 jede zachránit porouchaný vlak A a T1 samo cestou spadne do
poruchy: T1 se chová přesně jako běžný porouchaný vlak (počká si
`breakdown_delay`, jede dál sama, žádný "odtah pro odtah").

Kritickou věc jsi ale správně pojmenoval: **jakmile je T1 vlaku A
přiřazena, A musí čekat na TOHLE konkrétní T1 bez časového limitu a bez
tichého návratu k vlastnímu pohybu** — i kdyby T1 zdrželo vlastní
poruchou. Kdyby A mělo možnost "to už čekám dost dlouho, pojedu si sám",
mohlo by se přesně stát to, co jsi popsal: T1 by pak dojelo na místo s
příkazem "couple" a nikdo by tam nebyl. Jakmile je tedy odtah přiřazený,
je to závazek na obou stranách, dokud se buď nespojí, nebo hráč
ručně nezasáhne (zruší službu, prodá vozidlo, apod.).

Po vlastním zotavení T1 z poruchy: **T1 prostě pokračuje ve svém aktuálně
rozjetém pohybu k A** — nemusí se nejdřív vracet do svého depa. Vanilla
`HandleBreakdown()` už dnes funguje přesně takhle (porucha jen pozastaví
pohyb, neresetuje aktuální cíl/order), takže tohle nepotřebuje žádnou
speciální logiku navíc — je to zadarmo díky tomu, jak breakdown systém
už funguje.

**Rozhodnuto:** pokud je v okamžiku poruchy A k dispozici víc volných
odtahů v depech, vybere se ten **nejbližší** (vzdálenost po trati/cestě k
A, ne vzdušnou čarou — upřesníme při implementaci, ale záměr je
"nejbližší reálně dosažitelný", ne jen geometricky nejbližší depo, které
třeba ani nemá spojení).

### 5. Fronta při víc poruchách najednou → **žádná fronta**

Pokud v okamžiku poruchy není žádný volný odtah, vlak se rovnou chová
jako ve vanille (chvíli počká, pak jede dál sám) — nečeká, až se nějaký
odtah uvolní později. Nejjednodušší a nejméně konfliktní varianta, přesně
jak jsi navrhl.

### 6. Automatický odhad počtu vagonků k odpojení (Bug E) → **nižší priorita**

Vysvětlil jsi, že jsi ve starém patchi vždy zadával explicitní počet
vagonků a fungovalo to dobře — takže "auto" odhad bez potvrzení není
problém, který jsi v praxi řešil. Necháme explicitní zadání počtu jako
hlavní/výchozí cestu (tak jak to znáš a funguje), auto-odhad zůstane jen
jako nepovinná pomůcka do budoucna, ne prioritní část návrhu.

### Poznámka do budoucna (nerozhodujeme teď)

Zmínil jsi sdílení kolejí mezi hráči jako další krok po tomhle. Nebudu to
teď navrhovat, ale budu na to myslet při psaní kódu, aby couple/decouple/
odtah nepředpokládaly bez důvodu "všechny vlaky na trati patří jedné
firmě" tam, kde to není nutné (typicky u výpočtu adjacency/orientace —
tam na vlastnictví nezáleží; záleží na něm až u otázek typu "kdo smí s kým
couplovat" nebo "kdo platí za odtah", což teď neřešíme).

## Co bude další krok

Až tohle probereme a doladíme rozhodnutí výše, napíšu konkrétní seznam
souborů/funkcí/Command ID a teprve pak začnu psát kód — postupně, jednu
ucelenou část (nejspíš nejdřív sdílený `TryConsistSplice` základ, protože
na něm stojí obě featury) a vždy s buildem přes náš zavedený CI pipeline,
abychom hned viděli, jestli něco nerozbilo kompilaci.
