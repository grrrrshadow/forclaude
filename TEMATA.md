# Strom témat

Rejstřík projektu. Ne deník — **závěry**, které platí dál, roztříděné podle
toho, čeho se týkají.

- Jednotlivá kola testů → `TEST_LOG.md`
- Návrh funkce, dlouhé úvahy, historie rozhodnutí → `FEATURE_DESIGN_COUPLING_TOW.md`
- Jak se staví Windows build → `BUILD_NOTES.md`
- Odkud je zdroj hry → `openttd/VENDORED_SOURCE.md`

**Pozor při čtení návrhového dokumentu:** je psaný jako návrh před psaním
kódu a některá místa v něm popisují jako budoucí práci věci, které jsou
dávno hotové, nebo naopak takové, které se nakonec dělat nemusely (viz
4.1). Co platí **teď**, je tady ve stromu.

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
7. **Hráčův návrh je návrh, ne rozkaz.** Když hráč navrhne řešení a kód
   říká, že je to blbost, řeknu to rovnou a s důvodem — před tím, než na
   cokoliv sáhnu. Mlčky vykonat špatný nápad je horší než odporovat:
   stojí to testovací kolo. Platí to i obráceně — když nerozumím mechanice,
   řeknu to a doptám se, co je na obrazovce vidět, místo abych třikrát
   opravoval místo, kterému jsem jen věřil.
8. **Před editací si přečtu celý soubor, do kterého sahám.** Vyrobeno
   naostro: do credits jsem připsal řádek s „feature design", zatímco
   o obrazovku níž tentýž soubor říkal, že design je Karnův — a „karn
   s velkým K" jsem si vyložil jako novou přezdívku, místo abych si
   všiml, že Karn v souboru dávno je. Tři kola oprav na čtyřech řádcích
   textu. Zvlášť u souborů, kde spolu záznamy souvisí (credits, témata,
   překlady), platí: nejdřív přečíst, pak psát.

## 0.3 Na čem to celé stojí

Dvě věci, a ani jedna z nich není náš vynález:

**Palo123 (podepisuje se i jako karn) je inspirace.** Odtud je, jak to má vypadat a co to má umět pro
hráče — okno příkazů, „jet se spojit", „čekat na spojení", odpojit s
počtem, filtr podle nákladu. Není to předloha kódu: jejich větev je
z prosince 2020, z doby před přepsáním celého systému příkazů, takže se
dnes ani nepřeloží. Bere se **co dělají a jak to vypadá**, ne řádky.
Podrobně v kapitole 9.

**Couvání je vanilkové z beta 16, i s příznaky.** Kvůli tomu se šlo
z 15.3 na beta 16. Ve hře už je:

- skutečná jízda pozpátku (`DrivingBackwards`) místo magického přehození
  celé soupravy,
- GRF příznak **řídicí kabiny** (`HasCab`), který umí udělat vedoucí
  i z nemotorového vozu — tedy řídicí vůz,
- otázka „umí tenhle konec vést vlak?" (`CanLeadTrain`),
- **ořez rychlosti**, když vpředu kabina není (vanilkových 42 jednotek,
  tedy asi 68 km/h — hra počítá rychlost v imperiální jednotce, km/h je
  jednotka krát 1,609),
