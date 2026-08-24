# Strom témat

Rejstřík projektu. Ne deník — **závěry**, které platí dál, roztříděné podle
toho, čeho se týkají.

- Jednotlivá kola testů → `TEST_LOG.md`
- Návrh funkce, dlouhé úvahy, historie rozhodnutí → `FEATURE_DESIGN_COUPLING_TOW.md`
- Jak se staví Windows build → `BUILD_NOTES.md`
- Odkud je zdroj hry → `openttd/VENDORED_SOURCE.md`

---

# 0. START — tohle čti první

## 0.1 Proč tenhle soubor je

Mezi sezeními si nepamatuju nic. Přežije jen to, co je v repozitáři. Když
si závěr nezapíšu, vyvodím ho podruhé znovu — nebo hůř, vyvodím ho jinak a
rozbiju tím věc, která fungovala. Přesně to se stalo s plánkem nádraží
(ztracen dvakrát) a s tím, čím **není** způsobené zaseknuté pravé tlačítko
(vysvětlováno třikrát).

**Pravidlo: než se zeptám podruhé, kouknu sem.**

## 0.2 Jak spolu pracujeme

1. **Nejdřív se mluví.** Popíšu, co jsem našel a co navrhuju. Nesahám do
   kódu, dokud si nejsme jistí, že si rozumíme.
2. **Pushuje se, až když padne „pushni to".** Do té doby se povídá.
   Výjimka: záznamy do `TEST_LOG.md` a věci, které si o to samy řeknou.
3. **Build si pouští on sám.** Nespouštím ho z vlastní vůle, ani „aby to
   bylo hotové".
4. **Nezpracovávám úkoly, dokud nemám všechny informace.** Když přijde
   report po částech, zapisuju a čekám na konec. Předělávat okno třikrát
   za sebou, protože přišla další věta, je vyhozený čas.
5. **Chyba se hledá ve hře, v kódu.** Ne v tom, kdo hraje, ne v telefonu,
   ne v nastavení. Když se mi nabízí vysvětlení „ono je to tím, jak to
   spouštíš", je to skoro jistě špatná odpověď.
6. **Nepapírovat příznaky.** Hledá se příčina. Když je oprava jen
   ošetření následku, řeknu to nahlas.

## 0.3 Zkušební nádraží — plánek

Testuje se pořád ta samá situace. Bez tohohle se úkoly o spojování číst
nedají.

**Čtyři nástupiště, číslovaná shora dolů 1 až 4.** Na každém stojí řada
vagonků. Mašinka, která je tam nechala, stála:

- na **1 a 2** u **pravého** konce řady,
- na **3 a 4** u **levého** konce řady.

**Do stanice se vjíždí vždycky zprava**, tedy pohybem doleva. Liší se jen
to, jestli je mašinka v tom pohybu napřed nebo vzadu:

- na **1 a 2** mašinka **nacouvá** — jela přes směrování, které ji otočilo,
  takže je vlečeným koncem a skončí vpravo, vagonky vlevo od ní;
- na **3 a 4** jede **popředu** — na směrování nebyla, takže je vedoucím
  koncem a skončí vlevo, vagonky vpravo od ní.

Odpojená mašinka má na všech čtyřech nástupištích **nos pryč od vagonků**.
Liší se jen konec, na kterém stojí.

## 0.4 Pravidlo zrcadla

Vlak je popsaný **dvěma nezávislými veličinami** a většina chyb v tomhle
projektu vznikla tím, že se jedna vydávala za druhou:

- **hlava / konec** — pořadí seznamu vozů. Couváním se nemění.
- **vedoucí / vlečený konec** — který konec jede napřed. To je směr jízdy.
  Spojováním se nemění.
- **nos** — kam míří jeden konkrétní vůz.

| nástupiště | hlava řady | vedoucí konec |
|---|---|---|
| 1 a 2 | vpravo (u mašinky) | vlevo |
| 3 a 4 | vlevo (u mašinky) | vlevo |

**Na 1 a 2 míří hlava a vedoucí konec na opačné konce. Na 3 a 4 na ten
samý.** To je to zrcadlo.

**Důsledek — nikdy nejmenovat konec.** Jakékoliv pravidlo tvaru „vezmi
hlavu" nebo „podívej se na první vůz" je na jedné dvojici nástupišť
správně a na druhé přesně naopak. Proto oprava jedné dvojice donekonečna
rozbíjela druhou. Buď se projde celá souprava, nebo se přečtou obě
veličiny a jedna se odvodí z druhé.

