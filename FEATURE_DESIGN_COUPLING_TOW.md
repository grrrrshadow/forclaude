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

### Jednosměrné semafory a směr odtahu (2026-08-14, opraveno)

Původní verze tohohle bodu (níže přeškrtnuto pro záznam) byla špatně
domyšlená. Skutečný problém: odtah T1 dojede k poruše A po nějaké trati —
tou samou tratí se ale typicky musí **vrátit zpátky** (domovské depo bývá
přesně tam, odkud T1 přijelo, ne dál "dopředu" ve směru, kterým A
původně jelo). Jednosměrné semafory na téhle trati T1 pustí tam, ale ne
zpátky — to je reálný, běžný stav věcí v normální hře (T1 se přece
vydává z depa "dopředu" po vlastní trati, ne po cizí).

~~odtah nikdy nemá couvat zpátky tam, odkud přijel~~ — **zrušeno,
nahrazeno níže.**

~~přehrání přesné cesty pozpátku, žádné vyhodnocování semaforů na
zpáteční cestě vůbec~~ — **zrušeno, opraveno níže.** Tohle bylo
nebezpečně špatně: "přehrát cestu a nevyhodnocovat semafory" by
znamenalo ignorovat i to, k čemu semafory doopravdy slouží — nejde jen o
povolený směr, ale o **ochranu proti tomu, aby dva vlaky byly ve stejném
úseku trati zároveň.** Kdyby odtah jel zpátky bez ohledu na semafory,
mohl by se čelně srazit s vlakem, který mezitím tou samou tratí jede
správným směrem. Díky za odchycení, tohle by bez opravy skončilo přesně
tím druhem "vlak vybouchne" bugu, kterému se celou dobu snažíme
vyhnout.

**Opravené řešení (dvě části, obě potřeba):**

1. **Odtah si musí celou zpáteční cestu nejdřív rezervovat jako
   výhradní**, ještě než se s poruchou vydá zpátky — přesně to samé, co
   dnes dělá rezervační systém pro běžnou jízdu vpřed (jeden vlak = jeden
   rezervovaný úsek, nikdo jiný do něj nesmí vjet), jen rozšířené tak, aby
   šlo rezervovat i proti směru jednosměrného semaforu. Nejde o "vypnutí"
   kolizní ochrany — ta zůstává plně v platnosti (žádný jiný vlak do
   rezervovaného úseku nevjede, počká na jeho hranici stejně, jako dnes
   čeká na obsazený úsek) — jen umožňujeme, aby si rezervaci mohl v tomhle
   jednom případě vzít vlak jedoucí "špatným" směrem. Tohle je jediný
   bezpečný způsob, jak dovolit odtahu vrátit se stejnou tratí, ne
   obcházení semaforů jako takové.