- nastavení, jestli se vlak smí otáčet (u nás zamčené na „Nikde").

**Nic z toho se nepíše znovu.** Naše práce je použít to správně —
především se po spojení **ptát**, ne nařizovat. Podrobně v kapitolách 1
a 8.

## 0.4 Zkušební nádraží — plánek

Testuje se pořád ta samá situace. Bez tohohle se úkoly o spojování číst
nedají.

**Světové strany** (aby „vlevo/vpravo" nebylo na čem záviset): **pravá
strana nádraží je severovýchod, levá jihozápad.** Vlaky čekající na
spojení a vagonky na rozpojení přijíždějí ze **severozápadu**. Mašinka
s příkazem „jet se spojit" přijíždí **z obou stran**.

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

**Rozpojování se netestuje tady.** Spojená dvojice z tohohle nádraží odjede
na **druhé nádraží s jedním nástupištěm** a rozpojuje se tam. Plánek výš
tedy platí pro to, **jak byla souprava složená**, ne pro místo, kde se
rozpojuje — a to je na něm to důležité: podle toho, na kterém nástupišti
se spojovala, má hlavu na jedné nebo na druhé straně, a to si veze s sebou.

Takže i u chyby, která se stane až na tom druhém nádraží, platí pravidlo
0.6 — jen se neptám „na kterém nástupišti to bouchlo", ale **„na kterém
nástupišti byla ta souprava spojená"**.

## 0.5 Pravidlo zrcadla

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

## 0.6 Podle kterých nástupišť to zlobí se pozná, co je špatně

**Tohle je nástroj na hledání, ne pravidlo hry.** Platí o *našem zkušebním
nádraží* a nikde jinde. Hráč si postaví nádraží, jaké chce, a žádné číslo
nástupiště pro hru nic neznamená — **v kódu se na nástupiště neptá nic a
ptát nesmí.** Kdyby se někdy stalo, že se chování hry liší podle toho, na
kterém nástupišti vlak stojí, je to chyba, ne vlastnost.

Proč to funguje: to nádraží je schválně postavené tak, že **směr** mají
všechna čtyři nástupiště stejný a **hlavu** mají 1 a 2 na opačné straně než
3 a 4. Je to tedy pokus, který ty dvě veličiny odděluje. Odtud:

- **zlobí to na všech čtyřech nástupištích → problém je ve směru;**
- **zlobí to jen na 1 a 2, nebo jen na 3 a 4 → problém je v hlavě.**

Než začnu hledat, zeptám se, na kterých nástupištích se to děje. Odpověď
rozdělí podezřelé na půl dřív, než otevřu jediný soubor. Na jiném nádraží
ta zkratka prostě neplatí a hledá se normálně — nic se tím nerozbije.

### 0.6b Třetí veličina: **kam ten vlak pojede dál**

Ty dvě veličiny výš nejsou všechno a **výsledek, který nesedí ani na jeden
z těch dvou vzorců, ještě neznamená, že je měření vadné.** Vlaky, které na
nástupištích čekají na spojení, nikam nejedou — ale **mají v příkazech
další cíl, a ten je na každém nástupišti na jiné straně**:

- **nástupiště 1** — nejbližší cíl **vlevo**,
- **nástupiště 2** — nejbližší cíl **vpravo**,
- **nástupiště 3 a 4** — mašinky mají hlavu vlevo, cíle taky každá na jinou
  stranu, a **spojuje se tam k jejich vagonkům zezadu vlaku**.

Proto může jedno nástupiště ze čtyř dopadnout jinak než ostatní, aniž by to
byla náhoda: **liší se tím, kterou stranou má spojený vlak odjet.** Přesně
tak vyšla tabulka spojení v `TEST_LOG.md` u buildu #85, kde jako jediná
prošla dvojka — a byla to právě ta se směrem odjezdu opačným než jednička.

Do tabulky podezřelých tedy patří i **spor mezi tím, kudy spojený vlak
podle nás má odjet (tou cestou, kterou přijela mašinka), a tím, kudy
opravdu potřebuje jet dál.**

**A přesně to z toho vyšlo.** Dvojka je jediné nástupiště se směrem
odjezdu doprava; 1, 3 i 4 jedou doleva. V tabulce spojení prošla dvojka a
propadla 1, 3 i 4. **Shoda čtyři ze čtyř na jedné jediné veličině** — a ta
veličina není číslo nástupiště, je to směr odjezdu. Viz 2.5.

**Co drží kód nezávislý na nádraží, je pravidlo 0.5:** nikdy nejmenovat
konec. Všechno se rozhoduje měřením — které konce jsou si blíž, kterým
směrem vlak přijel, který konec má řídicí kabinu. Dokud to tak zůstane, je
jedno, jak si kdo nádraží postaví.

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
- **Snížená rychlost**, když vedoucí konec nemá řídicí kabinu,
  **zůstává**. Je to vanilkové (42 jednotek, asi 68 km/h) a je to správně:
  nikdo nekouká dopředu.
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

- **Otočení seznamu není hotové bez příznaku, který vede.** `DrivingBackwards`
  se jmenuje „hlava" nebo „konec" — je to místo v seznamu. Když se seznam
  otočí a fyzicky vedoucí konec zůstane stejný, musí se ten příznak přehodit
  s ním. Zapomněl jsem na to dvakrát, tak to teď dělá přímo
  `ReverseConsistOrder()` a zapomenout na to už nejde.
- **Otočit odloženou soupravu zpátky se musí na dvou místech, ne na jednom.**
  Při rozpojení ve stanici i **v depu**, kde odtahovka odkládá porouchanou.
  V depu to chybělo a vypadalo to takhle: porouchaná nebyla hlavou ničeho,
  test „nerozpadlo se to?" ji zahodil a odjelo se od toho v půlce — zůstala
  napůl spravená a odtahovka držela úkol, který už splnila. Zvenčí to vypadá
  jako **„vyměnili si role"**. Obojí teď dělá jedna funkce
  `MakeEngineLeadTheList()`.
- **Kdo neumí říct, kam míří, se na nic neptá.** Vanilla nedává směr ničemu,
  co považuje za vrak, a každé místo, které z toho dělá kolej, na tom spadne
  (`track_func.h:237`). Není to jedna chyba, je to celá třída: volajících je
  spousta a žádný z nich to nekontroluje. Ptají se proto
  `FreeTrainTrackReservation()` a `TryPathReserve()` samy a v tom případě
  nedělají nic.
- **Sebraný vlak leží ve výpisu obráceně a musí se otočit zpátky.** Spojení
  musí složit oba díly do seznamu v tom pořadí, v jakém fyzicky stojí na
  koleji. Když se vlak sbírá za ten konec, který míří ke sběrné mašince, je
  jeho vlastní pořadí v tom společném seznamu **opačné** — mašinka mu skončí
  na konci místo v čele. Dokud jede jako cizí vagony, nikomu to nevadí.
  Jenže po rozpojení se **všechno ve hře ptá jen hlavy seznamu**, jestli je
  tohle vlak — takže se z celého vlaku stane bezejmenná řada vagonků stojící
  na nástupišti. A jen na těch nástupištích, kde se ty dva potkaly tímhle
  koncem, což je přesně pravidlo 0.6.

  Při rozpojení se tedy seznam otočí (`ReverseConsistOrder`). **Nic se
  nehýbe**, vozy zůstávají na svých dlaždicích, mění se jen pořadí. Musí za
  tím jít dvě věci, obě jsou účetnictví o seznamu, ne o zemi: natočení
  každého vozu (`NormaliseCoupledConsistFacing`, počítá se „směrem
  k tomu přede mnou v seznamu") a příznak, který konec vede — ten se
  jmenuje „hlava" nebo „konec", takže když se seznam otočí a fyzicky vedoucí
  konec zůstane stejný, **příznak se musí přehodit**.
- **„Řada vagonků" znamená chybějící mašinku v celém řetězu, ne vagon
  v čele.** Na tom se zahazovaly příkazy: řetěz s mašinkou na konci vypadal
  jako řada vagonků a přišel o ně.
- **Kdo čeká na spojení, si nic neplánuje.** Nehledá cestu, nerezervuje
  trať, jen drží zem, na které stojí. Příkazy mu přiveze ta mašinka, co si
  pro něj přijede. Stačilo zbourat kus trati a všechny čekající mašinky ve
  stanici se rozběhly hledat novou cestu a rezervovat si ji — vlak, který
  nikam nepojede, přitom nesmí držet trať, na kterou se stojí fronta.
  Výjimka je nakládání: to je jeho vlastní věc a doběhne samo.
- **„Překážka je to, pro co jsem přijel" musí znamenat právě ten jeden
  vlak.** Ptát se místo toho „byl by ten vpředu dobrý partner?" je jiná,
  mnohem širší otázka: dvě mašinky, obě zaparkované a čekající, projdou tou
  zkouškou každá na té druhé. Mašinka pak brala cizí stojící vlak za svůj
  cíl, hledání cesty ji pustilo za něj a jela po trati proti němu. Cíl je
  zapsaný jménem (`couple_target`, `rescue_target`) — tak se čte jméno.
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

## 2.3 Co je z 15.3 pryč a nemá se to vracet

Na 15.3 byl kolem spojování postavený **vlastní hledač cesty** — vlastní
typ cíle `CYapfDestinationCoupleRailT`, `IsAdjacentToCouplePartner()`,
`YapfTrainFindCouplePosition()`, `YapfTrainCheckReverseForCouple()`.
Důvod: běžné hledání cesty se úmyslně vyhýbá obsazené dlaždici, takže
neumí zacílit na místo hned vedle partnera, a vlak skončil jako zaseknutý.

**V beta16 z toho nezůstalo nic a je to tak dobře.** Nahradily to dvě
mnohem menší věci:

- cíl je normální stanice, jen **zúžený na to nástupiště, kde partner
  stojí** (`couple_at_dest_station` v `yapf_destrail.hpp`),
- a povolení **zastavit těsně před partnerem** v `pbs.cpp`
  (`IsSafeWaitingPosition`, `IsWaitingPositionFree`).

Kdyby se ta stará cesta chtěla vracet, tohle je důvod, proč ne: byla to
celá souběžná kopie hledání cesty, kterou by bylo potřeba držet v souladu
s tou pravou.

## 2.4 Vedoucí konec po spojení

Má-li kabinu jen jeden konec, vede ten. Mají-li ji oba — a to je přesně to,
co z vlaku udělá připojení mašinky z druhé strany, tedy push-pull — jede se
tudy, kudy se přijelo, protože o té cestě se ví, že je průjezdná.

## 2.4b Dvě věty, které se nesmí zaměnit

**Tohle je teď to hlavní pravidlo a přebíjí obě verze 2.4 i 2.5 níž.**

- **Po spojení mašinka jede dál tím směrem, kterým přijela.** Sebrat něco
  tě neotočí. Spojení **nemění směr jízdy nikdy**.
- **Po rozpojení mašinka směr změnit musí** — je to logické a hráč to
  čeká. Musí odjet **pryč od vozů, které nechala stát.**

**Pozor, tady se to už jednou popletlo:** věta „odjeď pryč od toho, cos
odložil" patří **výhradně rozpojení**. **Spojení nic neodkládá.** Kdo tuhle
větu přilepí ke spojení, vyrobí přesně to otáčení po spojení, kvůli
kterému se vlaky rozsypaly.

A druhá polovina téhož, ať se nevyrábí zákaz, kde žádný není: **mašinka
po spojení smí projet skrz vlak i skrz vagony a tlačit je před sebou.**
To je v pořádku a je to normální provoz.

Vagonky se takhle chovaly odjakživa — a proto odjížděly předvídatelně —
a vlak sebraný z jiného vlaku **není jiný případ a nesmí se tak chovat.**
Tak to bylo ve `#81`.

Co to nahradilo: pravidlo „spojený vlak odjede tou cestou, kterou mašinka
přijela", které vlak na místě otočí. Bylo to jediné místo, kde spojování
měnilo směr jízdy, dělalo to rozdíl mezi mašinkou sbírající ze
severovýchodu a z jihozápadu, a nikdo o to nežádal.

**Otočit vlak smí teď už jen:**

- **rozpojení** — mašinka musí odjet od vozů, které nechala stát,
- **konec kolejí** a **konec hlavového nádraží**,
- **hráč sám** tlačítkem otočení. To je něco úplně jiného, protože ho
  zmáčkl.

**Výjimka o řídicí kabině je smazaná nadobro, tentokrát s důkazem.**
Dvakrát byla v kódu a dvakrát dělala totéž zlo, chycené bezhlavým rigem
(viz 2.10) přímo při činu: narovnání natočení po spojení zapíše hlavové
mašince nos vždycky „od těla vlaku" — tedy pryč od toho, co právě
sebrala — takže „vede konec s kabinou" znamená vždycky „vrať se, odkud
jsi přijel", ať mašinka přijela jakkoli. Hráč měl pravdu, že „v #90 to
jezdí blbě, takhle jako teď" — kabina byla v #90 i v mém včerejším
vrácení, a moje archeologie z deníku byla špatně.

**Platí jediné pravidlo pro všechno, co se spojuje: „neměnit směr
jízdy".** A po hráčově hlášení z reálného nádraží padlo i **měření** toho
pravidla — zůstala jen jeho konstanta:

Měřilo se „kterým směrem mašinka přijela" v okamžiku spojení. Jenže krok
před spojením mašinku otočí na místě **zábrana před volnými vagonky**
(„konec koleje (pri jizde) — volne vagony": aby do stojící řady nenajela,
hra ji zastaví otočkou). Směr přečtený po té otočce ukazuje zpátky domů,
a „pokračuj svým směrem" pak poctivě znamenalo „vrať se, kudy jsi
přijela" — i s vagonky. Čistá scéna rigu se u vagonků neodráží, takže
tam měření vycházelo; na skutečném nádraží se odráží skoro vždycky
(protokol save91 to měl ve stopách celou dobu: „pred spojenim - smer
prijezdu 5" u sběračky, která jela na západ).

Konstanta: mašinka jela **ke svému cíli**, takže pokračovat znamená
projet skrz to, co si přijela sebrat — vytlačit to před sebou. A po
spliceu visí sebrané vždycky na konci seznamu a nos hlavy ukazuje od
těla. Takže „neměnit směr" je stavebně **vede konec s vagonky**
(`DrivingBackwards = true`), pokaždé, bez měření. Trasu k dalšímu
příkazu si pak najde pathfinder až po odjezdu — vyjede se protější
stranou nádraží a k cíli za zády se objíždí okruhem, ne otočkou na
peronu. Kdo se opravdu má vracet, má na to hráčův reversní chod.

Zrušeno je i doptávání se hledače cesty hned po spojení (viz 2.5). Bylo
vyzkoušené a otáčelo vlak na třech nástupištích ze čtyř. Vlak, který má
jet opačně, se otočí **jako každý jiný vlak** — na konci nástupiště, sám
od sebe, až se rozjede.

## 1.5 Čudlík otáčení kolem depa je zatmavený všude, kde by nic neudělal

Otáčení hra odmítne **na dvou místech**, ne na jednom:

- **napříč vraty depa** — část vlaku uvnitř, část venku,
- **uvnitř depa, ale rozjetý** — na cestě ven. Vlak už leží podél kolejí,
  i když jsou všechny vozy pořád schované na políčku depa.

V obou případech příkaz **jen tiše nic neudělá**. Z pohledu hráče je to
mrtvý čudlík bez hlášky, tak na něj klikne třikrát za sebou. Proto je teď
zatmavený v obou případech, ne jen na vratech.

**Ptá se to jednou funkcí (`IsTrainReverseBlockedByDepot`), kterou používá
i samotný příkaz.** Nesmí nastat, že čudlík vypadá živě a příkaz odmítne —
to jsou dvě různé odpovědi na jednu otázku.

Přes konzoli se to dá vypnout: `depo123` (viz `_allow_reverse_on_depot_doorstep`).

**Samotné zamrzání tím vyřešené není** — jen se přestalo dát vyvolat
čudlíkem.

## 2.4c Konzolový příkaz `vlak123` — vidět, kterým koncem vlak jede

Obě veličiny z 0.5 nejde z obrazovky přečíst vůbec, a skoro každá chyba
ve spojování byla tím, že si odporují. `vlak123` je proto přidá do řádku
stavu v okně mašinky (`vlak123 on` / `vlak123 off`, samotné přepne):

- **H** — jede hlavou seznamu napřed, **Z** — jede zadkem (koncem).
  To je příznak „jede pozpátku".
- **DP** — nos prvního vozu míří **od** vlaku, **DZ** — míří **do** vlaku.
  Po srovnání směrů při spojení má vždycky vyjít DP; **kdykoli se objeví
  DZ, je rozsypané srovnání směrů** a je to chyba, ne stav.
- **R č.** — tuhle řadu si někdo zamluvil ke sběru, a to číslo je jeho.
  Jinde se to nedá zjistit nijak.

Je to zkušební přepínač, ne nastavení — stejně jako `depo123`.

## 2.6 Dvě opravy po testu #91

**Proč se vlak s vagonky otáčel, ať se na otázku otáčení sáhlo jakkoli.**
Hra se při každém posunu příkazu ptá „nechtěl bys k dalšímu cíli jet
druhým koncem?" (`CheckReverseTrain` v obsluze tiku). Nekonečná pokuta
za otočení prohraje jen tehdy, když dopředu žádná trasa k cíli nevede —
a přesně to je případ vlaku, který sebral vagonky a jehož další cíl leží
za zády na síti bez okruhu. Vlak, který sebral vlak, má další cíl
dosažitelný dopředu, proto se nikdy neotočil — stejné pravidlo, jiná
geometrie cílů. To je celý rozdíl „vlaky dobře, vagonky špatně".

Uzavření příkazu „na místě" tu otázku nevypnulo: posunulo jen index, a
samotné přepnutí na další příkaz provedl až `ProcessOrders` v obsluze
tiku **o tik později** — ohlásil posun a otázka padla stejně. Tou
skulinou se otočení vracelo, ať se otázka hlídala lístkem, vypínala
plošně nebo měřila kolej: pokaždé se stihla položit jindy nebo jinudy.

Oprava: **závěr spojení dokončí posun příkazu celý sám** — po posunutí
indexu hned zavolá `ProcessOrders`. Obsluha tiku pak žádný posun nevidí,
otázka nepadne a vlak odjede tak, jak ho spojení nasměrovalo: dál svým
směrem, vagonky vytlačí druhou stranou ven. Vlak, který se opravdu má
vracet, na to má hráčův reversní chod. Žádný stav na vlaku, žádná
výjimka.

**Na otázku otáčení se nesahá.** Pokus „kde vede kolej dopředu, nikdy se
neotáčej" vzal s sebou i otočení, na kterém stojí příjezd na spojení —
mašinka, co se potřebuje přehodit a nacouvat na vagonky, se točí právě
skrz „dopředu to k cíli nejde". Proto spojování začalo vycházet jen
někdy. Vráceno do podoby #91: pokuta je povolení, otočit se lze jen
tam, kde dopředu žádná trasa není.

**Po spojení s vagonky se nenakládá ani nevykládá — odjezd.** Příkaz
„jet se spojit" se po spojení uzavře na místě a nikdy předáním vlaku
stanici: vstup do stanice spouští nakládku a spojení není příjezd pro
náklad. Práce s nákladem na téhle stanici patří samostatnému příkazu,
který napíše hráč.

Jednorázový lístek na vlaku (příznakový stav, léčil příznak) byl špatně
a je pryč nadobro.

**Stopnutý vlak není partner.** Vlak, který hráč ručně zastavil, čeká
s příznakem „čekat na spojení" úplně stejně jako předtím — ale stopnutí
je gesto „nesahat": nenakládá, neodjíždí, a nesmí být ani cílem pro
mašinku, která se jede spojit. `IsWaitingToBeCoupled` ho teď vynechá;
porouchaného nebo havarovaného vlaku (odtah) se to netýká.

**Rozpojení, které mění vedoucí konec, dělá i doprovodnou práci.** Změna
vedoucího konce není jen příznak: vanilkové couvání ke každému vozu
otočí bookkeeping kopec/z kopce a znovu ho ohlásí na jeho políčku, aby
věděl, na které koleji stojí. Rozpojení jen přepínalo příznak, takže
vlak, který dojel vedený opačným koncem (po odjezdu reversním chodem),
měl po rozpojení rozbitou geometrii a o pár políček dál bouchl (assert
z #88). Vlak dojetý mašinkou napřed má z přepnutí no-op — proto VV bez
reversního chodu a VB s ním. Teď se ta práce udělá, ale **jen když se
příznak opravdu měnil**.


## 2.5 …ale „kudy se přijelo" je jen odhad, ne odpověď

To pravidlo výš je **odhad pro případ, kdy nic lepšího není**. Vlak ale má
příkazy a další místo, kde má být, může klidně ležet na druhou stranu. Na
zkušebním nádraží leží na druhou stranu **u tří nástupišť ze čtyř** — a
právě ty tři v tabulce spojení propadly (viz 0.6b).

Co se dělo: hra si tuhle otázku normálně vyřeší sama, ale **jen zároveň
s posunem příkazu** — kontrola „nemám se otočit?" visí až za tou
podmínkou. A vlak, který se spojil, má příkaz odbavený ručně u nás. Takže
se nikdo nezeptal, vlak si **zamluvil cestu ven tím směrem, který vyšel
z odhadu**, a vyjel přes celé zhlaví proti všemu, co jelo proti němu.

Proto se teď hned po spojení, **ještě než se zamluví cesta ven**, zeptáme
hledače cesty (`CheckReverseTrain`), kterým směrem se jede k dalšímu
příkazu, a když řekne opačně, vlak se obrátí. Zamluvená cesta a to, co vlak
opravdu udělá, jsou tím pádem jedno a to samé. Průjezdnost si hledač
ohlídá sám, takže se oproti odhadu nic neztrácí.

## 2.9 Odpojit a spojit na příkazu „jeď do depa"

Příkaz do depa má přepínače **Odpojit** a **Jet se spojit** (vzájemně se
vylučují), **žádné „čekat na spojení"** — vlaky se spojují na nádraží, kde
je to vidět; v depu se spojuje jen s odstavenými vagonky. Filtry
náklad/plnost/počet stejné jako na nádraží. Berou se i řady odstavené do
depa ručně přetažením — odstavená bezhlavá řada v kůlně nikam nejede,
takže „stojí tam" je celý význam čekání a žádný příkaz k tomu nenosí.

**Pravidlo konce — kotva, na které jsme se shodli:** výsledek spojení
v depu vypadá **přesně jako normální vlak s vagonky, který do depa vjel
celý a hráč ho otočil.** Vagonky se připojí na ten konec mašinky, který
kouká ke dveřím; ven vyjedou vagonky první a mašinka je tlačí; jako
úplně poslední opustí depo ten její konec, který vjel dovnitř první
(komínem napřed → komín ven poslední; nacouvala → zadek ven poslední).
Uvnitř se nic samo neotáčí a nic se neměří — konec vybral hráč tím, jak
vjel. V kódu se ten stav vyrábí týmž operátorem, kterým hra vlak v depu
otáčí (`TurnTrainInsideDepot`), žádnou napodobeninou.

Odpojení v depu: zbytek zůstane odstavený (bezhlavá řada bez příkazů;
odpojený celý vlak zůstane zabrzděný a příkazy si nechá). Práce se dělá
v bezpečné chvíli tiku, stejné jako odtahové úkony — příjezd si počet
zapíše (`depot_decouple_pending`) a tik ho vykoná.

**Tři úniky příznaků do zastávek cestou, chycené rigem při prvním
depovém běhu** (vlak s vypnutým non-stop staví všude a stav nakládky
podědí pole z pracovního příkazu):

- odpojení se provádělo v první stanici cestou — teď jen ve stanici,
  kterou příkaz jmenuje; depový počet spotřebuje jedině depo;
- nakládková větev spojování držela sběrače „na partnera" v cizí
  stanici — teď platí jen v cílové stanici příkazu;
- držení „nevyjížděj bez zamluvené řady" četlo příznak z nakládky a
  ptalo se na řadu v cizí stanici — teď platí jen na skutečném
  cestovním příkazu (jeď do stanice / jeď do depa).

A stráž „nevjížděj na políčko bezhlavé řady" má výjimku pro dlaždici
depa — v kůlně se nic nesráží a odstavená řada nesmí zavírat dveře
mašince, která si pro ni jede. Pád okna příkazů (assert na kliknutí bez
vybraného příkazu, crash log 2026-08-28) nahrazen tichou pojistkou u
všech čudlíků spojovací řady.

## 2.10 Bezhlavý zkušební rig — hra se testuje sama

Konzolový příkaz `testspoj` postaví celou zkušební scénu příkazy hry:
rovný pás, depo na obou koncích, průjezdné nádraží uprostřed, path
návěstidla, rozvozová souprava (doveze vagonky, rozpojí, zaparkuje) a
sběrná mašinka s příkazem „jet se spojit" a dalším příkazem za zády —
přesný tvar hráčova případu. `testspoj couvej` navíc sběrače v depu
otočí, takže na vagonky nacouvá. Vlastní čekání sběrače na řadu celou
scénu časuje samo, žádné řízení zvenčí.

Běží to bez obrazovky: `openttd -vnull:ticks=20000 -snull -mnull -x`
s `autoexec.scr` = `newgame` a `game_start.scr` = `vlak123 on` +
`testspoj`. Pod nulovým videem jde výstup konzole na stdout, takže celý
průběh — „pred spojenim", „spojeno", „vjel do stanice/depa", každá
změna vedoucího konce s příčinou, tep `teststav` — se čte jako protokol.
Správný odjezd končí „vjel do západního depa" (vytlačené vagonky),
špatný „vjel do východního". Tímhle rigem byla chycena a spravena vada
směru odjezdu; hráč už nemusí testovat každou hypotézu na telefonu.

---

## 2.11 Blikající rezervace u vrat depa — otrávený pahýl

**Co hráč viděl.** Krátká mašinka bez vagonů vyjížděla z depa, rezervace
pod ní blikala, a na křížení před depem se srazily dva vlaky. Ve velkém
testu tomu odpovídala stará odchylka R3/R4: jedna sběračka ze čtyř
poskakovala v ústí depa (třikrát „konec koleje (pri jizde)" a návrat do
vrat) a k vagonkům pak dojela s propadlým příkazem spojení.

**Příčina — jedna, obojí z ní.** Když si vlak v depu žádá o cestu ven a
skutečná trasa k cíli je zrovna neprůjezdná (drží ji vlak před ním), hra
má nouzovou větev „nenašel jsem cestu → zarezervuj aspoň nejbližší
bezpečné místo". Ta větev počítá **konec koleje za bezpečné místo** —
a záda jednosměrného návěstidla jsou z pohledu vlaku právě konec koleje.
U depa B tak vyšel „úspěch": jednopolíčkový pahýl na oblouk křižovatky,
který končí v zádech jednosměrky vjezdu. Vlak vyjel, o krok dál narazil
na neprůjezdnou jednosměrku, hra ho na místě otočila (u jednosměrky se
nečeká, jinak by stál navěky), vjel zpátky do vrat — a znovu. Každý
cyklus = rezervace tam, rezervace pryč: to je to blikání. A protože vlak
mezi otočkami jezdí po pahýlu, který nikam nevede, na křížení, kudy
pahýl vede, se s ním může potkat vlak, který tudy jede doopravdy.

**Oprava příčiny.** Nouzové „zarezervuj cokoli bezpečného" se nespouští
pro vlak, který ještě stojí v depu — ten už na nejbezpečnějším místě je.
Když cesta ven zrovna není, vlak zůstane ve vratech a ptá se každý tik
znovu, stejně jako to celou dobu dělaly ostatní mašinky ve frontě (těm
pahýl nevyšel jen proto, že ho vždycky držel právě ten jeden smolař).
Vlaku na širé trati zůstává nouzová větev beze změny — tam bezpečné
místo potřebuje; a skutečně nalezená cesta končící na kusé koleji
nástupiště jde jinou větví a nemění se taky.

**Ověřeno rigem na hráčově save91** (`teststartdepo 70 122` +
`testklon 11 3 [reverz] za 8000`): před opravou 3 odrazy od vrat a
jedna nesebraná souprava; po opravě žádný odraz, spojení 4/4 s reverzem
i bez, srážek 0. Tím je uzavřená i odchylka R3/R4.

**Nové oči rigu:** příkaz `testmapa <x1> <y1> <x2> <y2>` vypíše koleje,
návěstidla (směr, jednosměrnost, barvu) a rezervace výřezu mapy; stopa
výjezdu z depa hlásí, kde rezervace končí; „konec koleje" hlásí důvod
(červená / žádná kolej / cizí kolej / volné vagony) a zmizení či zabrání
cíle spojení se vypisuje.

**Dodatek 2: z čekání se vyjíždí jen s rezervací.** Hráčova fotka z #101:
sběračka čekala bez cíle na širé trati (čekání jí cestu právem zahodilo),
a v okamžiku, kdy odkladačka složila vagonky, **vyrazila bez cesty** —
okno poctivě psalo „Nelze dosáhnout…" a přesto jela — přímo do odkladačky,
která si svou cestu držela správně. Uvolnění z čekacího bloku teď
vyžaduje rezervaci: stojící bezpečná rezervace pustí vlak hned, jinak se
zkouší TryPathReserve (každý osmý tik) a do té doby vlak stojí. Vagonky
se tak sbírají, až když odkladačka uhne — což je přesně pořadí, které na
tom nádraží platí i pro všechno ostatní. Ověřeno maticí: sběračky hned
za odkladačkami, za 500, za 1500 i standardní protokol s reverzem i bez
— všude 4/4, nula havárií.

**Konzole do souboru: `vlaksav [jméno]`.** Uloží celý zpětný zásobník
konzole (od nejstaršího řádku) do `vlaksav.txt` (nebo `<jméno>.txt`)
v osobní složce hry — hlášení nehody pak nemusí být po screenshotech.
Hloubku zásobníku řídí hráčova nastavení `console_backlog_length`
a `console_backlog_timeout`; na dlouhé záznamy je dobré si délku zvednout
(`setting console_backlog_length 1000` v konzoli).

## 2.12 Spojení se uzavírá u nosu — jinak se vlak doplíží partnerovi pod kola

Sbíječka dojede k partnerovi, kolizní kontrola ji zastaví na šířku
spřáhla — a spojení se má uzavřít hned na místě. Neuzavíralo se:
hledání partnera šlo **po rezervaci**, a čekající **vlak** (na rozdíl od
bezhlavé řady) má rezervaci i sám pod sebou. Ta navazuje na cestu
sbíječky beze švu, procházka po ní partnera přejela a skončila kdesi
daleko za stanicí — „nikdo tam není". Sbíječka se tedy každý tik znovu
rozjela, o jednotku popolezla, znovu dostala stopku… a spojení se
uzavřelo teprve, když oba stáli na **stejném políčku** — o 3 jednotky
blíž, než se sluší. Ta těsnost pak přežila celé vlečení (vlak jede
v zákrytu, rozestupy se nemění) a vybuchla až při rozpojení: obě půlky
stály v kolizní vzdálenosti a první pohyb kterékoli z nich — **i směrem
pryč** — kontrola prohlásila za srážku. V rigu čtyřikrát ze čtyř;
u hráče se to jevilo jako „stojí navždy", protože tam mašinku držela
ještě falešná překážka (téma 3) a k pohybu vůbec nedošlo.

Oprava u kořene: hledání partnera se **nejdřív podívá na políčko přímo
před vlastním koncem** (fyzická sousednost — o tu tady celou dobu jde)
a teprve pak věří procházce po rezervaci. Spojení se tak uzavře na
první stopce, rozestup zůstane na šířku spřáhla a rozpojení je zpátky
symetrické se stavěným vlakem. Pro páry spojené natěsno ve starších
hrách platí záchranná brzda z tématu 3: odjezd od sebe není srážka.

**Dodatek: rezervace nemají majitele — a vlak v depu žádnou nemá.**
Hráč vyfotil sběračku, která si zarezervovala trať přes stojící
odkladačku; rig chytil mechanismus u vrat depa: sběračka **bez cíle**,
stojící v depu, si každých 32 tiků „uvolňovala svou cestu" — jenže vlak
celý v depu žádnou vlastní rezervaci nemá, a to uvolňování vykráčelo
z vrat a **mazalo čerstvé výjezdové rezervace ostatních mašinek ze
stejného depa**. Dvě se pak potkaly na prahu. Oba čekací bloky (sběračka
bez cíle i vlak čekající na sebrání) teď uvolňují cestu jen mimo depo —
v depu není co pouštět. Ověřeno: sběračky puštěné hned za odkladačkami
i s prodlevami 500/1500 tiků — dřív 2–3 srážky na běh, teď 4/4 spojení
a nula havárií.

---

## 2.13 Depo: prázdný odjezd se neděje a rezervovaná řada je vidět

**Sběračka nesmí vjet do depa a vyjet bez vagonků.** Příjezd do depa
uzavíral rozkaz (`VehicleEnterDepot()` ho přepsal na dummy), takže když
vagonky ještě nedorazily nebo je někdo mezitím sešrotoval, mašinka
poskočila na další rozkaz a vycouvala prázdná. Všechno, co dál v cyklu
na těch vagoncích stojí, pak jedno kolo nesedí — na nádraží prostě
chybí a nic to neřekne. Rozkaz „jeď se spojit do depa" se proto při
příjezdu **neuzavírá**; uzavře ho až samo spojení (`TryCoupleAtDepot()`),
přesně jako u mašinky, která už v depu stála, když rozkaz přišel. Vedlejší
zisk: mašinka, která přijede dřív než vagonky, na ně v depu počká
a sebere je, jakmile dorazí.

**Řádek „reservováno" v okně depa.** Bezhlavá řada, kterou si už nějaká
mašinka zabrala (`couple_claim`), se v okně depa vyzvedne **nad ostatní
řádky** a vpravo dostane nápis *reservováno*. Řádky se objeví jen tehdy,
když pro vagonky opravdu někdo jede — zábor se ptá na obě půlky najednou
(`IsRakeClaimedForCoupling()`), takže zábor po mašince, která už nejede,
řádek neudrží. S rezervovanou řadou hráč v okně depa nic nedělá: klik,
tažení ani prodej přes okno na ni nesáhnou. Cesta zpátky vede přes
mašinku — sešrotovat ji nebo jí poskočit rozkaz zábor pustí (téma 2.2).

Zábor bere mašinka, která může být přes půl mapy, takže se v depu v ten
okamžik nic nemění a okno by se samo nepřekreslilo: proto
`MarkCoupleClaimChanged()` u záboru i u jeho uvolnění a rozdělení řad
při každém obnovení seznamu, ne jen při jeho stavbě.

**Ten řádek musí být opravdu uzavřený, ne jen neklikací.** Hráčovo
hlášení: mašinka jede pro dva vagonky, hráč mezitím vagonky kupuje —
a nově koupené naskáčou na rezervovaný řádek a mašinka odveze všechny.
Okno depa na rezervovanou řadu nepustí klik ani tažení, jenže okno není
jediná cesta dovnitř:

- vagon postavený v depu si hledá řadu **svého druhu**, ke které se
  připojí (`FindGoodVehiclePos()`) — a rezervovaná řada je přesně taková;
- nově postavená **lokomotiva** si naopak posbírá volné vagony, které
  v depu stojí (`NormalizeTrainVehInDepot()`) — a co je zabrané, volné
  není.

Obojí teď zabranou řadu přeskočí, a jako záchytka to navíc odmítá i sám
příkaz `CmdMoveRailVehicle` na obou koncích. Samotné spojení tudy nechodí
(`TryCoupleAtDepot()` i `AssembleDepotRake()` spojují přímo), takže
zámek nic nerozbíjí.

Naměřeno na scéně `koupitpri` (dvě sběračky si zaberou po 2, pak hráč
koupí 3 vagony). **Bez pojistky** hned po nákupu: řada o **5 vozech
a bez nároku** — koupené vagony pohltily rezervovanou řadu i s rezervací.
**S pojistkou:** rezervované řádky zůstávají 2 a 2, koupené si udělají
vlastní řadu o 3, a každá sběračka odveze přesně své dva.

**Nápis na řádku** je černý, normální velikosti a nese **číslo
lokomotivy**, která si řadu zabrala — v jednom depu může být zabraných
řádků víc a „reservováno" samo o sobě neřekne čí. Malé oranžové písmo
bylo na telefonu špatně vidět.

---

## 2.14 V depu smí jeden rozkaz odpojit i připojit

Na nádraží jsou to protiklady — dva kusy práce na opačných koncích jedné
zastávky a jeden rozkaz dělá jeden z nich. **V depu ne:** vlak stojí celý
a schovaný na jedné dlaždici s brzdou, obě půlky jsou tatáž operace na
soupravě ve stejném bezpečném okamžiku tiku, a kůlna je přesně místo, kam
se jezdí jednu řadu nechat a druhou si vzít. Rozkaz do depa proto smí
nést obojí a dělá to v pořadí, v jakém by to dělal hráč: **nejdřív nechat,
pak vzít.** Zákaz na nádraží platí dál, čekání na sebrání vylučuje pořád
všechno.

Vyžádalo si to jedno nové políčko, `Train::depot_dropped_rake`: **co vlak
zrovna nechal, si nesmí hned vzít zpátky.** Bez toho odloží řadu a
v příštím tiku si ji sebere, znovu a znovu, a rozkaz nikdy neskončí.
Jméno platí jen dokud vlak stojí v té kůlně na tom rozkazu — jakmile
vyjede, je to obyčejná odložená řada a smí pro ni být poslán jako
kdokoli jiný. Ukládá se: čekání na vhodnou řadu může být dlouhé.

Scéna v rigu: `testspoj depo oboji` — v depu leží 2 odložené vagony,
odkladačka přiveze 3 a má je vyměnit. Ověřeno: před 4 vozy / 2 odložené,
po výměně 3 vozy / 3 odložené, a vlak dojede domů.

---

## 2.17 Přepínač: reversní chod se u „jeď se spojit" doplní sám

V nastavení hry, stránka **Příkazy**, přibyl přepínač. Zapnuto (výchozí)
říká *„automaticky, po připojení ve stanici vlak odjede stejnou stranou
nádraží kterou přijela lokomotiva"*, vypnuto *„manuálně, po připojení na
průjezdném nádraží lokomotiva nezmění směr jízdy"*; nápověda dole zní
*„automatické doplňování reversního chodu u příkazu jeď se spojit."*

Funguje to jen jako **předvyplnění**: ve chvíli, kdy hráč u rozkazu na
nádraží zmáčkne *jeď se spojit*, zamáčkne se s ním i *reversní chod*.
Čudlík zůstává čudlíkem — kdo ho nechce, hned ho vymáčkne. Vypnutý
přepínač nedoplňuje nic a všechno je jako dřív.

**V depu se nedoplňuje nic**, a to dvakrát: podmínka je vázaná na čudlík
u stanice, a `MOF_REVERSE_OUT` navíc přijímá jen rozkaz typu
`OT_GOTO_STATION`. Kudy se vyjíždí z kůlny, rozhoduje to, kudy se do ní
zajelo (téma 2.13), ne přepínač.

Řádek v nastavení nenese jméno, ale rovnou **stav** — název `str` je
holé `{STRING}` a hodnotu dodává `val_cb` (`CoupleAutoReverseValueText`).
Co si hráč z toho řádku potřebuje přečíst, je která z těch dvou možností
zrovna platí, ne štítek, jehož význam si pak musí pamatovat.

---

## 2.15 Číslo na rozkazu do depa je „sber tolik", ne „najdi řadu o tolika"

Nástupiště a depo nejsou stejné místo a **stejné číslo tam neznamená
totéž.** Řada u nástupiště je celek, který tam někdo postavil celý:
„čtyři vozy" tam znamená **kterou řadu** vzít, a řada o pěti to není.
Depo je **sklad**. Vagonky do něj přijdou po pár kusech od toho, kdo
zrovna jel kolem, a leží tam v hromádkách, které pro sbírající mašinku
neznamenají nic. „Čtyři vozy" tam znamená **čtyři vozy**.

Dřív to bylo `count != GetCoupleCount()` — přesná shoda celé řady — i
v depu. Hráč pak měl v depu spoustu odložených vagonků a mašinka stála,
protože žádná hromádka neměla přesně tolik, kolik chtěl rozkaz.

Teď: `AssembleDepotRake()` v depu **spočítá, co tam leží**, a když je toho
dost, vezme přesně požadovaný počet — klidně napříč několika
hromádkami — a zbytek nechá stát jako vlastní řadu. Když toho dost není,
**nevezme nic** a čeká. Půl nákladu je horší než žádný: všechno dál
v cyklu na tom počtu stojí. Filtry na náklad a naloženost se pořád ptají
celé odložené řady (vybírají, ze kterých hromádek se smí brát); počet
říká, kolik z nich vzít.

**Rezervace se udělá tak, že se ty vagonky opravdu poskládají**, hned při
záboru, ne až při příjezdu. Tím je rezervace vidět a tím jich může být
v jednom depu víc: zabraná řada **je** řada — stojí v depu, má v okně
depa svůj řádek s nápisem *reservováno* — a zbytek je taky řada, ze které
si další mašinka udělá svou. Nikde se nemusí držet „dva z těch pěti",
protože po tom kroku nic takového neexistuje: je tam dvojka a je tam
trojka.

Scény v rigu: `testspoj depo sklad <počet>` — v depu leží řady o 5 a 3
vozech a jedou pro ně **dvě** sběračky. Naměřeno: „sbírej 2" → obě
sběračky odjedou se 2 (v okně depa mezitím dva samostatné zabrané řádky,
každý na svou mašinku, a jeden volný); „sbírej 6" → první vezme 5+1
**napříč dvěma řadami**, druhá čeká; „sbírej 20" → nikdo nevezme nic
a nic se v depu ani nepřeskládá.

## 2.16 Změna filtru se musí projevit hned

**Hráčovo hlášení:** filtr nastavený špatně (vagonky na zrní, v depu jsou
uhelné) mašinku zastaví — a **když ho hráč opraví na uhlí, už to
nezabere.**

Příčina: `v->current_order` je **kopie** rozkazu pořízená ve chvíli, kdy
se rozkaz stal aktuálním. Úprava rozkazu v seznamu tu kopii nemění.
`CmdModifyOrder` už kopii dorovnával — ale **jen u rozkazu na nádraží**;
u rozkazu do depa přenášel jedinou věc, otáčení v depu. Filtry a
„připojit" tam nebyly vůbec. Mašinka tedy do konce hry četla filtr, se
kterým do depa dorazila.

Zvenku to nejde poznat: vlak čekající na špatnou věc a vlak čekající na
správnou věc, která tam není, vypadají úplně stejně.

**Opraveno** — rozkaz do depa dorovnává totéž co rozkaz na nádraží. A ke
všemu: **když se změní popis toho, co se má sebrat, pustí se už zabraná
řada a volba se udělá znovu.** Filtr říká, co sebrat; když se změní,
musí se odpověď spočítat nanovo, jinak vlak dál veze to, co mu hráč
právě zakázal.

Scény: `filtrspatny` (špatný filtr, nikdy neopraven → 0 spojení, čeká) a
`filtropraveny` (špatný filtr, po chvíli opraven → sebere hned).

**Řádek rozkazu čte v pořadí, v jakém se to opravdu dělá.** Bylo
„(jet se spojit) … (a nechat si 1)", je „(nechat si 1) (a připojit)".
Napřed nechat, pak vzít — jak by to dělal hráč rukou. Slovo *připojit*
místo *jet se spojit*, protože v depu se nikam nejede.

---

## 2.15b Poznámka k rigu

**Dvě chyby, které si ta scéna sama na sobě našla.** Vagon postavený
v depu se sám připojí k volné řadě svého druhu, která tam už stojí
(`FindGoodVehiclePos()`), a **žádný příkaz volnou řadu nerozdělí** —
`CmdMoveRailVehicle` bez cíle vagon neodpojí, ale hledá pro něj dobré
místo, a to dobré místo je ta řada, ze které přišel. První dvě verze
scény o tom nevěděly a postavily jednu řadu o osmi. Test „napříč dvěma
řadami" pak procházel z úplně jiného důvodu, než měl. Dvě oddělené řady
v jednom depu jdou udělat jedině **z jiného druhu vagonů**.

## 2.18 Příkaz se jmenuje „připojit" a na řádku se nepíše, co každý ví

Dvě věci od hráče, obě o místě na řádku rozkazu.

**Jedno jméno pro obojí.** „Jeď se spojit" bylo dlouhé a v depu se týž
rozkaz jmenoval jinak než na nádraží. Všude je teď **„připojit"** —
čudlík, přípona na řádku i text v nastavení hry. K tomu sedí protějšek
„odpojit", který se tak jmenoval odjakživa.

**Na řádku se nepíše „vagony" ani „náklad".** Plné a prázdné můžou být
jedině vagony a uhlí může být jedině náklad; ta dvě slova zabírala třetinu
místa a neřekla čtenáři nic, co by nevěděl. Na řádku je tedy
`(plné) (uhlí) (3 vozů)`. Na čudlících popisky zůstaly — tam místo je a
sloupec potřebuje záhlaví.

## 2.20 Přepínač reversního chodu: „(invalid parameter)" a proč tam byl

**Hráčovo hlášení, podruhé:** v nastavení hry je na tom řádku pořád
napsáno „invalid parameter".

Příčina, a je moje: řádek nastavení se kreslí jako
`GetString(nadpis, STR_CONFIG_SETTING_VALUE, param1, param2)`, tedy nadpis
dostane hodnotu k doplnění. To funguje u posuvníků a rozbalovacích
seznamů. **U zaškrtávacího nastavení to nepoužívá v celé hře nikdo** —
prošel jsem všechny `SDT_BOOL` a `SDTC_BOOL` ve vanilce a `val_cb` nemá
ani jedno. Stav u nich ukazuje ten přepínač vlevo, a nadpis je prostý
popisek. Já si k tomu vymyslel vlastní `val_cb` a nadpis s `{STRING}`,
a to se nemělo čím doplnit.

Opraveno tak, jak to má celá hra: `val_cb` pryč, nadpis bez `{STRING}`,
stav říká přepínač zapnuto/vypnuto a co která poloha znamená, stojí
v nápovědě pod tím.

**Poučení:** když se nějaká kombinace nastavení nikde ve vanilce
nevyskytuje, není to díra na trhu, je to varování.

## 2.21 Co se doplní samo, musí se samo i odebrat

Přepínač z tématu 2.17 doplňoval reversní chod ve chvíli, kdy hráč zmáčkl
„připojit". Odebrání „připojit" už ale reversní chod nechávalo zamáčknutý,
takže hráči zůstal rozkaz, který vlak na nádraží pro nic za nic otáčí —
a on to nikdy nezadal. Teď se drží obojího směru: zapnutí doplní, vypnutí
odebere. Pořád jen předvyplnění, ne přinucení — čudlík zůstává a hráč si
ho může přemáčknout.

## 2.22 V depu jde vagon připojit i na tu druhou stranu mašinky

Příkaz staví vlečený vůz **za** ten, který dostane, takže puštění na vůz
znamená „před tenhle" a zadává se jménem vozu předchozího. Jenže před
hlavou vlaku žádný předchozí není a vanilka to tam vzdá: puštění vagonu na
mašinku **neudělá vůbec nic, potichu**. Jediná cesta na tenhle konec je
trefit se do prázdna za posledním vozem, což je u krátkého vlaku v depu
proužek. Z hráčovy strany má tedy mašinka jednu stranu, která vagony bere,
a druhou, která je bez vysvětlení odmítá.

Hráč si v depu přerovnává vlastní vlak a smí na obou koncích, takže
puštění na hlavu vlaku připojí **za ni**, místo aby neudělalo nic. Nic se
tím neztrácí — „před hlavu" neexistuje a je to jediný případ, který se
mění.

## 2.19 Rozkaz se posouvá jedině tažením, ne klepnutím

**Hráčovo hlášení:** když kliknu na rozkaz, je bílý a jde posunout; chci
ho posouvat jen tažením, ne tím, že je vybraný — netrefím se na jiný
rozkaz, kliknu mezi řádky a on se posune.

Vybrání rozkazu **natáhne** táhni-a-pusť (`SetObjectToPlaceWnd(..., HT_DRAG)`)
a puštění tlačítka se doručí jako položení tam, kde tlačítko nahoře je —
což není tam, kde šlo dolů, pohnula-li se ruka mezitím o pár bodů. Na
dotykové obrazovce se pohne vždycky. Rozkaz, který měl být jen vybraný, se
tak na hranici řádků zvedl a položil o řádek vedle.

Rozlišuje se to tím, co gesto opravdu bylo: `OnMouseDrag()` běží jen když
je ruka na rozkazu a **hýbe se**, takže si tam příznak `order_dragged`
zapíše, a `OnDragDrop()` bez něj nepřesouvá nic. Tažení funguje beze
změny, klepnutí vybírá a nic víc.

---

# 3. Rozpojování (decouple)

- **Vagonky nejsou k mání, dokud odkladačka stojí vedle nich.** Odpojení
  při příjezdu znamená, že řada existuje, i když odkladačka ještě nestihla
  odjet (červená, ucpané zhlaví). Sběračka poslaná v tu chvíli dojede
  k obsazenému nástupišti, ke konci řady se nedostane, spojit se nesmí
  a umí jedině to vzdát a odjet — hráč to nachytal nastraženou mašinkou
  a poslal první vlaksav log. Pravidlo je zrcadlem „odložený počká, až
  odloživší uhne": řada se nenabízí, dokud odkladačka nestojí aspoň dvě
  políčka daleko (měřeně; jméno odkladačky nese couple_claim, zábor
  sběračky se pozná podle couple_target zpátky). Sběračka mezitím drží
  na svém čekacím místě a log říká proč: „vagonky zatim nejsou k mani -
  nastupiste jeste obsazene masinkou, ktera je odlozila". Rig:
  `testspoj blok` (odkladačka se z nástupiště nikdy nehne — sběračka si
  řadu nesmí nikdy zabrat).
- **Jízdní řád odpojovacího příkazu patří vagonkům, ne mašince.** „Počkat
  8 dní" na příkazu s odpojením znamená: mašinka odpojí a jede hned;
  odložená řada těch 8 dní **nečinně stojí** — nenakládá, nevykládá,
  není cílem spojení (neubírá náklad sousednímu nástupišti) — a teprve
  pak začne práci, kterou jí mašinka nechala. Smysl: mašinka mezitím
  odváží jiné vagonky z vedlejšího peronu, řady se střídají. Okno řady
  po tu dobu píše „Stojí podle jízdního řádu (zbývá N dní)". Hráč čekání
  ukončí Přeskočit na příkazu řady, jako všechno ostatní u řad. Bez
  jízdního řádu se nic nemění. Rig: `testspoj rad`.
- **Odpojuje se při příjezdu, ne při odjezdu.** Mašinka nemá důvod
  prosedět nakládku, která patří vagonkům, co tu nechává — vagonky si
  naloží samy, až odjede (umí to, je to zavedená mechanika). Odpojí se
  hned, jak vlak zastaví v nakládce na stanici, kterou příkaz jmenuje
  (čte se skutečný příkaz, takže mezizastávky při vypnutém non-stop to
  nikdy nespustí); co má naložit/vyložit ponechaná část, to si v klidu
  doodbaví ve zbytku své zastávky — samotná mašinka nemá nic a jede.
  Dřív se odpojovalo při odjezdu (hák v LeaveStation — zrušen, mechanismus
  je jen jeden, v TrainLocoHandleru u ostatní vlakové chirurgie).
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
- **Vlak stojící ZA mašinkou není překážka v cestě.** Po odpojení se
  mašinka ptá o cestu ven a hlídka „v cestě stojí vlak" jí ukazovala na
  vlak, který právě odložila — procházka po rezervaci prohledává celý
  peron, takže našla i vagonky **za jejími zády**, a mašinka to vzdávala
  navždy: stála na peronu i potom, co se trať dávno uvolnila, a jelo se
  s ní jen semaforkem (force). Hráčovo hlášení sedělo přesně („když se
  cesta uvolní, musím ji přinutit; ostatní vlaky tím místem jezdí").
  Rozhoduje se skalárním součinem: kdo stojí za výjezdním koncem, není
  překážka a jde se rovnou hledat čerstvá cesta. Rig: `testspoj vlek
  blok` — mašinka po odpojení drží červenou od nastražené překážky,
  periodicky se ptá znovu, a jakmile překážka uhne, **rozjede se sama**.
- **Rozpojená dvojice, která se od sebe vzdaluje, není srážka.** Obě
  půlky stojí po rozpojení na šířku spřáhla od sebe — a po spojení,
  které se uzavřelo o chlup blíž (nebo ve starším savu), i uvnitř
  vzdálenosti, kterou kolizní kontrola počítá jako náraz. Kdo ty dvě
  jsou, je zapsáno: odložená půlka nese nárok se jménem odkladačky.
  Odjezd od sebe (skalární součin směru jízdy a spojnice) se nechá jet;
  jízda proti sobě bouchá dál, nárok nenárok.

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
- **Odtahovka řekne v okně, proč nikam nejela.** Pět podmínek vyslání
  o sobě nedávalo vědět, takže „ona prostě stojí" bylo všechno, co šlo
  nahlásit. Píše to místo, které rozhoduje, ne druhá kopie těch podmínek
  napsaná jinde — jinak by okno mohlo tvrdit něco jiného, než hra dělá.
  Čtyři odpovědi: *má příkazy* · *jede tam jiná* · *vidí poruchu ale
  nesmí* (něco porouchaného je, ale nepočítá se jako k odvezení — cizí
  firma, v depu, nebo mu vypršela lhůta) · a prosté *čeká na poruchu*
  (nikde není nic porouchaného).
- **Vyslání se dělo v depu a v depu se hned rušilo.** Tohle byla ta
  skutečná příčina toho, že odtahovka nikdy nikam nevyjela, ať bylo
  porouchaných vlaků kolik chtělo. Obsluha „odtahovka stojí v depu
  s úkolem" byla napsaná pro jeden případ — *dovezla porouchanou, je
  hotovo* — a předpokládala, že když má odtahovka cíl a stojí v depu, tak
  se právě vrátila. Jenže cíl se přiděluje **taky v depu**, takže hned
  následující tik viděl slovo od slova ten samý stav a přečetl čerstvý
  úkol jako splněný: zrušil ho, uvolnil porouchanou a nechal odtahovku
  „čekat na poruchu". Znovu a znovu, úkol nikdy nepřežil jeden tik.
  **Ty dva stavy rozlišuje jediná otázka: je porouchaná zapřažená za mnou?**
  Splněný úkol ji má v soupravě, čerstvý ji má pořád venku na trati.
  Druhá půlka téže chyby: obsluha si brala celý tik pro sebe, takže se
  odtahovka nikdy nedostala k místu, které vypouští vlaky z depa. Tik si
  bere jen tehdy, když opravdu něco udělala.
- **Odtahovka bez úkolu musí v depu držet sama sebe.** Být ve službě
  znamená stát v depu s puštěnou brzdou — jenže vlak, který stojí v depu
  s puštěnou brzdou, je jinak vlak, co se chystá vyjet, a hra ho pustí
  ven. Dřív ho tam držela ta samá věc, co rušila čerstvý úkol (viz výš),
  takže když ta odpadla, odtahovka začala vyjíždět bez nehody a psala
  „žádné příkazy". Ty dva případy se teď rozlišují, ne slučují.
- Pojistka: **tři měsíce herního času od poruchy** (ne od vyslání) — stejně
  dlouho jako u havárie, je to jedna lhůta pro obojí. Pak se
  porucha spraví sama a trosky začnou mizet po vanilla způsobu, čímž se
  trať uvolní.
## 4.2 Havárie je stav vlaku, ne jeho konec

Tohle bylo dlouho vedené jako „vrak se z principu nedá pohnout". **Už
neplatí — přepsali jsme, co havárie je.**

Vanilla nasadí havarovanému vlaku příznak `VehState::Crashed` a tím mu vezme
všechno ostatní: od té chvíle nemá směr, nemá cestu a nemá budoucnost. Třese
se, ztrácí po jednom voze a zmizí, a mezitím se s ním nedá dělat nic — což je
přesně problém, protože jediné, co s ním chceme dělat, je odtáhnout ho.

Byl to zároveň zdroj celé rodiny **pádů hry**. Zeptej se takového vozu, kam
míří, a odpověď je „nikam"; každé místo, které z té odpovědi dělá kolej, na
tom spadne (`track_func.h:237`). Ve vanille se nikdo neptá, protože vrak je
věc odepsaná. U nás si ho odtahovka připojí k sobě a odjede s ním, takže se
ptá spousta míst.

**Takže havarovaný vlak žádný `VehState::Crashed` nedostane.** Dostane
`VehicleFlag::Wreck` a zůstane obyčejným vlakem:

- zastaví, kde stojí, a **zešedne** (`PALETTE_CRASH`, ptá se `IsWrecked()`),
- v okně píše **havárie**,
- **hráč s ním nemůže odjet** — příkaz start ho odmítne. Kouknout se do něj
  a zkopírovat z něj příkazy jde.
- **čeká na odtah**, přesně jako porouchaný,
- výbuch na obou koncích a **kouř kolem** — jeden obláček na každý vůz, až
  tři dlaždice od vlaku. Vanilla to rozpouští do stovek tiků, my musíme
  všechno naráz, jinak je z havárie jen vlak, co potichu zešedl.
- noviny s počtem mrtvých jsou beze změny,
- **po třech měsících**, když nikdo nepřijede, zmizí. Ne po vozech —
  odtahovka má přijet k celému vlaku, ne k tomu, co z něj zbylo.

`HandleCrashedTrain()` a `DeleteLastWagon()` tím pádem pro naše vraky
neběží vůbec.

## 4.1 Dvě velké věci z návrhu, které se nakonec dělat nemusely

Stojí za zapamatování, protože v návrhovém dokumentu jsou pořád popsané
jako nutná práce, a nejsou.

**Rezervace proti jednosměrnému návěstidlu.** Návrh počítal s tím, že
odtahovka se musí vrátit tou samou tratí, po které přijela — do svého
domovského depa — a proti jednosměrkám to nejde. Řešit se to mělo
rozšířením rezervačního systému. **Odpadlo tím, že se nevrací:** vlak
s poruchou jede do **nejbližšího** depa, protože smysl odtahu je dostat
ho z trati, a nejbližší cesta z trati je ta nejlepší. Domovské depo si
odtahovka hledá až potom, sama a prázdná.

**Vlastní příznak „tenhle vůz je vizuálně otočený".** Návrh ho chtěl kvůli
tomu, aby spojená dvojice vypadala jako souprava s mašinkou na obou
koncích. **Odpadlo přechodem na beta 16**, kde je skutečná jízda
pozpátku. Zároveň tím zmizelo riziko chyby A ze starého patche — příznak,
který měl ovlivnit jen vykreslení, tam přepisoval skutečný směr jízdy.

## 4.3 Odtah na výhybkách — odtahovka si vrak narovná

**Co hráč hlásil.** Assert `IsValidDiagDirection(exitdir)` (train_cmd.cpp,
vagon hledá předchůdce a ten není na sousedním políčku): odtahovka se na
výhybkách nemá jak dostat k vraku před čumák, spojení se přesto uzavřelo
„na blízkost" a vznikl vlak s dírou ohnutou přes křižovatku. Hráčův návrh:
odtahovka vyprošťuje, tak ať si vrak narovná. Návrh je správně — na
výhybce žádné ježdění najetí neumožní.

**Tři pravidla, která z toho vzešla:**

1. **Spojení se uzavře jen na čistých koncích.** Čistý konec = oba konce
   na rovných kolejích (X/Y) a po kolejích na sebe navazují (měří se
   krátkou obchůzkou po kolejích, `AreCoupleEndsRailConnected`; peron se
   počítá celý, protože sledovač kolejí ho přeskakuje jedním krokem).
   Nádražní spojování se nemění — perony jsou rovné z principu. Nečisté
   obyčejné spojení se prostě neprovede (vlak stojí dál); nečistý odtah
   jde na narovnání.

2. **Narovnání (`LayCasualtyAlongTow`), jen pro odtah.** Vrak se zvedne
   **vcelku** a položí se na kolej před odtahovku, vůz za vozem po
   políčkách; dotahovací krok ho pak přitáhne. Napřed se celá pokládka
   naplánuje — cizí vlak v cestě, tunel/most nebo konec koleje ji odmítne
   a pak se nestane vůbec nic, vrak leží dál. Opuštěná políčka se uklidí
   jako při vyjetí vlaku (rezervace, přejezd, návěstidla). Na rovině se
   nenarovnává — tam platí normální najetí jako dřív.

3. **Kasualita nedrží trať.** Porouchaný vlak si nechával zarezervovanou
   cestu, kterou už nikdy nepojede — a odtahovka se přes ni k němu
   nedokázala dostat (nemohla si trasu zabrat, stála v depu do vypršení
   lhůty). Ve chvíli, kdy vlak začne čekat na odtah, se mu cesta uvolní
   a drží jen zem pod sebou — stejné pravidlo jako u odložených vlaků.

**Rig:** `testodtah` postaví scénu (porucha ohnutá přes výhybku do jižní
odbočky, odtahovka na zavolání v západním depu), `testodtah rovina` je
kontrola na rovné trati (narovnání se nesmí spustit), `testodtah krizeni`
rozbije kasualitu přímo na políčku křižovatky. Obě ověřeny: spojení
bez pádu, odvoz do depa, složení vraku; regrese nádražního protokolu na
save91 4/4 s reverzem i bez.

**Zůstatek rezervace po odtahu (hráčovo hlášení, opraveno).** Úklid
políček opuštěných narovnáním přeskakoval **celé políčko**, když na něm
kdokoli stál — jenže na křižovatce může odtahovka stát na rovném kusu,
zatímco se vrak zvedá z oblouku, a rezervace oblouku pak zůstala viset
navěky (zarezervovaný kus trati, na kterém nic není a nic ji nikdy
nepustí). Přeskakuje se teď jen, když někdo stojí **na tomtéž kusu
koleje**. Chyceno `testodtah krizeni` + výpisem `testmapa` po odtahu
(`testza <tiky> <příkaz>` — odložené spuštění konzolového příkazu);
po opravě zbývají po odtahu jen rezervace pod živými vlaky.

---

## 4.4 Odtahovka jede proti návěstidlům — ale zaplatí za to celou cestou

**Hráčovo hlášení:** odtahovka marně hledá cestu, i když porouchaná stojí
kousek před jejím depem; když ji přinutí vyjet, jezdí okolo poruchy.
Reprodukováno v rigu (`testodtah jednosmer`) — jednosměrná návěstidla na
hlavní trati a odtahovka dělá tohle donekonečna:

```
Vlak 2: vyjizdi z depa (82,11) - rezervace konci na (83,11)
Vlak 2: zmenen vedouci konec - konec koleje (pri jizde)
Vlak 2: vjel do depa (82,11)
```

Vyjede o jedno políčko, jednosměrné návěstidlo před ní je pro hledání
cesty **konec koleje**, otočí se, vjede zpátky a znova. Porouchané
mezitím vyprší lhůta a odjede sama.

**Proč to musí jít proti návěstidlům.** Porouchaný vlak stojí tam, kde
stojí, a **za ním se staví fronta**. Volno bude jedině **před ním** —
až odjedou vlaky, které jely před ním. Na trati provozované jedním
směrem to znamená, že odtahovka k porouchanému může jen **proti provozu**,
zepředu, čelem k němu. Jinak k němu nikdy nedojede.

**Čím se to platí.** Není to výjimka nalepená na to, aby odtahovka
projela tam, kam obyčejný vlak nesmí. Je to druhá půlka téhož pravidla:
jede proti návěstidlům, a **za to si musí zamluvit celou cestu, než se
vůbec pohne, a cestou nikde nezastaví.** Zastavit se v půlce, u návěsti,
čelem proti provozu, je to jediné, co nikdy udělat nesmí.

Provedeno na pěti místech, všechna říkají totéž:

- `IsSafeWaitingPosition()` (pbs.cpp) — pro odtahovku na výjezdu je
  jediné bezpečné místo k zastavení **až u porouchané**. Tím se
  rezervace stává **všechno, nebo nic**: buď se zamluví celá cesta od
  vrat depa k nosu porouchané, nebo odtahovka nevyjede vůbec.
- `FollowTrainReservation()` (pbs.cpp) — sledování **vlastní** rezervace
  musí umět jít stejnou cestou, jinak si odtahovka přečte svou rezervaci
  jako končící u prvního návěstidla a začne zamlouvat znovu.
- `ExtendTrainReservation()` (train_cmd.cpp) — návěstidlo proti ní není
  zeď.
- `FreeTrainTrackReservation()` (train_cmd.cpp) — **pouštění musí umět
  projít tudy taky.** Jinak by za prvním protilehlým návěstidlem zůstala
  trať zamluvená vlaku, který už nejede — držená napořád, nikým.
- `SignalCost()` (yapf_costrail.hpp) — hledání cesty to nesmí prohlásit
  za slepou uličku.

**A ještě jedno, které se ukázalo až po prvních čtyřech:** rezervace už
vedla až k porouchané, a odtahovka se přesto u prvního návěstidla
otočila — tentokrát ji zastavil **běh hry**, ne hledání cesty
(`red_signals` v `TrainController`, hláška `krok duvod: cervena na …`).
Doplněno tam, ale úzce: odtahovka projede návěstidlo **jen po koleji,
kterou už má zamluvenou**. Červená na koleji, kterou nedrží, ji zastaví
jako kohokoli jiného — z tohohle se nesmí stát povolení jezdit na
červenou.

Ověřeno: `testodtah jednosmer` — odtahovka vyjede, zamluví si cestu až
k porouchané, projede proti návěstidlům, dojede k ní **zepředu**, spojí
se a odtáhne ji do depa; porouchaná pak jede dál po svých. Ostatní scény
(21 celkem) beze změny.

### „Všechno, nebo nic" je rozhodnuté a nepředělává se

Navrhl jsem otevřít otázku, jestli by odtahovka na vytížené hlavní trati
neměla smět zastavit i na holé trati proti provozu — celá cesta naráz se
tam totiž zamluvit skoro nedá. **Hráč to zamítl a rozhodnutí je staré:
celá trať až k poruše je jenom její.** Odtahovka počká v depu, dokud
odjedou vlaky, které jely před poruchou; pak je před poruchou volno, celá
cesta se zamluví a odtahovka vyjede. Znovu se to nezvedá.

Čekání v depu tomu odpovídá a je nekonečné jen zdánlivě: `CheckTrainStay-
InDepot()` zkusí zamluvit cestu znovu každých 37 tiků, pořád dokola, a
okno odtahovky mezitím říká „Má výjezd, nenachází cestu k případu"
(`RescueHold::NoPath`). Strop je jediný, a je to strop porouchaného vlaku,
ne odtahovky: `RESCUE_DEADLINE_DAYS` (čtvrt roku). Když se do té doby
trať neuvolní, porouchaný to vzdá a spraví se po vanilkovém, odtahovka
zruší výjezd a vrátí se do pohotovosti. Zaseknout se tím nedá nic.

---

## 4.5 Po spojení vede odtahovka, ne porouchaný vlak

**Hráčovo hlášení:** odtahovka dojela správně, zepředu, spojila se —
**a zůstala stát.** Za porouchanou stál další vlak, takže tam nemohla.
Hráč jí v okně dal otočit a jela.

Po každém spojení se nastavuje „vede zadek", a spojka věší partnera na
konec seznamu — takže vede **to, co bylo sebráno**. Pro sběračku
s vagonky je to správné pravidlo: pokračuje tam, kam jela, protože tam
ty vagonky patří.

**Pro odtah je to obráceně, a ze stejného důvodu čtenýho naopak.**
Odtahovka nic nedoručuje. Jela proti provozu nahoru za vlakem, který
zůstal stát, a celý její úkol je dostat ho z trati **pryč**. Pokračovat
dál znamená tlačit porouchanou před sebou přesně do provozu, který se za
poruchou nakupil — kam nemá co dělat a často se tam ani nedostane.

Takže **vede odtahovka**. Spojka věší porouchanou na konec a srovnání
natočení nechává nos hlavy mířit pryč od těla, takže „vede hlava" je
strukturálně „zpátky tou cestou, kterou odtahovka přijela" — tou, kterou
si zamluvila a o které jako jediné ví, že je volná. A je to i to, co
lokomotiva s mrtvým vlakem dělá: **táhne ho**, netlačí ho před sebou
proti provozu. Otáčet se přitom nemusí; je otočená správně už ve chvíli
spojení.

Naměřeno v `testodtah jednosmer`: dřív `spojeno - couva ano` a vlak
odjel dál na východ, teď `spojeno - couva ne, celo: masinka` a odtáhne
porouchanou zpátky na západ do depa, ze kterého vyjela.

## 4.6 Cesta tam a cesta zpátky nejsou tatáž jízda

**Hráčovo hlášení:** odtahovka si přestala rezervovat trať až k porouchané,
vyjede a někde zůstane stát na semaforku.

Moje chyba z tématu 4.4. Pravidlo „jediné bezpečné místo k zastavení je
až u porouchané" jsem přivázal na `IsOnRescueRun()`, jenže to platí
**i po spojení** — dokud se výjezd neuzavře v depu. Cesta domů tím spadla
pod stejný zákaz: jediné, co se počítalo za bezpečné, byla porouchaná,
kterou už měla za sebou. Na trati, kde cesta domů není volná celá naráz,
si tedy nezamluvila **nic** a zůstala stát u prvního návěstidla natrvalo.

Cesta tam a cesta zpátky nejsou tatáž jízda a nesmí dostat stejná
pravidla. Tam jede proti návěstidlům a platí za to celou cestou. Zpátky
je to obyčejný vlak s dlouhým nákladem, který jede do depa obyčejnou
cestou a u návěstidel čeká jako každý jiný. Rozlišuje to
`IsFetchingCasualty()` — je to výjezd **a** porouchaná ještě není součástí
téhle soupravy — a všech pět míst z tématu 4.4 plus výjimka z červené
v `TrainController` se ptají jeho, ne `IsOnRescueRun()`.

Scéna `testodtah daleko`: porouchaná až na druhém konci pásu, odtahovka
si zamluví 35 políček přes všechna čtyři protilehlá návěstidla, spojí se
a dotáhne ji domů.

## 4.7 Spojit se s poruchou není totéž co ji odvézt

**Hráčovo hlášení:** už jela pěkně, ale neodtáhla tu poruchu, zůstala tam
stát.

Rozkaz „jeď do nejbližšího depa" dostane odtahovka ve chvíli spojení a
jeho **výsledek se zahazoval**. Přitom v té chvíli může klidně selhat:
odtahovka stojí na úseku, ze kterého se právě teď do žádného depa dostat
nedá — cestu domů drží fronta, která se za poruchou nakupila, nebo je
zpátky jen skrz jednosměrné návěstidlo, které už porušit nesmí, protože
zpátky je z ní obyčejný vlak (téma 4.6).

Zeptat se jednou a nechat to být znamená, že tam stojí **navždy**. Vlastní
rozkazy odtahovka mít nesmí, takže ji nic jiného nikdy nerozjede, a
porouchaná už na nikoho nečeká — je připojená — takže nikdo další pro ni
nepřijede. Z hlediska hry se ta porucha z mapy nikdy neztratí.

Opraveno tak, že se odtahovka ptá dál: `TrainLocoHandler()` se zeptá
znovu vždycky po 64 ticích, dokud rozkaz neprojde. Ne každý tik — jedno
odmítnutí stojí celý běh hledače cest a to, na co se čeká (uvolnění
trati), trvá o hodně déle než jeden tik.

A okno odtahovky to mezitím řekne: `RescueHold::NoDepot`, „Připojeno, ale
žádné depo v dosahu". Zvenčí totiž vypadá vlak, který se spojil a nemá
kam, úplně stejně jako vlak, který se právě chystá vyjet — a „ono to tam
jen stojí" je jinak všechno, co se o tom dá říct.

### Rig na to neuměl přijít, protože měřil jen půlku

Všechny čtyři odtahové scény končily měření **u spojení** a to
prohlásily za úspěch. Odtahovka, která se spojila a pak do konce hry
stála na trati, jim vycházela zeleně. Přesně tu chybu pak našel hráč.

Napraveno dvakrát:

* `HandleRescueEngineInDepot()` říká nahlas „odtah dokončen — porucha
  složena v depu na (x,y)" a všechny odtahové scény čekají na **tuhle**
  větu, ne na „spojeno".
* Nová scéna `odtahbezdepa` tu poruchu staví schválně: `testdepo pryc`
  zbourá domovské depo ve chvíli, kdy je odtahovka venku, `testdepo zpet`
  ho o 8000 tiků později postaví zpátky. Odtahovka se spojí, nemá kam,
  řekne to — a jakmile je depo zpátky, **sama** se rozjede a poruchu
  doveze. Testuje se tím to podstatné: ne že se zastaví, ale že se zase
  rozjede.

Poznámka k rigu: `Command<>::Do` **nepromazává** frontu návěstidel — to
dělá až cesta, kterou jdou hráčovy kliky. Bourání a stavění z heartbeatu
(`testza`) tedy nechalo frontu stát a první pohnuvší se vlak spadl na
`assert(_globset.IsEmpty())`. `testdepo` si po sobě volá
`UpdateSignalsInBuffer()` samo.

## 4.8 Odtahovka na rozcestí — rig konečně staví hráčovu trať

**Hráčův popis jeho tratě:** z depa vede cesta doprava na okruh a tudy
odtahovka přijede **za** vlak, který stojí za poruchou — ten je k ní
otočený zadkem a za ním trať zamluvená není, jsou to PBS návěstidla.
Nebo může doleva, proti jednosměrným návěstidlům; tam jsou ty výhybky,
jedna vpravo, jedna vlevo.

To je tvar, který žádná dosavadní scéna neuměla: **odtahovka má na
vybranou.** Nová scéna `testokruh` ho staví — hlavní trať, z ní okruh
dolů a zpátky, jednosměrná návěstidla jen na krátké cestě, porouchaná
až za místem, kde se okruh vrací. Obě cesty k ní vedou, takže se neměří
nic než **kterou si vybere**.

### Co se tím našlo

**1. Pokuta za jízdu zezadu do PBS návěstidla platila i pro odtahovku.**
`rail_pbs_signal_back_penalty` je 15 políček za každé takové návěstidlo.
Krátká cesta proti třem z nich byla tím dražší než objížďka kolem, takže
hledání cest posílalo odtahovku dokola — hráčovo první hlášení znělo
přesně takhle: „když ji přinutím vyjet, jezdí okolo poruchy". Zrušil jsem
větu z tématu 4.4 jen napůl: zákaz (`DeadEnd`) jsem sundal, pokutu nechal
stát. Je to jedno pravidlo, ne dvě — jízda proti provozu je způsob, jak se
k stojícímu vlaku dostat, a platí se za ni zamluvením celé cesty předem.
Účtovat ji podruhé znamená jen koupit špatnou cestu. Zpátky je odtahovka
obyčejný vlak a platí jako každý.

**2. Nedořešeno: odtahovka nevyjede, jakmile je na cestě jakékoli
rozcestí.** Scéna to ukazuje na dvou nezávislých věcech — okruh
(`testokruh`) i nástupiště na cestě (`testokruh rovne`) — a bez obojího
(`testokruh rovne bezstanice`) jede. Hlášení je pořád stejné: „cesta není
— hledání/rezervace selhaly", donekonečna, z depa. Zamluvené přitom není
nic než dvě políčka, na kterých porouchaná stojí (`testrez`).

Kde to je: `ExtendTrainReservation()` projde **jen jeden běh koleje bez
odbočky**. Na rovné trati dojde od vrat depa až k porouchané a celá
rezervace se povede, aniž by se kdo ptal hledače cest — a to je jediný
důvod, proč všech pět dosavadních odtahových scén procházelo. Jakmile je
na cestě rozcestí (nebo nástupiště), předá se to hledači cest — a ten
vrátí `res_dest.tile == INVALID_TILE`, tedy **cíl nenašel**. Odtahovka
tedy zřejmě hledačem cest nikdy úspěšně neprojela; fungovalo jen to, co
šlo obejít.

### Změřeno, ne odhadnuto: hledání cíl nenajde

Doplněná hláška v `ChooseTrainTrack()` to říká rovnou:

```
Vlak 2: hledani z (18,44) na cil (42,44) - CIL NENALEZEN, konec nikde
```

Takže to není rezervace, která by selhala na obsazené trati — **hledání
cest se na políčko porouchané vůbec nedostane.** Tím padá celá řada
vysvětlení, která začínala u toho, kdo co drží.

### Co se ukázalo jako slepá ulička

Napadlo mě, že vinu má **sdílená paměť úseků** v YAPFu: odpovědi
„jednosměrné návěstidlo je slepá ulička" a „za jízdu zezadu do návěstidla
se platí" se ukládají do mezipaměti, která je společná pro všechny vlaky a
maže se jen při změně kolejí — o to, *kdo* se ptá, se nestará. Na trati,
kterou už projely obyčejné vlaky, by tedy odtahovka dostala jejich
odpověď.

Úvaha platí a je to skutečná chyba: kdo udělá uloženou odpověď závislou na
tazateli, nesmí do té mezipaměti. Proto tam `IsFetchingCasualty()` teď
`DisableCache(true)` volá, stejnou cestou, jakou to už dělá případ složitých
směrových bodů. **Ale tuhle chybu to neopravilo** — scéna padá dál. Zapsáno
jako oprava jiné, latentní chyby, ne jako řešení téhle.

Příští krok je dostat hlášku dovnitř `ChooseRailTrack()` a zjistit, kam až
nejlepší uzel došel — jestli se hledání zastaví u prvního návěstidla,
u nástupiště, nebo někde jinde.

Pomůcky k tomu, obojí zůstává v rigu: `testrez` vypíše, kdo ve scéně co
drží, a `ChooseTrainTrack()` teď při vzdání v depu řekne, **kterým** ze
tří konců to bylo.

---

# 5. Myš, kurzor, stavba

## 5.0 Co je „naše" nastavení posunu mapy

V nabídce je **pět** voleb. První je naše a je přednastavená. Je to
**to, co má hra sama na Windows přednastavené — jen bez zasekávání
tlačítka.** Nic víc a nic jinýho.

Pod ní jsou **všechny čtyři vanilkové volby beze změny**, včetně toho
zasekávání: jsou v seznamu proto, aby se dalo srovnávat, a volba
pojmenovaná jako hry se musí chovat jako hra. Proto je oprava zasekávání
(`EndViewportScrollIfLetGo`) zapnutá **jen pro naši volbu**. To je celý
rozdíl mezi ní a vanilkovou.

**Pozor: vanilla má dva výchozí stavy.** V `gui_settings.ini` jsou dva
bloky téhož nastavení:

- `ifdef UNIX` → `MapRMB` (mapa jede, **kurzor se hýbe**),
- `ifndef UNIX`, tedy Windows → `ViewportRMBFixed` (jede pohled,
  **poloha myši uzamčena**).

Hraje se na Windows, takže „jako vanilla" znamená **uzamčený kurzor
a pohyb pohledu**. Já se podíval jen na první blok, prohlásil jsem opak
a spletl se; správně to řekl uživatel.

**Uzamčený kurzor znamená, že hra ukazatel každý snímek vrací zpátky.**
To vracení je právě to, čím ukazatel doopravdy stojí — ne to, že se
nakreslený kurzor nehýbe.

**Třikrát jsem to zkazil tím, že jsem si k tomu přimyslel víc, než bylo
domluvené:**

1. **Zamrazil jsem kurzor a zároveň vypnul vracení ukazatele.** Nakreslený
   kurzor pak stál, ale skutečný ukazatel Windows utíkal pryč a nebyl
   vidět; po puštění tlačítka byl někde jinde.
2. **Pak jsem „opravoval", že mapa jezdí moc rychle**, a přepsal počítání
   pohybu na „proti minulému snímku". To je správně jen tam, kde se
   ukazatel nevrací. Kde se vrací, vyrobí to vracení samo stejně velký
   pohyb na opačnou stranu — takže tím byly rozbité **i všechny vanilkové
   volby** a nešly testovat.
3. **Pak jsem naši volbu udělal s volným kurzorem**, protože jsem si
   spletl výchozí stav.

`gfx.cpp`, `gfx_type.h` i `smallmap_gui.cpp` jsou zase **bajt po bajtu
vanilkové** a ve `window.cpp` je v posunu mapy jediná přidaná věc to
volání navíc; zbytek jsou dvě podmínky rozšířené o naši volbu.

**Poučení, ne technické:** když se řekne „jako vanilla, jen bez téhle
jedné chyby", tak se **nesmí přidat nic jiného** — a napřed se zjistí, co
vanilla na téhle platformě doopravdy dělá, ne co dělá na první, na kterou
padne oko.

## 5.0b Tažení končí tím, že se mapa přestane hýbat

Nápad hráče a je lepší než všechno, co jsem zkoušel předtím, protože se
**neptá tlačítka**. Každý jiný způsob, jak tažení ukončit, musí věřit tomu,
co si hra myslí o tlačítku — a to je právě ta jediná věc, o které víme, že
je špatně: čte se jako držené, když ho nikdo nedrží, a tažení přežije ruku.

Mapa takhle lhát neumí. Dokud tažení opravdu běží, ukazatel se hýbe — to
*je* tažení. Když se zastaví, buď ruka pustila, nebo se zastavila, a v obou
případech se v tu chvíli nic netáhne.

Proto se tažení ukončí po **vteřině bez pohybu**. Nestojí to nic
viditelného: dalším stiskem začne hned nové, a na tom druhu ovládání, kde
to zlobí — prst drží tlačítko na obrazovce a druhý táhne mapu — je tlačítko
pořád dole, takže další pohyb prostě pokračuje. Co se tím získá: **zaseknuté
tlačítko nemůže mapu držet déle než tu vteřinu.**

Stopka se nuluje uvnitř téže funkce, ne tam, kde tažení začíná — jinak by
každé nové tažení skončilo na snímku, ve kterém začalo, a zapomenout na to
by šlo znovu.

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

**Ta jejich větev je z prosince 2020**, tedy z doby OpenTTD 1.10/12.0. Od
té doby se v OpenTTD přepsal celý systém příkazů, takže se ten kód dnes
ani nepřeloží. Není to „chybí pár drobností" — je to jiný jazyk. Bere se
z toho tedy **co dělají a jak to vypadá**, ne řádky.

Kde se u nich dá koukat: `order_gui.cpp`, `order_cmd.cpp`, `train_cmd.cpp`,
`pathfinder/yapf/yapf_rail.cpp`, `lang/english.txt`.

Nejcennější jsou jejich vlastní commity, které začínají „Fix: crash..." —
autor v nich sám pojmenoval, co opravoval. Odtud je těch šest pravidel
v kapitole 11.

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

## 10.2 Vlastní ikonky: `grfcodec` v kontejneru je

Dlouho jsem si myslel, že vlastní sprity přidat nejde. Šlo to celou dobu:
**`apt-get install grfcodec`** (balík je i s `nforenum`). Je to obyčejný
balík distribuce, jen jsem se na něj nepodíval.

Jak se ikonka přidá:

1. obrázek do `media/baseset/openttd/`, **8bpp v DOS paletě**, průhledno
   je index 0,
2. řádek do `media/baseset/openttd/openttdgui.nfo` — `-1 sprites/<jméno>.png
   8bpp <x> <y> <š> <v> 0 0 normal` — a o jedna výš číslo v hlavičce
   (`05 15 \b <počet>`),
3. jméno souboru do `media/baseset/openttd/CMakeLists.txt`,
4. `OPENTTD_SPRITE_COUNT` v `src/table/sprites.h` o jedna výš a nová
   konstanta `SPR_...  = SPR_OPENTTD_BASE + <index>`,
5. přeložit — CMake si `grfcodec` najde sám a `openttd.grf` i
   `openttd.grf.hash` přepíše **ve zdrojovém stromu**. Obojí se commituje,
   protože **CI `grfcodec` nemá** a bere hotový soubor z gitu.

Kontrola, že to vyšlo: `grfcodec -d openttd.grf` a v `sprites/openttd.nfo`
se podívat na blok `05 15`.

**Ověřeno, že `grfcodec` reprodukuje původní soubory bajt po bajtu** —
`orig_extra.grf` se po přegenerování nezměnil ani o bit. Přegenerování
tedy nic nerozbije.

### Co udělat s obrázkem, než se z něj stane sprite

Ikonka z icons8 je 16 × 16 PNG s průhledností a **vyhlazenými okraji**.
Ani jedno ve spritu nepřežije a naslepo převedená vypadá hrozně:

- **Poloprůhledné pixely se musí useknout.** Hra nezná „napůl"; nechá je
  plné, takže je z vyhlazeného okraje špinavý lem.
- **Barvy se musí přichytit jen na šedou a dřevěnou řadu palety.** DOS
  paleta má málo neutrálních šedí, takže když se převod nechá vybírat
  volně, měkký šedý okraj skončí na nejbližší **modré** a celá ikonka má
  fialový lem. Tohle byl ten viditelný problém.
- **Kolem tvaru patří tmavá obrysová linka**, jak ji má každá ikonka ve
  hře. Bez ní se nářadí ztratí v šedi tlačítka, na kterém sedí.

Dělá to `media/baseset/openttd/openttdgui_rescue.py`, takže se to dá
zopakovat i pro další ikonku.

**16 × 16 je správná velikost** — tlačítko v okně vozidla je 18 × 18
a lem si vezme po pixelu z každé strany.

---

# 11. Pasti v enginu

- **Kdo zrovna jedná, se musí říct nahlas.** `_current_company` není
  „vlastník vlaku, který se právě tiká" — je to prostě to, co běželo
  naposledy, a v tiku vozidla to bývá **nikdo** (`OWNER_NONE`, číslo 16).
  Kdo z tiku sáhne na cokoliv, co se na tu proměnnou ptá, položí hru:
  přidělení čísla nové soupravě při rozpojení jde přes
  `GetFreeUnitNumber()`, ta se ptá na `Company::Get(_current_company)`,
  a seznam firem na „nikoho" odpoví pádem
  (`Company pool: asked for index 16`). Stejná past dvakrát: nejdřív
  u spojování (příkaz tiše propadl na kontrole vlastníka), pak
  u rozpojování (pád). Léčí se jedním řádkem
  `AutoRestoreBackup cur_company(_current_company, v->owner);` na začátku
  každé funkce, která z tiku mění soupravu.
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

## 11.1 Vzor, na kterém stojí spojování

Vanilla ho má odjakživa v `CmdMoveRailVehicle` — to je přetahování vagonků
v okně depa:

1. zálohovat **oba** vlaky (`MakeTrainBackup`),
2. přeuspořádat řetězce (`ArrangeTrains`),
3. ověřit výsledek (`ValidateTrains`),
4. při chybě obnovit **oba**, teprve při úspěchu provést viditelné
   následky.

`TryConsistSplice()` je tenhle vzor vytažený ven, aby ho mohlo použít i
spojování na trati, rozpojování a odtah. Rozdíl proti depu je jediný: depo
vyžaduje `IsStoppedInDepot()`, na trati to nahrazuje blízkost, nulová
rychlost a orientace.

## 11.2 Šest pravidel, která vyplynula ze starého patche

Rozbor je ve `FEATURE_DESIGN_COUPLING_TOW.md`, „Zásadní zjištění č. 3".
Tady jsou závěry, protože platí pro cokoliv nového:

- **Vykreslování nesmí měnit herní stav.** Starý patch přepisoval
  `this->direction` uvnitř funkce, která má jen spočítat obálku pro sprite.
  Cokoliv, co ji zavolalo, otočilo vlaku skutečný směr jízdy. Odpovídá to
  hlášeným „výbuchům". Co jen počítá odvozenou hodnotu, bere vozidlo přes
  `const*`, ať to hlídá překladač.
- **Jedno sdílené primitivum, ne čtyři kopie.** Spojení, rozpojení,
  připojení odtahu a jeho odpojení mají jednu cestu kódu
  (`TryConsistSplice`), která vždycky zálohuje **obě** strany, vždycky
  validuje před zápisem a vždycky se symetricky vrátí. Starý patch měl
  couple pečlivý a decouple ledabylý — v jednom souboru.
- **Sousednost se čte z topologie, ne z pixelů.** Porovnávat pixelovou
  vzdálenost na přesnou rovnost selže na oblouku, u článkových vozidel
  nebo o jeden pixel. Dlaždice a trackdir jsou diskrétní.
- **Žádné sdílené bity mezi různými typy příkazů.** Starý patch četl ten
  samý rozsah bitů jako „kolik vozů odpojit" i „kolik připojit". Po změně
  typu příkazu se pak četlo smetí jako platná hodnota. Pole příkazu jsou
  vlastní členy.
- **Odhad se hráči ukáže, nikdy se nepoužije potichu.**
- **Nic nevratného před validací.** Rezervace trati není součástí zálohy
  vozidel, takže se nedá vrátit. Řeší se to pořadím: nejdřív jen čtení a
  kontrola, teprve po ní zápis. Pak není co vracet.

---

# 12. Blueprint (Spaceone)

## 12.1 Odkud je a co s ním smíme

Patch **Blueprint** pro OpenTTD 15.3.

- Autor: **Spaceone**, na GitHubu **age77**
- Zdroj: <https://github.com/age77/openttd-15.3-blueprint>
- Licence: **GPL v2**, stejná jako má sama hra. Píše to jeho `README.md`
  („GPL v2, the same as OpenTTD itself") a každý přidaný zdrojový soubor
  nese obvyklou hlavičku OpenTTD. `patch/blueprint.patch` je podle autora
  úplný odpovídající zdroj.

**Smíme ho použít**, když zůstanou hlavičky a autorství. Jméno autora patří
do hlaviček přebraných souborů a sem.

Soubor s patchem už v repozitáři není — po přenesení se smazal, jak bylo
domluveno. Odkaz nahoře je to, co zůstalo.

## 12.2 Proč nahradil moji verzi kopírování

Moje `copypaste_gui.cpp` uměla kopírovat jen v rámci jedné rozehrané hry.
Chtěné bylo přenést plochu **do jiné hry**, tedy uložit ji do souboru. To
Blueprint umí, a k tomu spoustu dalšího, takže moje verze je pryč a nic
z ní nezbylo.

Co je ve hře teď:

- **8 přihrádek** (`NUM_BLUEPRINT_SLOTS`), každá se dá pojmenovat
  (dvojklik na číslo),
- plocha až **255 × 255** (`MAX_BLUEPRINT_DIMENSION`),
- **otáčení a zrcadlení** — čtyři otočení po 90° a dvě osy zrcadlení,
  správně se přitom otáčí i koleje, návěstidla, výhybky depa a silnice,
- posun výšky **±8** a tři režimy úprav terénu: žádné, jen pod stavbami,
  celá plocha (`BlueprintTerraformMode`),
- **soubor** ve složce `blueprint` vedle `openttd.cfg` — uloží se všech
  osm přihrádek najednou (formát `OTTD-BPSET-1;` a base64); jedna
  přihrádka jde i přes schránku jako `OTTD-BP-1;` + base64,
- **náhled v mapě** ještě před klepnutím: koleje se kreslí jako čáry,
  ostatní stavby jako obdélníky, a co se na dané místo nevejde, svítí
  červeně,
- kopíruje se **železnice, návěstidla, silnice a tramvaje, depa, kanály,
  zdymadla, loděnice, mosty a tunely, nádraží a stanoviště, přístavy,
  bóje a letiště** — všechno, co patří hráči.

Vkládání jsou pořád **obyčejné stavební příkazy jeden po druhém**, přesně
jako by to hráč kladl ručně: stojí to, co to stojí, ctí to vlastnictví,
odmítne to, co terén neunese, a funguje to i ve hře více hráčů. Co se
nevejde, se přeskočí — nevypíše se u toho chyba za chybou.

## 12.3 Co bylo potřeba přepsat při přenosu z 15.3

Patch je proti 15.3 a mezi ní a beta 16 se hodně přejmenovalo. Nešlo tedy
nic „přiložit", muselo se to přepsat. Skoro všechno bylo mechanické:

- výčtové typy jsou dnes `enum class` (`Track::X` místo `TRACK_X`,
  `TileType::Railway` místo `MP_RAILWAY`, `Commands::BuildRail` místo
  `CMD_BUILD_SINGLE_RAIL`, a tak dokola),
- `TrackBits` a `RoadBits` jsou dnes bitové množiny, ne čísla — takže se
  nedají sčítat s příznakovým bitem náhledu; ten se nastavuje zvlášť,
- pole indexovaná výčtem potřebují `TrackIndexArray` nebo `to_underlying`,
- **příkazy na most a tunel dostávají druh koleje a druh silnice zvlášť**,
  místo jednoho společného čísla.

Dvě věci z původního patche se **nepřebíraly schválně**: přejmenování
titulku hlavního menu a titulku okna hry na „Blueprint". To je značka jeho
větve, ne naše.

## 12.4 Ikonky

**Ikonky jsou skutečné, ne vypůjčené** — 16 vlastních spritů 20 × 20.
Napoprvé se to povedlo i bez `grfcodec`: sprity jsou uložené hotové
v `media/baseset/openttd.grf` a ten soubor je v beta 16 **bajt po bajtu
stejný** jako v 15.3, takže binární část patche na něj sedla přesně tak,
jak byla napsaná. Od té doby je `grfcodec` v kontejneru (viz 10.2) a
sprity se dají přidávat normálně.

## 12.5 Kde to je

**V nabídce úprav krajiny na horní liště**, hned pod „Vysadit stromy",
a nikde jinde. Vlastní ikonka na horní liště i tlačítko v liště úprav
krajiny byly zkusmo taky; obojí je pryč, obě lišty jsou vanilkové.

Položka v nabídce má **číslo 4**, i když je v seznamu druhá. Kam se
v nabídce vykreslí, určuje pořadí přidávání; číslo je jen to, čím se
odpovídá na kliknutí. Přečíslovat kvůli jednomu řádku čtyři vanilkové
akce nemá smysl.

---

# 13. CZTR Wagons-Cargo — jmenovitá výjimka pro jeden GRF

Výjimka je v `newgrf.cpp` a je **jmenovitá**: platí pro GRF id `MI\x02\x13`
(`0x4D490213`) **a zároveň** pro jméno `CZTR Wagons-Cargo 1.0.0`. Verze
1.1.0 se nesmí dotknout ničím.

Sada je ve hře v `CZTR_Wagons_cargo.yagl` (rozebraný GRF, 145 553 řádků).
Odtud se dá zjistit všechno, co následuje.

## 13.1 Grafika podle nákladu tam je, jen jinudy — přes **podtyp nákladu**

**Tahle kapitola tvrdila opak a bylo to špatně. Tady je, co platí.**

Co jsem si ověřil správně: každý `feature_graphics<Trains>` (Action 3) má
`default_set_id` a nanejvýš jednu položku `0xFF` (nákupní seznam), seznam
`cargo_types` je prázdný u všech vagonů. Z toho jsem usoudil, že grafika na
nákladu nezávisí. **To byl chybný závěr — díval jsem se jen o patro výš.**

Uvnitř těch skupin se sada ptá na **`variable[0xF2]`, což je podtyp nákladu
(`cargo_subtype`)** — v tom souboru 32×, s několika větvemi na různé sady
spritů. A podtyp je právě to, co nastaví přestavba na náklad, který sada
zná. **Takže obrázek podle nákladu tam je, jen se k němu nejde přes
Action 3, ale přes podtyp.**

Co se stane s nákladem, který autor nenakreslil: vagon si nechá podtyp,
který měl předtím, ten ukazuje na obrázek, který v téhle skupině není, a
vozidlo spadne na sprity toho, čím bylo nahrazeno — **vykreslí se jako
úplně jiné vozidlo.** Přesně to je ta „diagnostická mašinka".

**Oprava:** `ApplyWagonCargoException()` si zapamatuje, které náklady
autor nakreslil (je to přesně ten seznam přeložení, který hra spočítá
z vlastností sady, **než** ho rozšíříme), a vagon vezoucí cokoli jiného se
svému GRF hlásí **bez podtypu**. Tím padne na první obrázek, který má —
což je přesně to, co bylo od začátku zadané: *„vemeš první náklad, který
vagonek má definovaný, a grafiku toho nákladu dáš všem novým nákladům."*

Kopírování skupin spritů podle nákladu, které tu jednu dobu bylo
(`SetSpriteGroup(cargo, …)`), opravdu nikdy nic nedělalo — kopírovalo
výchozí skupinu na místo, kam se nikdo nedívá. To je pryč a vracet se
nemá; ta věc se řeší podtypem, ne skupinami.

## 13.2 Proč Uacs nebyl v depu ke koupení

`CalculateRefitMasks()` končí řádkem, který **vypne vozidlo**, kterému
nezbyl žádný platný náklad: vymaže mu podnebí, ve kterých je dostupné, a
tím zmizí ze seznamu v depu. Vypnuté vozidlo už nic pozdějšího nevzkřísí.

Vagony té sady jsou omezené na sypké náklady a k tomu mají seznam nákladů,
které vézt nesmí. S jiným průmyslem, než pro jaký byly kresleny, ten
seznam může pokrýt **všechny** sypké náklady, které ve hře jsou. Uacs a
Falls se v tom seznamu liší o jediné číslo — a to stačí na to, aby jeden
zbyl a druhý ne.

Moje původní oprava běžela **až po** `CalculateRefitMasks()`, takže
nastavovala náklady vozidlu, které už bylo vypnuté. Proto je výjimka
rozdělená na dvě půlky: `PrepareWagonCargoException()` rozšíří náklady
**před** výpočtem, `ApplyWagonCargoException()` dodělá zbytek po něm.

## 13.2b Ta verze musí být v seznamu vidět

Hra má v seznamu NewGRF nastavení „zobrazovat staré verze" a **implicitně
je vypnuté** — z každého GRF se nabídne jen ta nejnovější verze. Autor
mezitím vydal 1.1.0, takže se hráči skryje přesně ta verze, pro kterou je
výjimka napsaná, a dostal by se k ní jedině ruční úpravou `openttd.cfg`.
Do `.cfg` hráč chodit nemá.

Proto je v `newgrf_gui.cpp` v `BuildAvailables()` druhá půlka té samé
jmenovité výjimky: tenhle jeden GRF se do nabídky dá vždycky, ať je to
nastavení jakékoli. Rozhoduje o tom `IsWagonCargoExceptionGrf()`
v `newgrf.cpp` — jedno a to samé místo pro obě půlky, aby se nemohly
rozejít.

## 13.2c Čím je vagon postavený naložený

Vagon může být po výjimce přeložen na všechno, ale postavit se musí
s jedním konkrétním nákladem, a to není kosmetika: hra počítá kapacitu pro
každý další náklad **poměrem proti tomu, se kterým byl vagon postaven**.
Vzít prostě první náklad ve hře by ze všech otevřených vozů udělalo osobní
vagony a rozhodilo všechny kapacity.

Proto si `PrepareWagonCargoException()` schová třídy nákladů, které autor
vagonu dal (sypké / kusové / kapalné), **než** je rozšíření přepíše, a
`ApplyWagonCargoException()` z nich vybírá. Teprve když ve hře žádný náklad
těch tříd není, sáhne se po prvním, který je.

## 13.2e Ověřeno spuštěním, ne jen čtením

Hráč nahrál skutečný `.grf` (155 MB) a FIRS 3.0.12 jako přílohu GitHub
release `newgrf` — do repozitáře se nevejde, release unese 2 GB. Hraje
s FIRS, **parametr 0 „Economy" = 5 (Extreme)**. Kontejner pustí ven jen
GitHub, takže tudy vede cesta i příště.

Z toho se povedlo hru poprvé spustit tady (headless, OpenGFX postavené ze
zdrojů přes pip nml) a výjimka má trvalý tichý výpis
(`-d grf=2`, řádky „Wagon exception: engine …").

**Co spuštění odhalilo: sada přepisuje původní vanilkové vagony.**
Definuje vlastnosti přímo na původních ID (0x1B–0x26…), takže v enginovém
poolu **není jediný vagon bez GRF** — a hledání „vanilkového uhelného
vagonu" v poolu nemělo co najít, `coal_capacity` zůstala 0 a Uacs vozil 1.
Kapacita se teď bere z **původní tabulky vozidel** (`GetOriginalCoalWagonCapacity()`,
globální původní id 29 „Coal Truck", 30 jednotek), kterou žádný GRF
přepsat nemůže.

## 13.2f Čtyři vagony se ptají rovnou na náklad, ne na podtyp

Nad rozebraným `.yagl` běží simulátor řetězů skupin (vazby čísel skupin
v pořadí souboru, vyhodnocení výhybek) — `sim.py` ve scratchpadu; kdyby
se ztratil, dá se napsat znovu podle 13.1. Co změřil na skutečné sadě:

- **73 vagonů ze 77** s podtypem 0 dojde na obrázek → lež o podtypu stačí.
- **Uacs s podtypem 1** dojde na „výsledek callbacku" → selhání → náhradní
  sprity. Přesně ta diagnostická mašinka; s podtypem 0 dojde na obrázek.
- **4 vagony — M, Z, Ztr, Ztr/Ztrc — na podtyp nekoukají vůbec.** Větví se
  rovnou na **var 0x47 (druh nákladu přeložený tabulkou sady)** a neznámý
  náklad padá do slepé uličky bez ohledu na podtyp.

Proto lže i var 0x47: vagon s nenakresleným nákladem hlásí svůj **první
nakreslený** náklad (třídy, váhu i přeložený slot toho nákladu). Simulátor
potvrdil, že se známým slotem dojdou všechny čtyři na obrázek.

## 13.2g Obrázky se přepínají i podle **roku výroby** — a záchranná síť

Diagnostická mašinka u Uacs po přestavbě měla ještě třetí patro, které
simulátor četl špatně a odhalil ho až běh ve skutečné hře: řetěz jde
**podtyp → rok výroby → náklad**. Sloty nákladů s obrázkem se **liší éru
od éry** (1946–68 / 1969–92 / 1993–2008 / 2009+), takže **žádný jeden
převlečený náklad není správně vždycky** — slot dobrý v jedné éře je
v jiné slepá ulička.

Proto je oprava dvoupatrová:

1. **Převlek podle slotů** (`Engine::drawn_slots`, `disguise_cargo`):
   při načtení se projde strom skupin vagonu (`CollectDrawnCargoSlots()`
   v `newgrf.cpp`) a posbírají se sloty, které vedou na obrázek. Náklad,
   jehož vlastní slot obrázek má, se **nepřevléká** — ukáže se doopravdy.
   Ostatní se hlásí jako `disguise_cargo`.
2. **Záchranná síť v `Train::GetImage()`, tři patra.** Nákupní obrázek
   nestačil: má jediný pohled, takže se vagon neotáčel s kolejí (hlášeno
   na #92, „jen jedna sprite"). A převlek za skutečný náklad hry taky ne:
   nakreslený slot může patřit nákladu, který v téhle hře vůbec není,
   takže **není za koho se převléct** — ale výhybky čtou jen bajt slotu,
   takže se dá **předložit slot sám** (`_wagon_exception_forced_slot`).
   Patra: (a) zkoušet nakreslené **sloty** jeden po druhém přes skutečný
   resolver, první, který v éře vagonu projde, se zapamatuje
   (`Engine::disguise_slot`) → plné otočné sprity; (b) nákupní obrázek;
   (c) prázdný sprite pro články — sada je chce neviditelné.

Důsledek: **diagnostická mašinka se u vagonů sady už nemůže objevit
vůbec.** Tím pádem není co schovávat v nabídce přestavby — všechny
náklady mají aspoň civilní obrázek vagonu. Uacs vozí stavebniny/cement
a vypadá jako Uacs.

## 13.2h Zkušební lavice: konzolový příkaz `cztr_test`

`cztr_test all` (nebo `cztr_test <id šestnáctkově>`, např. `cztr_test b1`)
postaví v čerstvé hře depo, koupí každý vagon sady, přeloží ho postupně na
**všechny náklady hry** a u každého se zeptá vykreslování: `O` = vlastní
obrázek, `o` = obrázek z nákupního seznamu (síť), `.` = záměrně prázdný
článek, `X` = náhradní sprity (= diagnostická mašinka). Přiděluje si
railtype a peníze samo (sada vypíná všechny původní vlaky, takže čistá
hra jinak nemá ani koleje). Ověřeno: **35/35 vagonů kreslí** na hráčově
`openttd.cfg` (uložené ve scratchpadu; hráč ho poslal, rok testu 2020,
`never_expire_vehicles = true` — bez něj dva vagony s pozdním zavedením
nejdou koupit, což není chyba grafiky).

V resolveru je vypínatelná stopa `_cztr_trace` (zapíná ji lavice), která
vypisuje průchod výhybkami — přesně tahle stopa našla to rokové patro.

## 13.3 Kapacita: sada odpovídá nulou

41 vagonů té sady má v nastavení `cargo_capacity: 1` a k tomu obě
zpětná volání o kapacitě odpovídají nulou:

- volání `0x36` (změna nastavení) pro nastavení `0x14` (kapacita) → `0x8000`,
  tedy návratová hodnota 0 — v souboru 84×,
- volání `0x15` (kapacita po přeložení) → `0x8000`, tedy 0 — v souboru 42×.

`Engine::CanCarryCargo()` se přitom dívá jen na **nastavení**, ne na
volání, takže vagon projde jako schopný vézt náklad a pak veze nulu.

Proto stará podmínka `if (rvi.capacity == 0)` nikdy nevyskočila: nastavení
není nula, je jedna. Nově dostanou vagony s kapacitou 0 nebo 1 kapacitu
původního uhelného vagonu a **jejich zpětná volání o kapacitě se přestanou
brát v potaz** (`Engine::ignore_capacity_callback`). Vagony, které skutečnou
kapacitu uvedly (57, 60, 200 …), se nechávají být i s vanilkovým chováním.

**Kapacitu dostane jen vagon, který jde koupit.** Delší vagony sada skládá
z hlavy a jednoho či dvou **neviditelných článků** (`" Invisible"`,
instance 0x00AF a 0x00B0 u Uacs), a ty mají tu jedničku taky. Kdyby každý
z nich dostal uhelnou kapacitu, jeden vagon by vezl za dva za tři. Článek
se od vagonu pozná tím, že **není dostupný v žádném podnebí** — právě to
z něj dělá článek a ne vozidlo.

## 13.2d Jak Uacs vypadá zevnitř

Užitečné pro hledání: Uacs je instance `0x00B1` a je **kloubový**. Zpětné
volání `0x16` (skupina `0xD4`) vrací pro článek 1 instanci `0x00B0` a pro
článek 2 instanci `0x00AF`, oba pojmenované `" Invisible"`. Vlastní obrázek
nese hlava; články jen dělají délku.

Grafika hlavy vede přes výchozí skupinu `0xFD` → `0xCE` → `0xD3` →
sada spritů `0x0000`, kde je v 8bpp jen bod 1×1 a skutečný obrázek je až
ve 32bpp (`zin4`). Tak to má celá sada, i vagony, které fungují.

---

# 1.4 Vanilkové „otočení na místě" se u nás nemůže stát vůbec

Důležité při hledání, ať se to nehledá tam, kde to není. Hra má dva
způsoby, jak obrátit vlak:

1. **couvání** — vlak se nepohne a jen začne jet druhým koncem,
2. **otočení na místě** (`ReverseTrainSwapVehicles`) — vozy si vestoje
   vymění místa, první s posledním. To je ten „magic flip".

Volání číslo 2 je v celém `train_cmd.cpp` **jediné** a je zahrazené
podmínkou, ve které je i nastavení `difficulty.train_flip_reverse_allowed`.
A to nastavení je u nás **natvrdo zamčené na `None`** — nedá se přepnout
v nabídce a při každém načtení hry se bezpodmínečně přepíše zpátky. Tím je
ta podmínka **vždycky** splněná první částí, takže:

- **vlak se u nás nikdy neotočí na místě, vždycky jen začne couvat;**
- **na `CanLeadTrain()` se v tom rozhodování nikdy nedojde** — je až za
  tím nastavením.

Z toho plyne i to, že sebrání příznaku „má kabinu" GRFům (13.5) na tohle
rozhodování **nemá vliv žádný**. Vliv má jinde: na rychlostní postih vlaku
bez kabiny a na naše pravidlo, který konec vede spojený vlak.

Ani přerovnání směrů při spojení (`NormaliseCoupledConsistFacing`) viditelné
otočení způsobit nemůže: `Train::GetImage()` u otočeného vozu sám obrací
směr pohledu, takže se obrácení směru a přehození příznaku vzájemně vyruší.

**Takže když hráč uvidí, že se vlak otočil, není to vanilkové otočení.**
Zbývají naše vlastní věci: přepojení seznamu (`ReverseConsistOrder`) a
otočení v depu — a to druhé je zahrazené na „celý vlak uvnitř depa".

---

# 13.5 Žádný GRF nerozhoduje o tom, jak se otáčí vlak

Dva příznaky vozidla umožňují sadě rozhodovat o otáčení za hru:

- **„má kabinu"** (`ExtraEngineFlag::HasCab`) — říká, že i vůz bez pohonu
  smí vést vlak. Právě na tohle se ptá `ReverseTrainDirection()`, když
  vybírá, jestli se vlak **otočí na místě**, nebo jen **začne jet na
  druhou stranu**. Sada, která ten příznak rozdává, tím mění chování
  každého vlaku, ve kterém takový vůz je.
- **„staré otáčení v depu"** (`EngineMiscFlag::RailFlips`) — sada si
  otočený vůz kreslí a měří sama, místo aby to nechala na hře.

U nás se o tom, kterým koncem vlak jede, rozhoduje **měřením** a pravidly,
která hráč vidí. Když do toho zvenčí mluví ještě sada, přestane se to dát
číst — a hlavně přestane být reprodukovatelné, protože výsledek závisí na
tom, které sady jsou zrovna načtené.

Proto se oba příznaky u všech GRF vozidel **zahazují**
(`IgnoreNewGRFReversingFlags()` v `newgrf.cpp`) a každý vlak se otáčí
stejně. Není to mířené na žádnou konkrétní sadu a nic jiného to vozidlu
nebere — grafiku, kapacitu ani cokoli dalšího si nechává.

---

# 13.6 Sada vozidel bez své sady kolejí jede po obyčejných kolejích

Vanilka vozidlo, jehož typ kolejí žádný nahraný GRF neposkytuje, **potichu
vypne** — sada mašinek bez sady kolejí, pro kterou byla dělaná, se prostě
„naschvál nespustí". U nás ne: takové vozidlo spadne na **obyčejné koleje
hry** — elektrická mašinka na elektrifikovanou běžnou kolej, všechno
ostatní na obyčejnou trať. Zvláštní kolej byla přání autora sady, ne
podmínka.

Platí to obecně pro všechny GRF (`newgrf.cpp`, místo kde se překládají
`railtypelabels`). Vypnutí zůstává jen vozidlu, které žádný typ kolejí
nikdy neuvedlo — tam není z čeho couvat.

---

# 13.7 FIRS se kvůli CZTR mašinkám sám vypíná — naše hra mu je zatají

**Co se dělo.** FIRS od verze 4 nosí v sobě natvrdo seznam „nekompatibilních"
GRF (ve zdrojáku `src/grf/incompatible_grfs.py`) a při startu hry se ptá, jestli
některý z nich není nahraný. Všechny čtyři sady CZTR Engines na tom seznamu
jsou — Steam `4D490207`, Electric `4D490208`, Diesel `4D490209`, EMU `4D490210`.
Když FIRS některou najde, vyhodí fatální hlášku `E01: Incompatible set` a **sám
sebe vypne** — bez FIRSu pak nejede průmysl. Důvod autorů FIRSu: CZTR mašinky si
definují vlastní náklady. U nás ale náklady vozí Uacs vagonky, které si je berou
od FIRSu (téma 13), takže ta pojistka jen překáží.

**Jak je to vyřešené.** Ta kontrola je obyčejný dotaz GRF na hru: „je/bude
aktivní GRF s tímhle ID?" (akce 7, podmínka `0x0A`). Odpovídá na něj jediné
místo — `newgrf/newgrf_act7_9.cpp`, `SkipIf()`. Tam je teď výjimka
`HideGrfFromIncompatibilityCheck()`: **když se FIRS (jakákoli verze, ID
`F12500xx`) ptá na některé ze čtyř CZTR ID, hra odpoví „takový GRF neznám".**
FIRS pak chybový blok sám přeskočí — úplně stejně, jako by CZTR ve hře nebylo —
a normálně se spustí. Lež platí jen pro tazatele FIRS a jen pro ta čtyři ID;
každý jiný dotaz kohokoli na cokoli jede beze změny. Obráceně se nelže schválně:
kdyby se CZTR ptalo na FIRS, může to dělat proto, aby si podle toho zapnulo
FIRSí náklady — to se rozbít nesmí.

**Ověřeno v bezhlavém rigu** třemi mini-GRF: napodobenina CZTR (jen hlavička
s ID Diesel), napodobenina FIRSu (stejný dotaz + stejná fatální chyba jako
pravý FIRS) a kontrolní GRF s cizím ID a toutéž kontrolou. Výsledek: CZTR
aktivní, FIRS chybu přeskočil a aktivoval se, kontrolní GRF chybu vyhodil —
takže napodobenina je věrná a výjimka opravdu platí jen pro tu jednu dvojici.
Generátor těch GRF není v repozitáři (je to pomůcka rigu, ne hry).

**Druhá půlka: s FIRS 5 smí jen vagonky 1.0.0.** Odblokování ukázalo, že
s FIRS 5 pak běží i CZTR Wagons-Cargo **1.1.0**, které na FIRS 5 připravené
nejsou (výjimka na náklady — téma 13 — je jmenovitá pro 1.0.0 a verze 1.1.0
se nesmí dotknout). Nechceme autorům zasahovat do úmyslů, tak si hra drží
vlastní párové pravidlo (`LoadNewGRF`, hned u resetu stavů): **když je ve
hře FIRS 5 (`F1250009`) a k tomu Wagons-Cargo (`4D490213`) v jiné verzi než
jmenovité 1.0.0, ta verze se vypne** s fatální hláškou „Tahle verze CZTR
Wagons-Cargo není připravená na FIRS 5. Použij starší CZTR Wagons-Cargo
1.0.0" — ukáže se v červeném rámečku po startu i u GRF v nastavení, stejnou
cestou jako FIRSí E01. Bez FIRS 5 se vagonek nikdo nedotkne. Rig: tři běhy
s napodobeninami — FIRS5+1.1.0 → 1.1.0 vypnuto; FIRS5+1.0.0 → obojí jede;
1.1.0 samotné → jede.

---

# 14. Statistika firmy podle TTDPatch

V okně firmy je pod čudlíkem ředitelství čudlík **Statistika**. Otevře okno
se seznamem: nadpis „Kolik čeho společnost celkem přepravila" a pod ním
řádek na každý náklad, který firma kdy dovezla — jméno vlevo, množství
vpravo. Náklady, se kterými firma nikdy nic neudělala, se nevypisují.

**Odkud se to číslo bere.** Hra si dosud pamatovala jen
`CompanyEconomyEntry::delivered_cargo`, a to jen za posledních
`MAX_HISTORY_QUARTERS` čtvrtletí — na „od založení" to nestačí. Přibyl
proto průběžný součet `Company::total_delivered_cargo`, který se
přičítá na tom jednom místě, kde se počítá i to čtvrtletní
(`economy.cpp`, `DeliverGoods()`).

Součet se ukládá do savegame (verze `CompanyTotalDeliveredCargo`). Starší
uložené hry se načtou dál, jen v nich ta statistika začíná od nuly —
zpětně se dopočítat nedá, ta data v nich nejsou.

---

# 15. Otevřené

- Pády po spojení a rozpojení: `pool_type.hpp:174` (sáhnutí mimo seznam
  objektů) a `track_func.h:168` (žádaná jedna kolej, dostala se jiná
  množina). Potřebuju `crash.log` — píše se do složky k `openttd.cfg`, ale
  jen když se po tom hlášení nechá hra sama doběhnout.

  **Ze `crash.log` (build #78) se stack trace nedozvíme a nikdy nedozvíme.**
  V logu je `"stacktrace": []` — prázdné pole, ne „nepovedlo se to
  posbírat". Hra si výpis zásobníku dělá přes systémovou knihovnu
  `dbghelp.dll`; když ji nenajde nebo v ní nenajde funkce, celý ten krok
  **tiše přeskočí** a nechá prázdné pole. Přesně to se stalo. Vyloučeno
  je přitom to, co by člověk čekal: assert **není** slepá ulička bez
  kontextu — `abort()` se odchytí a převede na skutečnou výjimku
  s platným kontextem, takže kdyby ta knihovna byla, výpis by tam byl.

  **Důsledek: hlášku si musí nést informaci sama.** Proto se fond od
  `0afcee0` pojmenuje i s indexem. Řádek `reason` v logu je totiž to
  jediné, co se odtamtud spolehlivě dozvíme.

  Co ještě log řekl: verze `b1178bc` (tedy bez opravy vraku v depu a bez
  toho pojmenování), Windows 7 SP1, dvě železniční neštěstí krátce
  předtím, šest vlaků, a `_current_company` mimo firmu.

  Co už se o tom `track_func.h:168` ví, bez logu:

  - Hlásí se ta hláška **z `TrackBitsToTrack()` samotné**, ne z mého
    diagnostického assertu v `Train::ReserveTrackUnderConsist()`. Ten by
    ohlásil `train_cmd.cpp` a ohlásil by se dřív. **Takže to není
    `ReserveTrackUnderConsist()`** — a to je půlka podezřelých pryč.
  - Zbývají místa, která tu funkci volají: `DeleteLastWagon()`
    (`train_cmd.cpp`, maže vozy havarovaného vlaku po jednom),
    dvě místa v `TrainController()` s `chosen_track`, `signal.cpp` a
    `rail_cmd.cpp`.
  - `DeleteLastWagon()` mělo tu samou díru: pro tunel/most výjimku mělo a
    o depu **vědělo** — o pár řádků níž se na depo ptá ve smyčce — ale
    samo volání `TrackBitsToTrack()` bylo nad tím a nechráněné. Že se u
    nás havarovaný vlak v depu ocitnout může, je novinka, kterou přinesl
    odtah. **Opraveno** (`e842ce1`), ale jestli to byl ten pád ze
    screenshotu, ověřené není.
  - **Ta samá hláška už jednou byla, a stojí v `TEST_LOG.md`:** build #68,
    `assert(chosen_track.Count() == 1 && !chosen_track.Any({Wormhole,
    Depot}))`, situace **hromadný výjezd z depa**, mašinky s příkazem „jet
    se spojit" ztratily orientaci. To je ta samá podmínka, jen tenkrát ji
    ohlásil diagnostický assert nad `chosen_track` a dnes ji hlásí
    `TrackBitsToTrack()` samotná. A poslední hlášení je zase z hromadného
    výjezdu z depa.

    **Takže druhý kandidát jsou obě místa v `TrainController()`, která
    předávají `chosen_track`** — a ta sedí na situaci líp než mazání
    havarovaných vozů. Kdyby se hledalo znovu, začít tady.

  - **Znovu, build #115 (`crash20260831000622`), a stopa už řekla kde:**
    `Assertion failed at line 7320 of train_cmd.cpp: chosen_track.Count()
    == 1 && !chosen_track.Any({Wormhole, Depot})`, `"stage": "game
    running"`. To je **druhý** z těch dvou assertů — ten za celým
    if/else, tedy platí i pro **vagonovou** větev (`prev != nullptr`,
    „vagon jede za předchozím"). Hráčův popis: „něco v depu". Malá mapa,
    3 vlaky, poruchy zapnuté.

    Ověřeno a **vyloučeno**: podezření, že `AssembleDepotRake()` po
    slepení odložených řad neobnoví cache soupravy, neplatí —
    `TryConsistSplice()` si volá `NormaliseTrainHead()` sám, a ten dělá
    `ConsistChanged(CCF_ARRANGE)`.

    **Co k tomu přibylo:** hned před oběma asserty stál výpis
    `krok ROZBITY: …`, který říká přesně to, co je potřeba — které
    vozidlo, odkud kam, `enterdir`, vybrané bity a kolej předchozího —
    ale psal se **jen se zapnutým `vlak123`**. Teď se píše vždycky a jde
    navíc do crash reportu jako `crash.note`. Dvakrát už to spadlo na
    stroji, který neumí výpis zásobníku, a pokaždé byla odpověď
    v konzoli, kterou hráč neměl důvod mít zapnutou.
- Po načtení savu odjely ze stanice mašinky, které čekaly na spojení —
  poskočil jim příkaz.
- Peron 1 a 2 při spojení mašinka+mašinka: výbuchy, po načtení savu
  zamrzání.
- Otočit směr na první dlaždici od depa zamrazí vlak.
- Odpojení přeskočené při prvním příjezdu po načtení hry.

---

# 17. Přenos staré hry: co po vozidlech zůstane, když se vozidla nenačtou

**Pád po prvním opravdovém přenosu (build #111, `EXCEPTION_ACCESS_VIOLATION`,
`crash20260830115026`).** Hráčův testovací save se načetl, jeho skutečná
rozehraná hra spadla pár vteřin po načtení. Rozdíl **nebyly GRF** — v crash
logu je všech 62 „activated", mapa i datum se načetly (tik 19034) — rozdíl
byl, že v rozehrané hře **stála vozidla na nádražích a nakládala**.

Řetěz je v kódu jistý, ne odhadnutý:

1. Nádraží si drží `Station::loading_vehicles` — seznam vozidel, která
   u něj právě nakládají. Ukládá se jako `SLE_CONDREFLIST(..., Vehicle)`.
2. Přenos vozidla **přeskakuje**, takže `IntToReference()` na každý odkaz
   na vozidlo odpoví `nullptr`. Načítací smyčka
   (`SlStorageHelper::SlSaveLoad`) ale seznam **nezkracuje** — vytvoří
   tolik položek, kolik jich v souboru bylo, a pak do každé zapíše to
   `nullptr`. Ze seznamu vozidel je seznam děr, ne prázdný seznam.
3. `CallVehicleTicks()` volá `LoadUnloadStation()` na **každé** nádraží
   **každý tik** a hned na prvním řádku dělá `v->vehstatus` — bez
   kontroly na `nullptr`, protože ji tam nikdy nikdo nepotřeboval.

To je sáhnutí na nulovou adresu v prvním tiku po načtení. Přesně to,
co se stalo.

**Můj vlastní komentář u té opravy tvrdil, že to je v pořádku** — že
všechna místa, která jmenují vozidlo, si s „nic tu není" poradí, protože
prodané vozidlo za sebou nechává stejnou díru. **Pro jedno políčko to
platí, pro seznam ne.** Napsal jsem to jako hotový závěr, aniž bych to
u seznamu ověřil. Komentář je opravený a říká to.

**Oprava:** `AfterLoadLegacyDecoupleImport()` (afterload.cpp) uklidí po
konci načítání všechno, co po vozidlech na mapě zbylo, v jediném
okamžiku, kdy je jisté, že žádné vozidlo neexistuje:

- `loading_vehicles` na všech nádražích,
- `airport.blocks` — obsazená stání a pojezdovky; ponechané by letiště
  tiše odmítalo i letadla postavená později,
- rozdělané platby (`_cargo_payment_pool`; přes `CleanPool()`, protože
  destruktor sahá zpátky přes vozidlo na firmu),
- **všechny rezervace kolejí** (koleje, depa, přejezdy, nádraží, tunely
  a mosty) a přepočet závor na přejezdech.

Ty rezervace jsou to tiché a horší: kolej držená pro vlak, který se
nikdy nenačetl, je držená napořád. **I v tom „funkčním" testovacím save
jich zůstávalo pět.** Přenos to teď hlásí do konzole číslem, protože
právě tohle číslo je rozdíl mezi klidným testem a rozehranou hrou.

**Nereprodukováno u nás.** Ani jeden ze dvou savů, které máme, nemá na
nádraží nic nakládajícího (naměřeno: 0), a náš vlastní save se přes
přenos načíst nedá — přeskakování počítá s tvarem cizí větve. Řetěz je
doložený z kódu, ne z běhu.

**A pád to nebyl.** Build #112 s tou opravou spadl na stejném save znovu,
stejně (`crash20260830150239`). Ta díra v seznamu je skutečná chyba
a opravená zůstává, ale příčina pádu ležela jinde — a **v obou crash
lozích má firma všechny počty infrastruktury na nule.** Ty se
přepočítávají z mapy v `AfterLoadCompanyStats()` (company_sl.cpp), který
běží nepodmíněně na konci `AfterLoadGame()`. Nula tedy znamená: **hra
spadla ještě před ním**, uvnitř načítání, ne až v prvním tiku hry. To
zároveň vylučovalo seznam nakládajících vozidel, který se čte až za běhu.

## 17.3 Čudlík přenosu: celý mačkací, nad stahováním obsahu

Byl to úzký zaškrtávací řádek pod tlačítkem „hledat chybějící obsah
on-line" a nesl jednu zkratkovitou větu. Teď:

- **stojí nad** stahováním obsahu — patří k save, který je zrovna
  vybraný, ne k nastavení hry, a je to jediná věc v tom okně, kterou
  hráč musí najít, aniž by mu někdo řekl, že tam je;
- je **celý mačkací a tak vysoký, jak je potřeba** — text si kreslí sám
  (`DrawWidget`) a láme se přes tolik řádků, kolik zabere. Jednořádkový
  popisek by musel být krátký, a tím i zašifrovaný;
- **zapnuto** = zamáčknuté tlačítko a oranžový text. Obě znění jsou
  **stejně dlouhá schválně**: čudlík, který by při zmáčknutí změnil
  výšku, by odsunul všechno pod sebou zpod prstu, který ho zmáčkl;
- **zatmavený, když to není starý save.** Nabízet přenos na cokoli
  jiného znamená nabízet vyhození vozidel ze save, který by se načetl
  úplně v pohodě. Kterou hrou byl soubor psaný, je jen v jeho vlastním
  gamelogu (`Gamelog::Info()` → poslední zápis o revizi); nic jiného
  v souboru to neříká. Naměřeno: `testold.sav` psán verzí `0x19000000`,
  naše `0x20006d64` → jde zmáčknout; naše vlastní savy `0x20006d64` →
  zatmavený.

---

## 17.2 Skutečná příčina: rozdělaná platba bez vozidla

Build #113 už nesl stopu z tématu 17.1 a crash log poprvé řekl kde:

```
"stage": "afterload: ClearOldOrders"
```

`ClearOldOrders()` sama je dva řádky a spadnout nemůže. Hned za ní je
ale v `AfterLoadGame()` tohle, **nepodmíněně, bez ohledu na verzi save**:

```cpp
/* Fix the cache for cargo payments. */
for (CargoPayment *cp : CargoPayment::Iterate()) {
    cp->front->cargo_payment = cp;
    cp->current_station = cp->front->last_station_visited;
}
```

`CargoPayment::front` je odkaz na vozidlo
(`SLE_REF(CargoPayment, front, SLRefType::Vehicle)`). Rozdělaná platba
vznikne, když vozidlo začne vykládat, a zavře se, až doloží; **v rozehrané
hře jich je otevřených plno.** Přenos vozidla přeskakuje, takže každé
`front` vyjde na nic — a tenhle řádek do toho nic okamžitě zapisuje.
Sáhnutí na nulovou adresu uprostřed načítání.

To vysvětluje i to, proč testovací save bez GRF jde načíst: **na klidné
zkušební mapě nikdo nic nevykládal**, takže tam žádná rozdělaná platba
není (naměřeno: 0). Rozdíl nikdy nebyly GRF.

**Oprava:** úklid po přenosu se rozdělil na dvě půlky, protože jsou
potřeba ve dvou různých okamžicích. Co je viset zůstalé **jméno**
(seznamy nakládajících vozidel, rozdělané platby) musí ven **dřív, než
se rozběhne jediný řádek obyčejného dodělávání po načtení** — hned na
začátku `AfterLoadGame()`. Co je stav mapy (rezervace, obsazená stání na
letištích, závory) může počkat, až se mapa dopřevede do dnešní podoby,
a zůstává na konci. Můj původní úklid dělal obojí najednou a byl až na
konci — tedy **o dva tisíce řádků později, než ta smyčka**.

## 17.1 Crash log se u nás píše vždycky, a říká, kde byl kód

Dvě věci, které stály za tím, že hráč z prvních pádů neměl vůbec nic:

**Vanilka crash log schválně nenapíše, když v save chyběl nebo se
nahradil nějaký GRF.** `HandleSavegameLoadCrash()` (afterload.cpp) při
pádu během načítání nastaví `_saveload_crash_with_missing_newgrfs`, když
je kterýkoli GRF `Compatible` nebo `NotFound` — a `ExceptionHandler`
(crashlog_win.cpp) pak jen ukáže okno s hláškou a hru ukončí, **bez
souboru**. Pro nás je to obráceně: celý smysl přenosu staré hry je
načíst cizí save s tím, co je po ruce, takže právě ten případ potřebuje
hlášení nejvíc. U nás se hlášení napíše jako každé jiné a informace
o GRF jde **do něj**, ne místo něj (`crash.note`).

**Zbytek už funguje a nemusel se měnit:** `MakeCrashLog()` zapíše na disk
crash log, minidump, záchranný save i screenshot a **teprve potom** se
skočí do `ShowCrashlogWindow()`. Okno jen ukazuje cestu k souboru, který
tam v tu chvíli dávno je; OK hru jen ukončí.

**A protože na Windows 7 bez `dbghelp.dll` je `stacktrace` prázdný
(téma 15) a `reason` je jen „EXCEPTION_ACCESS_VIOLATION" — což říká *co*
a vůbec nic o *kde* — přibyla do hlášení stopa `crash.stage`.** Je to
jméno posledního místa, kterým načítání prošlo: `loading chunk XXXX` při
čtení souboru, `afterload: before <VerzeSaveGamu>` v každém z 131
verzních bloků `AfterLoadGame()`, `afterload: <JménoFunkce>` u každého
z 30 pojmenovaných volání, a `game running`, jakmile načítání skončí —
jinak by stopa ukazovala na načítání do konce hry. Ověřeno vynuceným
pádem: `"stage": "afterload: StartScripts"`.

---

# 16. Nedořešeno

Ne chyby — místa, kde je rozhodnuto jen napůl a ví se o tom. Každé z nich
je vědomé, ne přehlédnuté; čekají na rozhodnutí, ne na opravu.

- **„Prodat vše" v depu prodá i rezervovanou řadu.** Okno depa na řadu,
  pro kterou už jede mašinka, nepustí ani klik, ani tažení, ani prodej
  jednotlivého vozu — ale hromadné tlačítko jde mimo okno rovnou do
  příkazu `DepotMassSell` a rezervaci nezná. Nechané schválně jako
  úniková cesta: kdyby se něco zaseklo, hráč má čím řadu uvolnit, a
  mašinka to ustojí — cíl se jí zruší, počká v depu a sebere až to, co
  přijde po ní. Kdyby se to mělo zamknout, patří to do příkazu, ne do
  okna.