---

# 1. Vlak: směr, couvání, otáčení

## 1.1 Dvě různé věci, které se pletou

- **otočení hlavy (flip)** — přehodí se pořadí vozů, mašinka se objeví na
  druhém konci. Nastavení `difficulty.train_flip_reverse_allowed`.
- **vedoucí konec** — který konec jede napřed. Příznak
  `VehicleFlag::DrivingBackwards`.

## 1.2 Co je rozhodnuto

- Flip je **zamčený na „Nikde"**. Vlaky se na trati nikdy neotáčejí.
- **Kabina vede.** Vlak vyjíždí ze stanice mašinkou napřed vždycky. Není to
  preference podle ceny trasy — to je nepředvídatelné a hráč si podle toho
  nemůže nic naplánovat. Napsané je to jako cena (v hledání cesty se všechno
  počítá v cenách), ale tak vysoká, že ji přebije jedině to, že dopředu
  nevede **žádná** cesta.
- Ta jediná výjimka je **hlavové nádraží**. Tam vlak vycouvá a je to
  správně — hráč ten slepý konec postavil schválně.
- **Snížená rychlost 32 km/h**, když vedoucí konec nemá řídicí kabinu,
  **zůstává**. Je to vanilkové a je to správně: nikdo nekouká dopředu.
- **Depo je zatáčka.** Vjezd ani výjezd nemění, který konec vede, takže
  vlak vjede mašinkou napřed a vyjede mašinkou napřed, jen míří na druhou
  stranu. Nic se přitom nepřehazuje.

## 1.3 Kde to v kódu je

- `YapfTrainCheckReverse()` — `pathfinder/yapf/yapf_rail.cpp`
- `CanLeadTrain()` — `ground_vehicle.hpp`
- ořez rychlosti — `Train::GetCurrentMaxSpeed()` v `train_cmd.cpp`
- `CheckReverseTrain()`, `ReverseTrainDirection()` — `train_cmd.cpp`

---

# 2. Spojování (couple)

## 2.1 Dva druhy „jet se spojit"

**Mašinka k vagonkům.** Vagonky nejsou vlak a nemají vlastní příkazy;
přebírají příkaz od mašinky, která je tam nechala. Když jsou plné, příkaz
jim poskočí na „čekat na spojení" a od té chvíle čekají na odvoz.

**Mašinka k mašince.** Oba jsou plnohodnotné vlaky a **oba si své příkazy
nechávají**. Spojení je dočasné; po rozpojení každý pokračuje tam, kde
přestal. Na tomhle stojí odtah. Pohlcená mašinka si drží příkazy, číslo
i jméno — dřív se zahazovaly, a zahozené číslo se uvolní dalšímu vlaku,
takže se ta mašinka nemohla vrátit ani jako ona sama. Zároveň to řeší
synchronizaci ve hře více hráčů: nerozdávají se žádná nová id.

Vede vždycky ta s „jet se spojit". Spojený vlak má výkon obou mašinek.

## 2.2 Pravidla, která platí

- **Nejdřív se najde partner podle filtru, pak se teprve rezervuje trať.**
  Obráceně se držela trať proti všem ostatním, dokud nebylo rozhodnuto, a
  pak vlak jel k tomu, co si zarezervoval, místo k tomu, co si vybral.
- **Filtr počtu vozů počítá i mašinku** — mašinka a tři vagony je N=4.
- **„Plné / prázdné" znamená, které stojící vagonky si vzít.** Ne „počkej,
  až budou plné".
- **Bere se celá řada, ne část.**
- **„Čeká na spojení" platí, jen když vlak sám nikam nemůže** — stojí ve
  stanici, kam byl poslán, nebo je porouchaný nebo havarovaný. Příznak
  sedí na příkazu dávno předtím, než vlak dojede; bez téhle podmínky se
  mašinky vyjíždějící z depa slepily do jednoho vlaku.
- **Výjimka ze srážky a samotné spojení se musí ptát na to samé.** Když se
  ty dvě otázky rozejdou, vlak nebouchne a nespojí se — projede skrz.
  Kdo do spojení nepatří, bourá normálně, do porouchané mašinky i do
  vagonků na peronu.