2. **Vizuálně se souprava neotáčí** — po spojení se odtah + porouchaný
   vlak chová jako obousměrná souprava s lokomotivou na obou koncích
   ("top and tail", běžná reálná železniční praxe), ne jako jedna
   souprava co se otočí a jede opačně. Vanilla 15.3 tohle dnes neumí (jen
   "multiheaded" dvojice lokomotiv koupené jako pár mají grafiku pro obě
   strany, není to obecný nástroj). Budeme muset přidat vlastní obdobu
   toho, co měl starý patch (`VRF_REVERSE_DIRECTION` — bit "tahle
   konkrétní jednotka je vizuálně otočená, i když souprava/pohyb jede
   jinam") — **ale s ponaučením z Bug A**: ten flag smí ovlivnit jen
   vykreslení, nikdy nesmí přepsat skutečný `direction` (přesně tam
   starý patch udělal chybu, co způsobovala "výbuchy").

Tohle je větší kus enginové práce (nová vlastnost vozidla pro
vykreslování + rozšíření rezervačního systému o rezervaci proti směru
jednosměrného semaforu, se zachovanou kolizní ochranou) — bude to
samostatný krok až po tom, co budeme mít stabilní couple/decouple.
Zapsáno teď, abychom na to nezapomněli a nenavrhli tow logiku, která by
na tohle nebrala ohled.

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

## Technický plán — soubory, funkce, Command ID

Ověřeno přímo v `src/command_type.h` a `src/train_cmd.h`, jak se v 15.3
dnes registruje nový příkaz (na příkladu `CMD_MOVE_RAIL_VEHICLE`):

1. Nová hodnota v `enum Commands` (`command_type.h`).
2. Deklarace `CommandCost CmdXxx(DoCommandFlags flags, ...);` v
   `train_cmd.h`.
3. `DEF_CMD_TRAIT(CMD_XXX, CmdXxx, CommandFlag::Location, CommandType::VehicleManagement)`
   ve stejném souboru.
4. Tělo v `train_cmd.cpp`.
5. Volání přes `Command<CMD_XXX>::Do(DoCommandFlag::Execute, ...)` (interně)
   nebo `::Post(...)` (z GUI, s chybovou hláškou).

### Nové Command ID (návrh, čísla/pořadí upřesním při psaní)

| Command | Účel | Kdo volá |
|---|---|---|
| `CMD_COUPLE_TRAINS` | Spojit dvě soupravy na trati | hráč (GUI) i naše per-tick tow logika |
| `CMD_DECOUPLE_TRAIN` | Rozpojit soupravu na trati | hráč (GUI) |
| `CMD_MODIFY_ORDER` (existující, rozšířit) | Nastavit parametry couple/decouple/service orderu | hráč (GUI, order window) |

Odtah **nepotřebuje vlastní couple/decouple Command** — jakmile tow
dorazí k porouchanému vlaku, zavolá stejný `CMD_COUPLE_TRAINS` jako hráč
by zavolal ručně (jen ho spustí naše per-tick simulace, ne kliknutí).
Stejně tak rozpojení v depu po opravě je stejný `CMD_DECOUPLE_TRAIN`. To
je přesně ten "jeden sdílený mechanismus místo čtyř kopií" z návrhu výše.

### Sdílené primitivum: `TryConsistSplice`

Návrh umístění: nová statická funkce v `train_cmd.cpp`, vedle
`ArrangeTrains`/`ValidateTrains`/`MakeTrainBackup`/`RestoreTrainBackup`
(existující funkce, které využije beze změny). Návrh signatury:

```cpp
/**
 * Zkusí přeuspořádat dvě soupravy (spojit, nebo rozpojit v daném bodě) a
 * validovat výsledek. Při neúspěchu obě soupravy symetricky vrátí do
 * původního stavu. Nemutuje nic mimo vozidlový řetězec (žádná rezervace
 * trati) — to je odpovědnost volajícího, provést až PO úspěšném volání.
 */
static bool TryConsistSplice(Train **head_a, Train *split_point_a,
                              Train **head_b, Train *split_point_b,
                              bool move_chain);
```

Použije se z:
- refaktorovaného `CmdMoveRailVehicle` (beze změny chování — jen extrakce
  existující logiky, viz "První krok" níže),
- nového `CmdCoupleTrains` (couple na trati),
- nového `CmdDecoupleTrain` (decouple na trati),
- per-tick tow logiky (couple s porouchaným vlakem, decouple v depu po
  opravě).

### Nová pole (návrh, upřesním při psaní konkrétní části)

- `Train`: stav "čeká na odtah" (enum/bit), `assigned_tow` (VehicleID
  přiřazeného odtahu nebo `VehicleID::Invalid()`), uložený
  `VehicleOrderID` pro návrat k původním příkazům po opravě.
- `Order`: vlastní (ne sdílené, viz Bug D) pole pro couple/decouple
  parametry a pro nový "Service"/tow-wait order typ.
- `saveload/vehicle_sl.cpp`, `saveload/order_sl.cpp`: nová pole = nová
  savegame chunk verze, podle dnešních zvyklostí 15.3 (ne postup starého
  patche).
- `lang/english.txt`: nové řetězce pro GUI (chybové hlášky, popisky
  příkazů).

## První krok implementace — čistý refaktoring, žádná nová funkčnost

Než přidáme jakékoliv nové chování, uděláme **jeden samostatný commit**,
který jen vytáhne existující logiku z `CmdMoveRailVehicle` do
`TryConsistSplice` a `CmdMoveRailVehicle` ji zavolá — beze změny chování.
Cíl: mít jistotu, že extrakce je bezpečná (build zůstane zelený, hra se
chová identicky), než na tomhle základu stavíme cokoliv nového. Tohle je
přesně ten "nejdřív se nauč kompilovat beze změny kódu" princip z úvodu
projektu, aplikovaný teď i na první krok úpravy kódu samotného.

## Vendorování zdroje do tohoto repozitáře

Zdroj OpenTTD 15.3 (commit `14ec60f248547d4d062a1160f0fc26d742319888`)
zkopírován do `openttd/` v tomto repu (bez `.git` historie OpenTTD — naše
změny sleduje git tohoto repozitáře). `build-windows.yml` upraven, aby
defaultně stavěl `openttd/` z tohoto repa; možnost stavět z libovolného
externího zdroje (`source_repo`/`source_ref`) zůstává zachovaná pro
srovnávací/ověřovací buildy čistého vanilla kódu.

## Postup implementace (průběžně doplňovat)

- ✅ **`TryConsistSplice`** vytažen z `CmdMoveRailVehicle` (čistý refaktoring,
  ověřeno buildem i ručním testem v depu — beze změny chování).
- ✅ **`CMD_COUPLE_TRAINS` / `GetTrainCouplePartner`** — první funkční verze
  spojování na trati. Zatím jen nejjednodušší geometrie: `v` dojíždí zezadu
  a je zablokovaný stojícím vlakem před sebou, oba stejným směrem (typický
  případ pro odtah — dojíždějící mašinka). Adjacency přes
  `FollowTrainReservation` (existující enginová funkce, ne pixely — viz Bug
  C). Obecný případ "čelně proti sobě" zatím záměrně chybí, bude
  samostatný krok.
- ✅ **Tlačítko "Couple"** v okně vozidla (`WID_VV_COUPLE`) — **zavrženo po
  testu**: uživatel ho neviděl/nemohl vyvolat, testování mimo běžné herní
  situace (bez semaforů apod.) navíc nemá vypovídací hodnotu. Kód
  ponechán (neškodí, může se hodit), ale další testování se přesouvá na
  skutečné napojení do `Order` systému níže.
- ✅ **Decouple jako vlastnost normálního "jet do stanice" příkazu**
  (ne nový `OrderType`!) — bezpečnější návrh než plánovaný `OT_DECOUPLE`:
  nová dedikovaná pole `Order::decouple_count` (vlastní úložiště, ne
  sdílené bity — viz Bug D), `MOF_DECOUPLE_COUNT` v `CmdModifyOrder`,
  uložení do save (CH_TABLE chunk, staré savy dostanou default 0 =
  vypnuto), a spouštěcí háček přímo v `Vehicle::LeaveStation()` (jediné
  dobře definované místo "vlak právě odjíždí ze stanice", žádná nová
  detekce příjezdu od nuly). GUI: nový řádek v okně příkazů, viditelný jen
  pro vlak + "jet do stanice" příkaz, tlačítko otevře dialog na zadání
  čísla (přes `ShowQueryString`, stejný mechanismus jako u podmínkových
  příkazů) — psaní čísla funguje (jen herní konzole je ve Winlatoru
  rozbitá, běžná textová pole ne).
- ✅ **Postřeh o jednosměrných semaforech u odtahu zapracován do designu**
  (viz sekce výše) — odtah se nikdy nevrací stejnou cestou, jede dál
  dopředu s "opraveným" vlakem připojeným vzadu.
- ⏭️ **Další v pořadí:** ruční otestovat decouple (viz recept níže), pak
  `CMD_DECOUPLE_TRAIN` na trati (mimo stanice, pro obecné rozpojení) a
  obecný případ orientace couplingu (spojení proti sobě, s reverzem jedné
  strany).
- ⏭️ Až couple/decouple budou stabilní: samotná tow logika (breakdown
  hook, "Service" order pro odtahovou mašinku v depu, výběr nejbližší
  odtahovky).

### Jak otestovat decouple (nové, přes Order okno)

1. Vlak s alespoň 2 vozidly, v jeho příkazech vyber řádek "jet do
   stanice X" (klikni na ten řádek v seznamu příkazů).
2. Dole v okně příkazů by se měl objevit nový řádek s tlačítkem
   "Decouple: 0".
3. Klikni na něj → napiš číslo (kolik vozidel ponechat vepředu) → potvrď.
4. Vlak nech dojet a odjet z té stanice normálně (naložit/vyložit jak má).
5. Očekávaný výsledek: při odjezdu ze stanice se od zbytku vlaku oddělí
   vozidla za zadaným počtem a zůstanou stát na místě jako samostatná
   souprava (bez příkazů — ty si nastavíš ručně); přední část pokračuje
   dál podle svých příkazů.

### Jak otestovat `CMD_COUPLE_TRAINS`

1. V depu (vanilla, už funguje) rozdělit/postavit dvě samostatné soupravy.
2. Obě vyjet z depa na stejnou trať.
3. Druhou (zadní) poslat příkazem tak, aby dojela a zastavila se těsně za
   první (normální herní chování, žádný speciální krok — hra sama
   zablokuje druhý vlak před prvním).
4. Kliknout na druhý (zadní) vlak → v okně vozidla by mělo být aktivní
   tlačítko "Couple" → kliknout.
5. Očekávaný výsledek: obě soupravy se spojí do jedné, přesně jako
   výsledek přesunu v depu (statistiky, příkazy, unit number se převezmou
   podle stejné logiky jako `CmdMoveRailVehicle`).

## Otevřené body k pauze (2026-08-14, konec usage limitu)

- ✅ **Bezpečnostní oprava:** `difficulty.line_reverse_mode` (otáčení ve
  stanicích) a `pf.reverse_at_signals` (otáčení na semaforech) natvrdo
  zamčeny přes `SettingDesc::IsEditable()` — vlak po decouplu mohl
  narazit do vagonků, co za sebou nechal. Nastavení zůstává v menu
  viditelné (jen zašedlé), s vysvětlivkou v nápovědě dole.
- ⏭️ **Velký požadavek na GUI reskin (zatím nerozpracováno):** uživatel
  chce Order okno chovat se stylově jako v Palo123YPS (klik na stanici →
  nový řádek s decouple/couple parametry — blízké tomu, co dnes máme,
  ale styl/vzhled má sedět na starý patch), přebrat grafiku horní lišty
  ikon (výstavba, finance, atd.) z Palo123YPS, analyzovat rozdíl mezi
  semaforovými systémy 15.3 a Palo123YPS a případně přebrat semafory +
  jejich GUI z Palo123YPS. Z 15.3 chce zachovat jen GUI nastavení (pod
  ozubeným kolečkem). Úvodní GUI před generováním mapy zatím neřešit.
  **Tohle je velký, samostatný kus práce** — potřebuje se probrat a
  naplánovat zvlášť, ne narychlo přimíchat do bezpečnostní opravy.

## Poznatek: jak poznat, jestli hraješ NÁŠ build (2026-08-14)

Uživatel při testu omylem spouštěl starou referenční kompilaci
Palo123YPS (větev `Decouple`, commit `g251e5384`, datum 2018-08-14), ne
náš `forclaude` build — a chvíli si myslel, že testuje naše změny.
Poznat se to dá spolehlivě podle verze v hlavním menu:

- Náš vendorovaný zdroj v `openttd/` v tomhle repu **nemá vlastní
  `.git`** (schválně, viz `VENDORED_SOURCE.md`). `FindVersion.cmake`
  proto nemůže spustit `git describe` a spadne do fallbacku
  `REV_VERSION = "norev0000"` (viz
  `openttd/cmake/scripts/FindVersion.cmake:118`). Náš build tedy VŽDY
  ukazuje `norev0000` v hlavním menu, nikdy datum/větev/hash.
- Formát `<datum>-<větev>-g<hash>` (přesně to, co bylo na screenshotu)
  vzniká jen když se kompiluje ze skutečného git repozitáře s historií
  — to je Palo123YPS build, ne náš.

**Důsledek:** hlášené "zamčené čudlíky ve špatné poloze" (otáčení
povoleno) a bohaté orders GUI ze screenshotu patří k tomu starému
referenčnímu buildu, ne k našemu kódu. V našem zdroji jsem znovu
zkontroloval hodnoty:
- `difficulty.line_reverse_mode` ("Disallow train reversing in
  stations"): `def = true` (zákaz zapnutý) — bezpečný stav, zamčeno.
- `pf.reverse_at_signals` ("Automatic reversing at signals"):
  `def = false` (vypnuto) — bezpečný stav, zamčeno.
Obě natvrdo needitovatelné přes `SettingDesc::IsEditable()`. Pokud by
se stejný problém objevil i v exe s verzí `norev0000`, jde o skutečný
bug a je potřeba to nahlásit znovu.

**Doporučený postup příště:** před testem zkontrolovat verzi v hlavním
menu (musí být `norev0000`), teprve pak testovat a hlásit chování.

## Otevřený bod: odtah a už obsazená jednosměrná trať

Uživatel upozornil na scénář, který návrh exclusive-reservation řeší
jen částečně: na jednosměrné (semaforama chráněné) trati mezi depem
odtahovky a místem poruchy mohou stát/jet JINÉ vlaky ještě předtím, než
k poruše vůbec dojde. Rezervace celé zpáteční cesty pro odtahovku
nezaručuje, že cesta TAM bude volná — odtahovka může uvíznout za jiným
vlakem, který jí zablokuje jednosměrný semafor směrem k poruše.
Návrh tedy musí počítat s tím, že odtahovka prostě čeká (stojí, dokud
se cesta neuvolní běžným provozem), stejně jako by čekal kterýkoli jiný
vlak před obsazeným blokem — žádné obcházení kolizní ochrany. Zůstává
otevřené pro fázi, kdy se bude tow reálně implementovat.

## Coupling: detekce partnera funguje v obou směrech (run #17)

Test ukázal, že "wait to couple" nefungovalo, když druhý vlak přijel
zezadu (vzhledem k tomu, kterým směrem byla čekající mašinka otočená) —
`GetTrainCouplePartner()` hledal partnera jen dopředu, přes
`FollowTrainReservation()`. Uživatel musel čekající mašinku ručně
otáčet, aby "dozadu" ukazovala tam, odkud měl přijet druhý vlak — což
je přesně ta ruční obsluha, které se chceme vyhnout ("pro mě je
složité ovládat vlak ručně a snažit se vyvolat couple").

Řešení: `FollowTrainReservation()` (pbs.cpp/pbs.h) dostal nový
parametr `from_rear` — když je `true`, sleduje rezervaci od PŘEDU k
ZADU konzistu (tedy hledá dozadu) místo od zadu dopředu.
`GetTrainCouplePartner()` teď zkusí napřed dopředu, a pokud nic
nenajde, zkusí dozadu; vrací i informaci, kterým směrem partnera našel
(`partner_is_behind`), aby `CmdCoupleTrains()` mohl správně určit, které
straně se má připojit druhá souprava (vedoucí vlak si nechá svůj přední
konec, druhý se přiřadí vzadu). Žádné ruční otáčení už není potřeba —
stačí přijet a zastavit vedle čekající mašinky odkudkoliv.

**Odbočka k "musí couvat":** uživatel zkoušel ručně přijíždějící
mašinku otočit na konci koleje, aby jela "pozadu" k čekající — ale ve
vanilla 15.3 (a v naší vendorované verzi) žádné skutečné couvání
neexistuje, otočení je vždy okamžitý "magic flip" (vlak se přeorientuje
a dál jede vždy předkem, ve směru jízdy). To NENÍ bug, je to
standardní chování enginu už od nepaměti. Skutečné couvání (lokomotiva
zůstává vzadu, konzist fyzicky jede pozpátku) přidal až upstream
OpenTTD nedávno ("push-pull" / "backwards driving", červen 2026,
[oznámení](https://www.openttd.org/news/2026/06/25/backwards-driving),
[PR #15379](https://github.com/OpenTTD/OpenTTD/pull/15379),
[PR #15391](https://github.com/OpenTTD/OpenTTD/pull/15391)) — vyžaduje
obousměrnou lokomotivu nebo NewGRF-označený "cab" vagon a zapíná se
herním nastavením. Portovat tohle do naší 15.3 by byl srovnatelně
velký projekt jako odtah — zůstává jako možné budoucí rozšíření, ale
díky opravě výše (obousměrná detekce partnera) už není pro samotné
spojování potřeba: nezáleží, kterým směrem je která souprava otočená,
stačí fyzická blízkost.

## "Go to couple" příkaz (run #18): jak to řešil Paolo123YPS a proč to nepotřebuje beta16

Uživatel se ptal, jestli nejde použít couvání tak, jak ho měl
Paolo123YPS, místo portování celého "backwards driving" z beta16.
Stáhl jsem si jejich `Decouple` větev a podíval se na `OT_GOTO_COUPLE`
přímo v kódu (`order_cmd.cpp`, `train_cmd.cpp`,
`pathfinder/yapf/yapf_rail.cpp`). Zjištění: **žádné skutečné couvání
tam není.** Je to jen:

1. Vlastní typ příkazu `OT_GOTO_COUPLE` s vlastním YAPF pathfinderem
   (`CYapfCouple`/`DoTrainCouplePathfind`), který hledá cestu k
   PARTNEROVI (jinému vlaku) jako cíli, ne k pevné dlaždici.
2. Výjimka `may_reverse = ... || v->current_order.IsType(OT_GOTO_COUPLE)`
   — dovolí témuž, už ve vanille odjakživa existujícímu mechanismu
   volby couvání (magic-flip, ne skutečná jízda pozadu), aby se použil
   i pro tenhle příkaz, i kdyby jinak nebyl "nejkratší cestou".
3. Povinné otočení na konci koleje (`TrainApproachingLineEnd` →
   `ReverseTrainDirection`) funguje úplně stejně jako vždy — to není
   vázané na žádné nastavení, děje se to vždycky, i u nás. Přesně tohle
   se uživateli spustilo při ručním testu — jenže vlak neměl žádný cíl
   směřující k partnerovi, tak po otočení pokračoval jinam.

Jinými slovy: stačí dát vlaku SKUTEČNÝ cíl (stanici/místo, kde partner
stojí) a normální, ve vanille odjakživa existující otáčení (povinné na
konci koleje, volitelné jako zkratka jinde) ho tam dostane samo,
včetně otočení, když je potřeba. Žádná nová vrstva pohybu není
potřeba — to je přesně to, co dělá beta16 zbytečně velkým pro naši
potřebu.

**Implementace v1 (hotovo):** nový příkaz "Go to couple"
(`WID_O_GOTO_COUPLE`, pole `Order::go_to_couple`, `MOF_GOTO_COUPLE`) —
zatím funguje stejně jako "Wait to couple" (při příjezdu/zastavení
čeká, dokud se nenajde partner přes `GetTrainCouplePartner`, pak
spojí), ale navíc:
- má vlastní tlačítko/popisek v GUI, aby to vypadalo jako u
  Paolo123YPS (samostatný řádek v seznamu příkazů),
- prolamuje zámek `difficulty.line_reverse_mode` výhradně pro tenhle
  příkaz (`CheckReverseTrain()` v `train_cmd.cpp`) — bezpečnostní zámek
  chrání proti NEÚMYSLNÉMU otočení do vagonků po decouplu; úmyslné
  otočení kvůli příkazu, jehož smysl je právě dojet k partnerovi, není
  ten případ.

**Co ještě chybí (budoucí rozšíření, ne blokující):** vlastní
pathfinder hledající NEJBLIŽŠÍHO vhodného partnera jako cíl (jejich
`CYapfCouple`/`CYapfDestinationTrainRailT`) — zatím se "Go to couple"
musí zadat na konkrétní stanici, kde partner už stojí, ne "najeď k
nejbližšímu vhodnému vlaku kdekoliv". Tohle je přesně ta část, co se
bude hodit pro odtah (najít nejbližší porouchaný vlak) — nechávám jako
navazující krok, ne součást v1.

## Skutečná příčina "u vagonku bouchla, nespojila se" (test po run #18)

Test ukázal, že v1 výše nestačila: mašinka odmítala vyjet z depa (musel
se to protlačit přes semafor ručně) a po vynuceném vyjetí u vagonků
nabourala, aniž by se pokusila spojit. Příčina: **žádný pathfinder v
téhle hře neumí zacílit na dlaždici hned vedle obsazené dlaždice** —
každé normální hledání cesty se tomu záměrně vyhýbá (přesně to chrání
vlaky před srážkou). Když má "go to couple" příkaz jako cíl normální
stanici, kde už partner stojí, `ChooseTrainTrack`/`ExtendTrainReservation`
narazí na obsazenou kolej, nenajde žádný "bezpečný" bod k zastavení, a
vlak označí jako zaseklý (`MarkTrainAsStuck`) — přesně to viděl
uživatel. Vynucené vyjetí přes semafor pak jelo bez jakékoliv rezervace
= bez ochrany = srážka. To není bug v "couvání", je to mezera v tom, co
vůbec pathfinder umí považovat za platný cíl.

**Implementace (hotovo, viz commit):** postavil jsem vlastní YAPF
destination typ `CYapfDestinationCoupleRailT` (`yapf_destrail.hpp`) —
kopie existujícího `CYapfDestinationAnySafeTileRailT`, ale
`PfDetectDestination` místo "je to bezpečné a volné místo" kontroluje
"je hned vedle mě zaparkovaný platný partner" — přes novou funkci
`IsAdjacentToCouplePartner()` (`train_cmd.cpp`, zrcadlí přesně
validační podmínky z `GetTrainCouplePartner()`, aby cesta, kterou
pathfinder najde, šla opravdu dokončit spojením). "Follow" část (jak
se prochází koleje)
jsem nemusel psát znovu — `CYapfFollowAnySafeTileRailT` je už dost
obecná, jen jsem ji spároval s novým destination typem
(`CYapfCoupleRail`/`CYapfCoupleRailNo90`, přesně podle vzoru
`CYapfAnySafeTileRail`). Nová vstupní funkce
`YapfTrainFindCouplePosition()` (`yapf_rail.cpp`/`yapf.h`) se volá z
`ChooseTrainTrack()` jako záložní krok TĚSNĚ předtím, než by se vlak
normálně označil za zaseklý — ale jen pro vlaky s `go_to_couple`
příkazem, takže normální vlaky se chovají naprosto stejně jako dřív.

Tohle by mělo vyřešit i to, proč vlak nechtěl vyjet z depa (stejná
mezera, jen dřív v cestě) — depo taky potřebuje projít
`ChooseTrainTrack`, aby vlak vůbec vyjel.

Vedlejší drobnost ze stejného testu: přidal jsem i text "(go to
couple)" za řádek příkazu v seznamu, ať to vypadá jako u Paolo123YPS
(vlastní řádek), i když je to pořád technicky flag na normálním
příkazu "Jet do stanice", ne nový typ příkazu.

## Proč pořád couvá jen "z donucení" (test po run #20): chybějící druhá polovina

I s pathfinderem výše zůstala mašinka stát na semaforu ("Čekám na
volnou cestu") a bez couvnutí — teprve vynucené vyjetí skončilo
srážkou. Důvod: v enginu jsou DVĚ oddělené věci, co s otáčením souvisí,
a opravil jsem zatím jen jednu.

1. **Kde se REZERVUJE cesta** (`ChooseTrainTrack`) — to už umí najít
   partnera jako cíl (oprava výše).
2. **Kde se ROZHODUJE, jestli se má vlak vůbec otočit** — samostatná
   funkce `YapfTrainCheckReverse()`, volaná každý tik z
   `TrainLocoHandler`, PŘED tím, než se `ChooseTrainTrack` vůbec
   zavolá. Ta porovnává "cena cesty dopředu" vs. "cena cesty po
   otočení" — ale porovnávala to vždycky vůči BĚŽNÉMU cíli příkazu
   (stanici), ne vůči partnerovi. Takže i když jsem odemkl zámek
   `line_reverse_mode` pro "go to couple", vlak se otočit nerozhodl,
   protože jeho vlastní srovnání nákladů netušilo, že "otočit se" by
   ho přiblížilo k partnerovi — porovnávalo to se stejnou "nedosažitelnou"
   stanicí jako běžná rezervace.

**Oprava:** nová dvojice `CYapfCoupleReverseRail`/`CYapfCoupleReverseRailNo90`
(stejný `CYapfDestinationCoupleRailT` cíl jako předtím, ale spárovaný s
`CYapfFollowRailT` — tam žije `CheckReverseTrain`/`stCheckReverseTrain`,
na "any safe tile" follow typu není). Nová funkce
`YapfTrainCheckReverseForCouple()` (zrcadlí `YapfTrainCheckReverse()`
1:1, jen s jiným párem struktur) a `CheckReverseTrain()` v
`train_cmd.cpp` ji teď volá místo normální verze, když má vlak "go to
couple" příkaz. Sdílenou část výpočtu (tunely/mosty, penalizace) jsem
vytáhl do `GetReverseCheckOrigins()`, ať se nekopíruje.

Tohle by měla být ta chybějící druhá polovina — bez ní pathfinder sice
"věděl", že cesta k partnerovi existuje po otočení, ale nic vlak
k tomu otočení nedonutilo.
