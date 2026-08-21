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