- **Uvolnit trať před spojením, ne po něm.** Uvolňuje se dopředu od
  vedoucího konce; po spojení už vede druhý konec, takže cesta, kterou
  mašinka přijela, zůstane za vlakem a nikdy se neuvolní.

## 2.3 Vedoucí konec po spojení

Má-li kabinu jen jeden konec, vede ten. Mají-li ji oba — a to je přesně to,
co z vlaku udělá připojení mašinky z druhé strany, tedy push-pull — jede se
tudy, kudy se přijelo, protože o té cestě se ví, že je průjezdná.

---

# 3. Rozpojování (decouple)

- Číslo v příkazu je **kolik vozů si vlak nechá**, ne kolik jich nechá stát.
- Odpojená řada dostane **dva skutečné příkazy**: co jí mašinka nechala za
  práci na téhle stanici, a za tím „čekat na spojení". Hráč mezi nimi
  přeskočí běžným tlačítkem Přeskočit v běžném okně příkazů. Poskočí to i
  samo, jakmile je nakládání hotové.
- **„Čekat na spojení" a „naložit plné" současně nejde.** Řada, která má
  naložit do plna, má tady práci a partnerem se stává až potom.
- Odpojená část, která má vepředu mašinku, **je zase vlak**: příkaz jí
  poskočí a když další příkaz jmenuje stanici, ve které stojí, odbaví ho na
  místě, místo aby pro něj objížděla nádraží.

---

# 4. Odtah

- Odtahovka je **stav mašinky, ne příkaz**. Nastavuje se čudlíkem v okně,
  když stojí v depu. Vlak s příkazy jí být nemůže — zadání příkazů ji
  postaví mimo službu.
- **Porouchaná i havarovaná mašinka má na svém příkazu „čekat na spojení".**
  Odtahovka pak použije úplně obyčejné hledání partnera a obyčejné spojení.
  Žádná zvláštní větev.
- **V okně dál svítí porucha nebo havárie.** Stav vozidla se ptá nejdřív na
  havárii, pak na poruchu, a teprve potom na příkazy.
- Porouchaná mašinka si veze **své vlastní příkazy** celou cestu.
- **Odtahovku, která už někoho táhne, nejde odvolat** — musí ho dovézt.
- **Překážkou není to, pro co jsem přijel.** Rezervace trati se ptá, jestli
  v cestě nestojí jiný vlak, a když ano, označí vlak za zaseknutý a čeká.
  Porouchaná mašinka je ale vlak, který nikdy neodjede.
- Pojistka: **půl roku herního času od poruchy** (ne od vyslání). Pak se
  porucha spraví sama a trosky začnou mizet po vanilla způsobu, čímž se
  trať uvolní.
- Vrak se z principu nedá pohnout — vanilla z havárie dělá konec vlaku, ne
  jeho stav. Rozbor v `FEATURE_DESIGN_COUPLING_TOW.md`, sekce „Odtahovka —
  těžká místa", bod A.

---

# 5. Myš, kurzor, stavba

## 5.1 Zaseknutý čudlík posunu mapy

**Není to zařízením, na kterém se hraje.** Řečeno třikrát. Chová se to
stejně na počítači a je to tak roky. Každé vysvětlení, které začíná u
dotykového displeje, obalu nebo ovladače, je slepá ulice.

Co bylo zkoušeno a **nezabralo**:

- „levý je pán" — stisk levého ukončí tažení pravým. Ošetření následku.
- číst stav tlačítek každý snímek od systému. V jedné podobě to i
  zhoršilo: zahazoval se úchop okna, na kterém stálo druhé tlačítko.

**Skutečná příčina:** ukončení tažení mapy stálo ve frontě za jinými
režimy. `HandleViewportScroll()` je poslední z pěti obsluh v `MouseLoop()`
a každá předchozí smí událost zabrat a vrátit se dřív. Všechny čtyři
předchozí jsou režimy **levého** tlačítka — tažení okna, tažení v seznamu,
stavba kolejí. Dokud kterýkoliv běží, nikdo se nezeptá, jestli má tažení
mapy pokračovat. Proto to zdánlivě vyléčí klepnutí levým: ukončí ten
druhý režim.

**Poučení, které platí i jinde: ukončení režimu nesmí být schované za
předčasným návratem jiného režimu.**

## 5.2 Ukazatel při stavbě

Bílý obrys ukazuje, kam se postaví kolej. Při stisku se **zmrazí to, co
bylo nakreslené** — hra si při druhém čtení spočítala jinou dlaždici než
při vykreslení, a stavělo se jinam, než hráč viděl.

