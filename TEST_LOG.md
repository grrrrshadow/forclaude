# Záznam testů

Doslovné poznámky od hráče z testování Windows buildů. Nic se z toho zatím
neřeší — vyhodnotí se to najednou, až budou testy hotové.

## Build #91 (commit 2d4b3aa) — spojení vlak+vlak poprvé čisté; zbyly dvě vady

**Průlom: připojení k vlaku a odpojení je VV na všech čtyřech nástupištích,
z obou stran nádraží, zadkem i předkem.** Pravidlo „spojení nemění směr"
z `d0386db` drží.

Severovýchodní strana:

| test | zadkem | předkem |
|---|---|---|
| vlak: spojit + odpojit | 1234 VV | 1234 VV |
| vlak, reversní chod | 1234 **VB** | 1234 **VB** |
| vagonky: spojit + rozpojit | 1234 VV, ale **špatný směr odjezdu** | 1234 VV, špatný směr |
| vagonky, reversní chod | 1234 VV (odjezd „správně", ale jen shodou) | 1234 VV |

Jihozápadní strana: stejné výsledky řádek po řádku (vlak VV/VV, reversní
chod VB, vagonky VV se špatným směrem, s reversním chodem VV).
Vlaky a vagonky tam stojí jako na plánku, ale přijely jinak: ze
směrování se jezdí zadkem na 3 a 4, přímo na 1 a 2.

**Vada 1 — vagonky: po spojení vlak změní směr odjezdu.** Proti pravidlu
naprosté předvídatelnosti; hráč to nemá jak ovlivnit (reversní chod dává
stejný výsledek jako bez něj). Mezi depem a nádražím jezdí vlak správně.
Mechanika nalezena: vlak+vlak uzavře příkaz ručně a odjede rovně; vlak+
vagonky jde přes nakládku, příkaz poskočí až po ní, a vanilkové „nemám se
otočit k dalšímu cíli?" při posunu příkazu ho otočí. → jednorázové
potlačení té otázky po spojení.

**Vada 2 — reversní chod + odpojení vlaku = výbuch (VB), obě strany.**
Znaky z oken (`vlak123`, screenshoty z odpojovacího nádraží, soupravy
přijíždějí zprava): souprava před rozpojením H DP; po rozpojení mašinka H;
odpojený vlak jednou H DP, podruhé **Z DP**. Mechanika: rozpojení mění
vedoucí konec jen příznakem, bez vanilkové doprovodné práce (otočit
bookkeeping kopec/z kopce a znovu ohlásit každý vůz na jeho políčku).
Vlak, který dojel vedený opačným koncem — po odjezdu reversním chodem —
pak má rozbitou geometrii a bouchne; vlak dojetý mašinkou napřed má
z přepnutí příznaku no-op, proto VV. → doplněna ta práce (obojí opraveno
po #91, viz TEMATA 2.6).

## Build #90 — odjezdy po spojení, a čím se nádraží orientuje

**Světové strany zkušebního nádraží** (do teď se psalo „vlevo/vpravo"):
pravá strana nádraží je ve hře **severovýchod**, levá **jihozápad**. Vlaky
čekající na spojení i vagonky na rozpojení přijíždějí ze **severozápadu**.
Mašinka s příkazem „jet se spojit" jezdí **z obou stran**.

**Co je dobře:** připojené **vagonky** odjíždějí správně — nemění směr
jízdy a je to předvídatelné.

**Co je špatně:** připojené **vlaky** odjíždějí postaru, tou stranou
nádraží, kde se mašinka připojila, a mění směr jízdy. Všechny pak
bouchly při rozpojení.

Test připojení ze **severovýchodu** — jednotné na všech čtyřech, pozadu
i popředu: **1 VB, 2 VB, 3 VB, 4 VB**.

Test rozpojení, mašinka přijíždí z **jihozápadu**:

| jak se mašinka připojila | 1 | 2 | 3 | 4 |
|---|---|---|---|---|
| nacouvala k vlaku zadkem | V V | B | B | B |
| připojila vlak předkem | V V | V B (vyjela jinou stranou) | V B | V B |

**Rozhodující pozorování hráče:** když spojené vlaky **ručně otočil** při
odjezdu z nádraží, rozpojily se **perfektně všechny** — jen si občas
prohodí příkazy. Takže rozpojení samo o sobě v pořádku je a chyba je
v tom, kterým směrem spojený vlak z nádraží vyjede.

**Druhá stopa:** z jihozápadu odjíždějí vlaky správně (směrem, kterým
přijely) **kromě nástupiště 2**, kde odjíždějí postaru. Mašinky připojené
ze severovýchodu se tedy chovají jinak než ty připojené z jihozápadu —
a to je nesouměrnost, která nemá kde vzniknout, pokud se nejmenuje konec.

## Build #88 — pád v `TrainController()`, `train_cmd.cpp:6135`

Hláška: `chosen_track.Count() == 1 && !chosen_track.Any({Wormhole, Depot})`.
Je to **můj diagnostický assert**, ne vanilkový — a udělal přesně to, kvůli
čemu tam je: řekl nám, které volání to je.

Vanilka by na tom samém spadla o řádek níž uvnitř `TrackBitsToTrack()`
(ta hláška z buildu #78), jen bez udání místa. **Takže tohle je ten pád,
který se hledal od #78, a teď je zaměřený:** je to větev „vagon jede za
vozem před sebou" v `TrainController()` — `chosen_track = prev->track`
(případně `_connecting_track`) a pak `&= bits`. Vagon vjel na políčko,
které kolej jeho předchůdce vůbec nenabízí, takže po tom `&` nezbylo nic.
Není to tedy rezervace ani volba cesty; **je rozbitá geometrie vlaku** —
vůz a jeho předchůdce stojí tak, že je žádná kolej nespojuje.

Okolnosti podle hráče: mašinka **nacouvala ze směrování** a udělala „magic
flip", potom pád. Na hlavovém nádraží se má otáčet odrazem na konci.
**Bez mašinek CZTR problém mizí.** Vlak pak taky nechtěl odjet vyložit na
další stanici.

**Kde ten „magic flip" může vzniknout — prohledáno:** vanilkové fyzické
otočení vlaku (`ReverseTrainSwapVehicles`, vozy si vymění místa vestoje)
má v celém souboru **jediné volání**, a to je zahrazené podmínkou, která
vlak jedoucí pozpátku posílá druhou větví — takže **vlak, který nacouval,
tudy fyzicky otočit nejde.**

Zbývá tedy **naše vlastní přerovnání seznamu** (`ReverseConsistOrder`),
které nic nepohne a jen přepojí seznam odzadu dopředu. To se volá při
spojování, při rozpojování a při přebírání vedení v depu. Když se spustí
ve chvíli, kdy soupravy neleží tak, jak předpokládá, vznikne vlak, jehož
pořadí v seznamu neodpovídá tomu, jak stojí na kolejích — **a to je přesně
ta rozbitá geometrie, kterou hlásí assert.** Tam se hledá dál.

## Build #85 — podrobná tabulka výbuchů při spojení mašinky s vlakem

**Tohle je měření, které se v minulém kole ztratilo. Nesmí se ztratit
znovu a hráč ho nemá čím opakovat.**

Netýká se připojování vagonků — to je v pořádku. Měřilo se spojení
**mašinky s vlakem**. „V" = odvezla, „B" = výbuch. První písmeno je
standardní zkušební nádraží (čtyři nástupiště), druhé je nádraží pro
rozpojení (jedno nástupiště). Kde je jen jedno písmeno, na druhé nádraží
se to už nedostalo. Na rozpojení zatím nikdy nejede ze směrování.

| jak se mašinka spojila | 1 | 2 | 3 | 4 |
|---|---|---|---|---|
| předkem (komínkem napřed) | V B | V B | V B | V B |
| zadkem, ze směrování | B | V B | B | B |
| otočená v depu, předkem | B | V B | B | B |
| otočená v depu, zadkem | V | V | V | V |

Nádraží pro rozpojení bylo ke konci ucpané vraky, takže poslední řádek
druhé písmeno nemá; podle hráče by bouchly taky, ale neměřilo se to, tak
se s tím nepočítá.

**Co z těch čísel plyne, ještě než se sáhne do kódu:**

- **Tabulka je sama se sebou v souladu.** „Otočená v depu + zadkem" dopadá
  přesně jako „normální + předkem", a „otočená v depu + předkem" přesně
  jako „normální + zadkem". Otočení v depu obrátí, kterým fyzickým koncem
  mašinka na soupravu dojede — takže obě dvojice popisují tu samou situaci
  na kolejích. **Není to tedy náhoda ani šum, je to opakovatelný jev, a
  závisí jen na tom, kterým koncem mašinka soupravu potká.**
- **Na standardním nádraží** projde spojení vždy, když mašinka dojede
  komínkem k soupravě. Když dojede druhým koncem, bouchne to na
  nástupištích **1, 3 a 4, a na 2 ne**.
- To „a na 2 ne" nesedí ani na jeden vzorec z pravidla 0.6: není to všechna
  čtyři (směr) ani dvojice 1+2 nebo 3+4 (hlava). Nástupiště 2 je samo o
  sobě, a to je vlastní stopa — buď je na dvojce něco jinak, nebo je to
  něco, co na dvojce jen o vlásek nevyjde.
- **Na nádraží pro rozpojení** bouchne spojení předkem na **všech čtyřech**
  původních nástupištích. Podle pravidla 0.6 je to problém ve směru.

## Build #84 (commit 10e9f89) — otevřené, na příště

**Odložený vlak vybuchuje při dalším spojování.** Po rozpojení odjela
mašinka do depa a na místě zůstal stát jen přitažený vlak. Ten pak při
připojování dalšího vlaku začal vybuchovat. Hráč to nechává na příští
kolo.

Co k tomu vím předem, než se do toho pustím: tenhle build je první, ve
kterém je odložená souprava otáčena zpátky v seznamu a ve kterém je
havárie stavem vlaku místo jeho konce. Obojí sahá přesně na to, co ten
odložený vlak je, takže se hledá tam — a podle pravidla 0.6 bude první
otázka, na kterém nástupišti byla ta souprava **spojená**.

## Build #65 (commit 59798a3)

**Couple pozadu** — dobrý na všech čtyřech nástupištích. Všem čtyřem
mašinkám to *neposkočilo* na další příkaz „jeď do depa".

**Screenshot 1 — zaseknutí při výjezdu z depa po zmáčknutí „otočit".**
Mašinka při výjezdu z depa se zasekne, pokud zmáčknu otočit. Zopakováno na
druhém depu. Při vyjíždění dám otočit a mašinka se zasekne.
Na trati venku za depem ji otáčím normálně, žádný problém. V depu ji také
otáčím normálně, žádný problém. Problém je jen otočení *při výjezdu*.

Poznámka hráče k témuž screenshotu: tu třetí mašinku, co jede do depa, jsem
zasekl sám mačkáním vynucení jízdy, aby nabourala. Svítí jí zelená. Celá ta
situace je poškozená zaseklou mašinkou na výjezdu z depa — **neřeš tu
mašinku, co jede do depa; způsobuje to ta zaseklá na výjezdu z depa.**

Co je na screenshotu 1 vidět: okno depa Sindinghattan, Vlak #8 se stavovým
řádkem „Nelze dosáhnout Sindinghattan východ, 0 km/h", Vlak #7 „0 km/h –
Jede se spojit do Sindinghattan východ", Vlak #9 „Porucha".

**Porucha** — opraví se po půl roce, ale **kouř tam zůstane navždy**.

**Screenshot 2 — okno mašinky ve vanilla originálu (OpenTTD 16.0-beta2).**
Takhle vypadá okno mašinky ve vanille. Dole je místo na ikonku service.
Zachovej velikost okna — máme okno větší na výšku, jsou tam ikonky
zatmavené v depu i mimo depo.

### Druhé kolo poznámek k #65

**Couple popředu** — dobrý na všech čtyřech nástupištích. Nikomu příkaz
neposkočil na další příkaz „jeď do depa", stejně jako u couple pozadu.

**Screenshot 1 — otáčení při výjezdu z depa.** Už to funguje líp, ale ještě
se mi povede to zaseknout na první dlaždici.

Nápověda hráče, kde ta chvíle je: *„jestli hledáš chvíli, od které
implementovat tuto zdařilou opravu, tak to je přesně ta chvíle, kdy mašinka
v depu přestane psát ve svém okně Zastaveno."*

Opět: při hromadném výjezdu z depa se mašinky s příkazem „jet se spojit"
připojily zezadu k vlaku před sebou.

Co je na screenshotu 1 vidět: Vlak #6 „Nelze dosáhnout Sindinghattan
východ, 0 km/h", Vlak #3 „Míří do Železniční depo Canington, 0 km/h",
Vlak #2 „Čeká na poruchu" (odtahovka v depu).

**Pořadí ikonek v okně mašinky** — když je mašinka mimo depo, prohodit mezi
sebou ikonky „otočit" a „odtah": otočit nahoře, odtah dole. Aby bylo pokud
možno stejné pořadí venku i v depu.

### Třetí kolo poznámek k #65

**Pořadí ikonek v okně mašinky mimo depo** má být shora dolů:
do depa, semafor, otočit, odtah, příkazy, detaily.
V depu jsou ikonky ve správném pořadí.

**Zašedlá „service" i venku i v depu, když má mašinka příkazy** — ať hráč
vidí, že tam je něco nového.

**Zatmavování čudlíku odtah funguje dobře.** Dám příkaz a ikonka odtahu je
hned zabarvená. Smažu příkazy, ikonka odtahu jde zmáčknout a dole v okně
mašinky se píše „Čeká na poruchu". To je dobrý.

**Odtahovka ještě nejezdí odtahovat** — ještě nemáme doladěné couple.
Až bude couple doladěné, a to se blíží.

**Poskočení příkazu po spojení:** příkaz nepřeskočí po spojení na další
příkaz, ale poskočí při dalším průjezdu nádražím — takže to není rozbité
hodně.

**Decouple a nakládání:** vagonky po decouple nezdědí od mašinky příkaz
„nenakládat" ani „plně naložit".
- Když je příkaz „plně naložit", mašinka se neodpojí, dokud není vagonek plný.
- Když je příkaz „nenakládat", mašinka se odpojí hned a vagonky nakládají.

**Go to couple na nakládající vagon** — mašinka se připojí k vagonkům
a odveze je.

**Nabouraná mašinka** zmizí za půl roku stejně jako porouchaná, a zůstane
tam kouř jako po porouchané.

**Ikonka pro odtah:** hráč dodal ikonku 12×12 pixelů (kladivo).
Někam se musí napsat atribuce: `icon8.com hammer`.
Poznámka: samotný soubor s ikonkou zatím není v repozitáři — až na to
dojde, bude potřeba dostat PNG sem, aby se dalo přidat do openttd.grf.

## Build #68 (commit 08a9123)

**Go to couple popředu i pozadu** — 3. a 4. peron objede zbytečně nádraží
dokola. (Známý otevřený problém.)

**Screenshot 1 — pád, `train_cmd.cpp:4657`**
`assert(chosen_track.Count() == 1 && !chosen_track.Any({Track::Wormhole, Track::Depot}))`
Hromadný výjezd z depa, mašinky s příkazem „jet se spojit" ztratily
orientaci: nejedou přes směrování se otočit, jedou rovnou spojit.

**Screenshot 2 — decouple bez cesty do depa.** Hráčská chyba (obráceně
otočený jednosměrný semafor), cesta do depa neexistovala. Jsou rozpojené,
stalo se to při odjezdu. **Podstata: neměla kudy jet.**

**Screenshot 3 — okno depa.** Vlak 3 má menu v pořádku, Vlak 2 má
rozpadlý poslední řádek. Stačí kliknout na příkazy a srovná se to.

**Okno příkazů na zastávce:**
- spodní velký čudlík „Odpojit: N" smazat úplně
- zadávání počtu vagonků má vyskočit po klepnutí na malý čudlík „Odpojit"
- prohodit čudlíky „Odpojit" a „Vycouvat" — fungují dobře, jen si vymění místo
- „odpojit" není v závorce v příkazech

**Odtahovka** nejede odtahovat, když je někde porucha nebo bouračka.
(Vyslání zatím není postavené.)

**Čeština:** příkaz „jeď do depa" má v závorce „(Zastávka)", ve vanille
tam má být „(Zastavit)".

## Build po plánku nádraží — co se opravilo a proč

Zápisy k úkolům 1, 2, 6, 7, 8 z posledního kola testů. Úkoly 3, 4 a 5 se
ještě dodělávají, odtah (jízda) se podle pokynu nemění.

**Úkol 2 — příkaz po spojení na 3 a 4.** Kód po spojení nezjišťoval, který
konec vlaku vede, on to nařizoval podle pořadí seznamu: "vede hlava". Na
nástupištích, kam se nacouvá, míří hlava a vedoucí konec na opačné strany;
na těch, kam se jede popředu, na tu samou. Takže pravidlo pojmenovávající
konec sedělo na jedné dvojici a bylo obrácené na druhé — a oprava jedné
dvojice rozbíjela druhou donekonečna. Teď se to měří: spojený vlak vyjíždí
tou stranou, kterou spojující mašinka přijela, tedy vede konec, který jí
byl při příjezdu vlečený. Na obou dvojicích to vyjde správně a nikde se
nejmenuje hlava ani konec seznamu.

**Úkol 8 — spojení už v depu.** "Čekat na spojení" je na příkazu dávno
předtím, než vlak někam dojede, takže mašinka cestou i v depu četla, že
čeká, a slepila se s první, na kterou narazila. Nově to platí, jen když
vlak opravdu nemůže nikam jet: buď stojí v cílové stanici a odbavuje se
tam, nebo je porouchaný či havarovaný.

**Úkol 6 — odtah bez zvláštního kódu.** Porouchaná i havarovaná mašinka
teď dostane "čekat na spojení" na svůj příkaz. V okně se dál píše porucha
nebo havárie, protože ty mají v hlášení přednost před příkazem. Odtah tím
přestává být zvláštní případ: jede po stejném hledání partnera, stejném
přiblížení a stejném spojení jako všechno ostatní. Příznak se ruší, jakmile
vlak zase jede sám.

**Úkol 1 — nejdřív cíl, potom rezervace.** Zamluvení bylo zapsané jen na
vagoncích, takže hledání cesty o něm nevědělo a mířilo ke kterékoliv jiné
volné řadě, která prošla filtrem. Mašinka si tak zamluvila jednu řadu
a trať si zarezervovala k jiné. Zamluvení se teď píše na obě strany —
řada ví, kdo pro ni jede, a mašinka ví, pro co jede — a jakmile má vybráno,
nic jiného pro ni partner není. Dokud vybráno nemá, nedrží před sebou
žádnou trať.

**Úkol 7 — výjimka ze srážky byla plošná.** Říkala "do bezhlavé řady
vagonků smí najet kdokoliv", takže do stojících vagonků šlo beztrestně
narazit čímkoliv. Nově platí jen pro tu jednu dvojici, která si o sobě ví:
mašinka a to, co si zamluvila. Kdokoliv jiný bourá, jak má. Tím to platí
stejně pro vagonky i pro čekající vlaky a nemusí se to řešit dvakrát.

## Úkoly 3, 4, 5 — co se udělalo

**Úkol 3 — vagonky mají dva příkazy.** Odpojená řada teď dostane skutečný
seznam dvou příkazů: nejdřív to, co jí mašinka nechala za práci na téhle
stanici, a za tím "čekat na spojení". Hráč si otevře běžné okno příkazů
(ikonka příkazy je v okně vagonků odemčená) a **zmáčkne běžné Přeskočit**,
když chce vagonky odvézt dřív, než se naloží — třeba když zrušili průmysl
a už není co nakládat. Žádné zvláštní tlačítko.

Automaticky se to posune samo, jakmile je nakládání opravdu hotové.

A "čekat na spojení" **současně s nakládáním** platí jen tehdy, když hráč
nedal naložit plné. Když dal, má řada práci k dokončení a partnerem se
stává až potom. Dřív byla nastavená obojí najednou, což je špatně a
umožňovalo to odvézt poloprázdnou řadu.

**Úkol 4 — nakládání ve stanici.** Ta funkce ve hře je a jmenuje se
`order.improved_load`. Zapnutá dělá to, že vozidlo čekající na plné
naložení si náklad ve stanici zamluví pro sebe, takže na vozidla u
ostatních nástupišť nezbyde nic, dokud neodjede. Vypnutá nechá nakládat
všechny najednou a dělit se o to, co tam je. **Neodstranili ji, jen jí
sebrali popisek**, takže se v nastavení nedala najít a šla nastavit jen
konzolí. Popisek dostala zpátky a je v nastavení hry.

**Úkol 5 — posun mapy pravým tlačítkem.** Řešeno v kódu hry, ne v ovladači.
Dvě tažení mapy naráz nemohou být rozdělaná, a z těch dvou vyhrává to,
které hráč právě začal — takže **stisk levého tlačítka ukončí tažení
pravým**. Tím se z toho dostane i tehdy, když hra považuje pravé tlačítko
za držené a nikdo ho nedrží: jakmile tažení skončí, další začne až po
novém stisku pravého, takže zaseklý příznak nemá čeho se držet.

## Pád při odpojení a odtah, který projel skrz

**Pád hry, jakmile mašinka odpojila vagonky.** `Assertion failed ... pool_func.hpp:119: this->checked != 0`. Seznam příkazů je objekt z fondu
(pool) a ten fond trvá na tom, aby se ho někdo předem zeptal, jestli má
místo. `CanAllocateItem()` není rada, je to povolení, které si sama
alokace kontroluje. Odpojení dávalo řadě vagonků její dva příkazy, a
neptalo se — jako jediné místo ve hře. Všude jinde se ptají
(`CmdInsertOrder`, `CmdSellRailWagon`). Když se místo najde, řada dostane
seznam; když ne, obejde se bez něj a čeká dál, protože čekání sedí na
živém příkazu, ne na seznamu.

**Odtahovka projela skrz porouchanou mašinku.** Nebourala a nespojila —
vypadla mezi. Příčina: dvě různá místa se ptala na dvě různé věci.
Výjimka ze srážky se ptala, jestli má jeden z nich zapsaného toho druhého
jako cíl; spojení se ptalo, jestli je vlak na výjezdu k poruše nebo má
příkaz jet spojit. Když se ty dvě odpovědi rozešly, srážka se odvolala a
spojení se nekonalo. Nově se obě ptají jednou funkcí: *smí tihle dva teď
spolu spojit?* Nemohou se rozejít, protože je to jedna otázka. Kdo do
spojení nepatří, bourá — porouchaná mašinka i vagonky na peronu.

**Zápis o výjezdu se rušil jen z poloviny.** Výjezd je zapsaný dvakrát:
mašinka si píše, pro co jede, a porouchaná si píše, kdo pro ni jede.
Postavit odtahovku mimo službu nebo jí zadat příkazy maže jen ten první
zápis a druhý nechává. Zůstala tak mašinka, která už pro nic nejede, ale
pro kód srážky pořád "cíl měla" — proto neboural. Ruší se to teď na
jednom místě a z obou stran.

**Odtahovka nevyjela vůbec.** Rezervace trati se ptá, jestli cestě
nepřekáží jiný vlak, a když ano, označí vlak za zaseknutý a čeká, až
překážka odjede. To je správně pro každý vlak kromě jednoho: pro ten,
pro který jsme vyjeli. Porouchaná mašinka má vpředu mašinku, takže se
čte jako obyčejná překážka — a čekat na ni nemá smysl, protože je
porouchaná a nikdy neodjede. Odtahovka proto stála zaseknutá u
posledního návěstidla před ní. Bezhlavá řada vagonků už tuhle výjimku
měla; teď platí pro cokoliv, s čím se ten vlak smí spojit. Trať se
zarezervuje kousek před porouchanou a zbytek obstará spojení.


## Zámek trati po spojení, který nikdy nezmizel

Trať se uvolňuje tak, že se jde od vedoucího konce vlaku **dopředu** po
zamluvených dlaždicích. Dosáhne se tím jen na cestu, po které vlak
teprve pojede.

Do 6b25fef spojený vlak vždycky jel hlavou napřed — bylo to tam
natvrdo napsané. Uvolnění po spojení bylo napsané pro tenhle stav, a na
nástupištích, kde hlava náhodou byla tím koncem, kterým mašinka přijela,
to vycházelo.

6b25fef začal vedoucí konec **měřit**, protože spojený vlak musí
vyjet tudy, kudy sběrná mašinka přijela. Tím se na půlce nástupišť
vedoucí konec obrátil — a cesta, po které mašinka přijela, se ocitla
**za** vlakem. Uvolnění po ní nikdy nešlo. Zůstala zamluvená do konce
hry: spojený vlak po ní nemohl odjet a žádný jiný vlak tudy taky nesměl.

Chyba byla v tom uvolnění celou dobu; jenom se neprojevila, dokud byl
vlak nucený zůstat otočený jedním směrem. Změnil jsem, co znamená
"vpředu", a nepřesunul jsem jediné místo, které na tom stojí.

Uvolňuje se to teď **před** spojením, dokud oba vlaky ještě stojí tak,
jak přijely. Nezáleží pak na tom, jestli mašinka přijela popředu nebo
pozadu — nepojmenovává se žádný konec, každý vlak uvolní to, co sám
drží.


## Pravé tlačítko se po mé opravě zaseklo víc — proč

Dvě věci, obě moje.

**Ptal jsem se zdroje, který o tom tlačítku nemusí nic vědět.** Čtení
stavu tlačítek každý snímek se ptá systému. Když ale ukazatel není myš —
dotykový displej, tlačítko na obrazovce, vzdálená plocha — může stisk
přijít jako zpráva, aniž by se představa systému o tlačítkách vůbec
změnila. Na takové tlačítko systém celou dobu odpovídá "nedrží se", a
brát to vážně znamená stisk zrušit v okamžiku, kdy vznikne. Nově se
tlačítko zvedne, až když ho tohle čtení aspoň jednou vidělo dole: buď o
něm systém ví a pak se dá věřit i tomu, že řekne, kdy se pustilo, nebo o
něm neví a pak se ho tohle nedotkne vůbec a zprávy si to řídí samy jako
předtím.

**Zahodil jsem úchop okna kvůli tomu, čemu se nedá věřit.** Úchop (grab)
je jeden, ne jeden na tlačítko. Puštění tlačítka ho smí zrušit, jen když
už se nedrží nic — jinak se zahodí úchop, na kterém stojí to druhé
tlačítko, a zpráva o jeho puštění pak jde do okna, nad které se ukazatel
mezitím dostal. Tam se ztratí a tlačítko zůstane dole navždy. Rozhodovalo
se to podle **zapamatovaného** stavu, a zapamatovaný stav je právě to, co
tady bývá špatně. Ptá se to teď systému.
