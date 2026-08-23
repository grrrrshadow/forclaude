# Záznam testů

Doslovné poznámky od hráče z testování Windows buildů. Nic se z toho zatím
neřeší — vyhodnotí se to najednou, až budou testy hotové.

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