## 5.3 Režimy posunu mapy

Nastavení „Reakce pohledů na scrollování". První varianta drží kurzor na
místě a hra ho po každém pohybu násilně vrací. Nápověda toho nastavení
sama píše, že zamykání ukazatele nemusí všude fungovat.

---

# 6. Okna a rozhraní

- **Okno se nesmí přeskládat, když se právě doručuje klik.** `ReInit()`
  volaný z `OnClick()` posune rozložení pod prstem a klik dopadne na jiný
  čudlík. Odkládá se to do `OnMouseLoop()`. Stalo se dvakrát — v depu a
  pak znovu v okně příkazů.
- **Čudlík má držet místo.** Objevující se a mizející čudlík mění velikost
  okna, když vlak vjede do depa nebo z něj vyjede, a sloupec se hráči
  přeskládá pod prstem. Radši zašedlý než pryč.
- **Popisek bez místa ve stromu je půl práce.** Nastavení potřebuje `str`
  a `strhelp` **a** zařazení do stromu v `settingentry_gui.cpp`. Bez
  popisku se nezobrazí vůbec, bez stromu se najde jen přes vyhledávání.
- **Řada čudlíků s `NWidContainerFlag::EqualSize`** si šířku dělí sama,
  takže přidání dalšího ostatní zúží a nic z řady nevypadne.
- Okno bezhlavé řady vagonků má vlastní titulek a odemčené příkazy;
  podrobnosti a přejmenování zamčené.

---

# 7. Nastavení

## 7.1 Co hra zamyká

- `difficulty.train_flip_reverse_allowed` → vždy „Nikde"
- `pf.reverse_at_signals` → vždy vypnuto

Přepisují se při každém načtení, takže „zamčeno" platí i pro starý soubor
nastavení.

## 7.2 Postupné nakládání

`order.improved_load`, v okně nastavení pod **Vozidla → Příkazy**.

Zapnuto: ve stanici se nakládá po jednom. Kdo čeká na plné naložení,
zamluví si zboží pro sebe; ostatní nástupiště čekají, až na ně přijde
řada. Kdo přijel dřív, naloží dřív.

Vypnuto: nakládají všichni najednou a dělí se.

**Jednotlivá stanice se z toho vyjímá čudlíkem „Postupně" ve svém okně.**
U stanice se ukládá **výjimka**, ne stav — díky tomu znamená totéž, ať je
celohra na kterékoliv straně, a starý sav se načte jako „žádné výjimky".
Když je celohra vypnutá, čudlík je zašedlý; není z čeho vyjímat.

---

# 8. Co přinesla beta 16 (a proč v ní děláme)

- `VehicleFlag::DrivingBackwards` + `GetMovingFront()` / `GetMovingBack()` /
  `GetMovingDirection()` — **skutečné couvání** místo magického přehození
  soupravy. Kvůli tomuhle se šlo z 15.3 na 16.
- `ExtraEngineFlag::HasCab` — GRF příznak „tohle vozidlo má řídicí kabinu".
  Umí udělat vedoucí i z nemotorového vozu, tedy **řídicí vůz**. To je ten
  nový příznak; `RailIsMU` (jednotka DMU/EMU) je vedle něj starý a hrubší.
- `CanLeadTrain()` = má kabinu **nebo** je to mašinka **nebo** zadní půlka
  dvojité mašinky. Tohle je ta správná otázka.
- Ořez rychlosti na 32 km/h, když vedoucí konec neumí vést.
- Nastavení `train_flip_reverse_allowed`: Kdekoliv / Jen na konci tratě /
  Nikde.
- NewGRF kompatibilita s CZTR grafikami, které cílí na stejné schéma
  příznaků.

---

# 9. Předloha Palo123

`Palo123/OpenTTD-YPS`, větev `Decouple-wip`. Klonuje se ručně (proxy to
pustí, `add_repo` odmítá cizího vlastníka):

```
git clone --depth 1 --branch Decouple-wip https://github.com/Palo123/OpenTTD-YPS.git
```

Je to referenční **vzhled pro hráče**, ne referenční řešení —
implementaci mají jinou. Rozpis jejich okna příkazů, včetně toho, co který
čudlík dělá a co z toho u nás chybí, je ve `FEATURE_DESIGN_COUPLING_TOW.md`,
sekce „Předloha Palo123".

Podstatné: **nikdy neotáčejí celý vlak.** To je to samé rozhodnutí, ke
kterému jsme došli sami.

---

# 10. Build a repozitář

- Zdroj hry je **vendorovaný** v `openttd/` — kopie 16.0-beta2 bez vlastní
  `.git`. Práce na 15.3 zůstala na větvi `backup-15.3-decouple`.
- Windows build je GitHub Actions workflow „Build OpenTTD (Windows)",
  spouští se ručně přes **Run workflow**. Trvá zhruba 15–25 minut.
- **Číslo v liště je číslo toho běhu, který binárku vyrobil.** Bere se
  z `github.run_number` a zapisuje se do `.ottdrev`. Stejné číslo je
  v názvu artefaktu.
- **„Run workflow" staví aktuální špičku větve. „Re-run" postaví starý
  commit a nechá staré číslo.**
- Čísla běhů se nikdy nepoužijí znovu. Smazaný běh = zahozený log a
  binárka; na kódu to nemění nic, jen si už nedohledám, z čeho byl.
- Ke spuštění hry se stahuje **celý bundle** (`openttd-windows-x64`), ne
  samotné `openttd-exe`.
- Ze souborů repozitáře byl kdy smazaný jen `TEST_LOG.md` (commit
  `af780c3`) a je zpátky, obsahově shodný s tím, co bylo smazáno.

---

# 11. Pasti v enginu

- **Fond objektů se musí ptát předem.** `OrderList::CanAllocateItem()` není
  rada, je to povolení, které si sama alokace kontroluje
  (`assert(this->checked != 0)`). Kdo si vezme seznam příkazů bez zeptání,
  položí hru. Platí pro každý objekt z fondu.
- **Nesmí se splétat soupravy uprostřed procházení.** Spojení se dělá
  v `TrainLocoHandler()`, kdy po vozech nikdo neběhá. Kontrola srážky
  proto vlak jen zastaví a spojení odloží na příští tik.
- **Pole příkazu jsou vlastní členy, nikdy sdílené bity.** Sdílení bitů
  mezi různými typy příkazů byla chyba starého patche.
- **`GetTileTrackStatus` na dlaždici s depem** vrátí i depo — komu stačí
  jedna kolej, tomu to spadne na `TrackBitsToTrack()`.

---

# 12. Otevřené

- Pády po spojení a rozpojení: `pool_type.hpp:174` (sáhnutí mimo seznam
  objektů) a `track_func.h:168` (žádaná jedna kolej, dostala se jiná
  množina). Potřebuju `crash.log` — píše se do složky k `openttd.cfg`, ale
  jen když se po tom hlášení nechá hra sama doběhnout.

  Co už se o tom `track_func.h:168` ví, bez logu:

  - Hlásí se ta hláška **z `TrackBitsToTrack()` samotné**, ne z mého
    diagnostického assertu v `Train::ReserveTrackUnderConsist()`. Ten by
    ohlásil `train_cmd.cpp` a ohlásil by se dřív. **Takže to není
    `ReserveTrackUnderConsist()`** — a to je půlka podezřelých pryč.
  - Zbývají místa, která tu funkci volají: `DeleteLastWagon()`
    (`train_cmd.cpp`, maže vozy havarovaného vlaku po jednom),
    dvě místa v `TrainController()` s `chosen_track`, `signal.cpp` a
    `rail_cmd.cpp`.
  - `DeleteLastWagon()` je nejpodezřelejší: pro tunel/most má výjimku a
    o depu **ví** — o pár řádků níž se na depo ptá ve smyčce — ale samo
    volání `TrackBitsToTrack()` je nad tím a nechráněné. Vůz v depu tedy
    tu podmínku poruší. Že se u nás havarovaný vlak v depu ocitnout může,
    je novinka, kterou přinesl odtah.
- Po načtení savu odjely ze stanice mašinky, které čekaly na spojení —
  poskočil jim příkaz.
- Peron 1 a 2 při spojení mašinka+mašinka: výbuchy, po načtení savu
  zamrzání.
- Otočit směr na první dlaždici od depa zamrazí vlak.
- Ikonka `icons8-hammer-and-wrench-16.png` není zapojená (v kontejneru
  není `grfcodec`); až bude, patří k ní uvést `icon8.com hammer`.
- Odpojení přeskočené při prvním příjezdu po načtení hry.
