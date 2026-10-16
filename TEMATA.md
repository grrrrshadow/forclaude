# Témata (hra Decouple, forclaude)

Moje průběžné poznámky: co hráč chce, co jsme rozhodli, co je hotové a co zbývá. Píšu je vlastními slovy, nejnovější
nahoře. Commity jsou z větve `claude/github-connection-check-m6m898`.

## Pravidla práce (trvalá)

- Česky, krátce, tykat. Nejdřív domluvit, pak kódovat; dokud nejsou všechny informace, nezačínat.
- Chyba je v kódu, ne v obrázku: žádné zalepování příznaků. Návrh hráče je návrh, ne hotová věc.
- Build (Windows) dělá hráč, ne já. Commit a push po každém hotovém kroku.
- Žádná vyskakovací okna s povolením (nový host, add_repo apod.).
- **Mody a hra jsou dva oddělené světy.** GRF je mod: objekt místo spritu, jinak funguje jako dřív. Co GRF chce
  (barva, blednutí, rez, blikání majáku), vybírá GRF svým spritem (`stavy`). Hra sama nic nerozhoduje a nic do GRF
  nestrká. Kolega v grrrrf dělá GRF a objekty, já hru a prostředí pro ně.
- Zabudované ve hře: náš průmysl a budovy, náhrady domů (ÚV KSČ místo divadla 0x1F), výzdoba (holky na zastávkách,
  Mat, Tatra VB mezi budovami ÚV KSČ). Rozšíření GRF: Tatry, Sergej, V3S, divadlo MAXd – ve složce `decouple`
  se z nich stanou zabudované neodebíratelné GRF.
- Vanilla zůstává vanilla. Naše obrázky jen 32bpp, žádný grfcodec (jen yagl), kolegovy obrázky neupravovat.
- Kód OpenMW (GPL 3) nebrat, hra je GPL 2. Brát jen nápady. Example Suite od OpenMW je CC0 (modely smí).
- Kolegovi psát jen do `hra/...` v grrrrf.

## 16. 10. – světla vozidel z modelu, levnější noc

- Hráč: „auta musí mít vyznačená světla jako mají domy okna, kolem auta jsou světla metr daleko“. Kolega dal světla
  do modelů (Tatra v7, Sergej v6, herní Tatra VB): skla v uzlu `noc`, místa světel `svetlo_predni_NN`/`svetlo_zadni_NN`.
  Hotovo (`cd92535`): ve dne se skla nekreslí, v noci svítí, záře a kužel jsou z míst v modelu. Platí na otočené mapě,
  na sever i v pohledu z očí. Přední svítí, když vozidlo jede v čele, zadní, když za ním nic nejede.
- Hráč: „zlevni noční světla zblízka“. Dřív si v noci každý díl vozidla (i každý vagon) zvětšil rámeček o půldruhé
  dlaždice. Teď jen čelo a konec o kousek a kužel před autem se značí sám. V rigu zblízka (otočeno 30°): den 15,9 ms,
  noc dřív 28,7 ms a teď 20,0 ms na snímek, bez duchů.
- Světlo v noci (hráč: „zapínání noci necháme, kde je, v nastavení budeme nastavovat velikost tmy… nejen úplněk jasný,
  ale i skoro svítání“): Nastavení → Grafika, pod Nocí: tmavá noc (jako dosud), měsíc, úplněk, jasný úplněk, skoro
  svítání. Platí na mapě i v pohledu z očí a z kabiny, čím světleji, tím míň hvězd.
- 3D částice z OpenLoco: odloženo (hráč: „bojujeme se zpomalením hry, po buildu ti řeknu“).
- Windows build 278 spadl na `M_SQRT2` (MSVC ho nezná) – opraveno (`91c7b4d`). Při tom tři skutečné chyby z
  `-TILE_SIZE` (neznaménkové, vyšlo přes 4 miliardy): stěny základů v pohledu z očí se nekreslily, Mat nevybíral
  chodník u dveří, polní cesta v náhledu průmyslu ležela navrchu.
- Pohled z první osoby je i v nabídce nápovědy hned pod nocí (hráč: „dej to vedle přepínače noc a den“). V menu
  mapy zůstal.
- Simtrains: vůz a mašina byly natlačené do sebe. Příčina: hra dává vozidlu nejvýš 8/8 (asi 10,5 m), model 750 má
  16,24 m a B 598 24,2 m, takže se kreslily přes sebe. Teď GRF podle `Size` z .eng/.wag udělá dlouhé vozidlo kloubové
  (750 ze 2 dílů, B 598 ze 3) a model se kreslí přes všechny díly. V rigu jsou mezi vozy mezery.
- Úvodní hra (pozadí menu) je v 16.0-beta2 arktická, 15.3 měla toyland. Hráč chce mírné klima. Ptám se, jestli dá
  vlastní save, nebo ji mám postavit sám.

## 16. 10. – kolegovy domy a průmysl, hospoda 2×2, Tatra v6

- Hráč kolegovi řekl, ať jsou holky v hospodě větší. Kolega proto zvětšil celou hospodu na 2×2 (hráč: „zvětšil
  hospodu na 2x2“). Ve hře hotovo (`86440b3`): nová hospoda zabírá 2×2 (dům a zahrádka). Stará hospoda 1×1 ze
  starých her zůstává malá a kreslí se spritem.
- Holky u průmyslu po dovozu: socha, gymnázium a automat 30 dní po studentkách (u automatu po marihuaně). U chatky
  stojí po studentkách a sedí, když přijde i marihuana. Platí na mapě, v náhledu průmyslu i v pohledu z očí. V den,
  kdy odejdou, se průmysl překreslí. Rig: `test3d dovoz`.
- Svítící okna v noci teď mají i gymnázium, lihovar, automat a hospoda (girlanda nad zahrádkou). Holky domů
  mírného klimatu jsou dvakrát větší. Všechny klimaty a Mars mají noční okna.
- V pohledu z očí mají postavy na budovách a polích skutečnou velikost, předmět (rožeň) zůstává velký jako budova.
  Proti dvakrát větší hospodě proto holky vypadají malé – kolegovi jsem to napsal, ať řekne, jestli to chce jinak.
- Tatra v6 (kolega): ověřeno v `newgrf/` i v `baseset/decouple/`, stavy se najdou. Když leží dvě verze vedle sebe,
  bere se balíček u načteného GRF. Když GRF kreslí prázdné obrázky a balíček chybí, hra to jednou zapíše do logu
  (s tím, kde hledala). Odpověď kolegovi: `hra/3d/ODPOVED-PRUMYSL-HOLKY-TATRA-V6.md`.
- Čeká: víko motoru a maják VB (`dily`, `doplnky`) podle stavu z GRF; 3D částice (OpenLoco) – čekám na hráčovo
  ano a na to, jestli zlevnit i světla v noci zblízka.

## 16. 10. – OpenLoco, luk, průjezd obloukem

- OpenLoco je MIT: jeho kód smí do hry (s poznámkou MIT), původní obrázky z Locomotion ne. Hráč vybral:
  1. podvozky ano, 2. naklánění ano (i auta, ale jen ta z objektu: Sergej a Tatra VB), 3. efekty jako 3D částice
  ano, 5. víc tvarů trati ne; 4. (cíle scénářů) bez odpovědi – nedělám.
- Luk a šíp: objekt je v Example Suite hotový (CC0), vezmu si ho sám, nečekám na kolegu.
- Průjezd obloukem pomalý: změřeno v rigu (`test3d obloukjizda`, `19f8ff4`). Kreslení po oblouku jde stejně
  rychle jako vlak po koleji (rozdíl pod 1 jednotku na dlaždici). Zpomalení je pravidlo hry (realistické
  zrychlování): vlak s vagony v těsné zatáčce (1 kus diagonály mezi rohy) smí jen 88 km/h, ze 128 km/h zabrzdí
  na jedné dlaždici. S kulatými oblouky vypnutými je to na tik stejné. Zatáčka s 5 kusy diagonály: limit 168,
  vlak nebrzdí. Hráč: rychlosti podle vanilly (ostrý oblouk míň, pozvolný víc), nakreslené oblouky jim fyzikálně
  odpovídají (87/130/168 km/h). Hotovo `18d0164`: volba hry „Vlak brzdí plynule už před zatáčkou“ (vypnutá =
  vanilla) – strojvedoucí vidí ohyby dopředu a brzdí podle brzdné tabulky, do těsné zatáčky vjede 88 km/h.
- Simtrains (hotovo `74f95d1`): hráč dá stažené MSTS vlaky do `newgrf/simtrains/`, hra vedle každého `.eng`
  napíše GRF jedné lokomotivy (čísla z „chování“: výkon, rychlost, váha) a kreslí ji z jejího modelu `.s` a
  textur `.ace`. Nové vozidlo, ne převzetí vzhledu. Soubory MSTS nikdy do repa, grrrrf ani buildu (readme autorů
  zakazuje šíření i do jiných simulátorů). Pak doplněno: DXT textury (`cdf384f`), textové (starší) modely a
  vagony `.wag` (`132ae28`) – osobní vůz veze cestující podle PassengerCapacity, nákladní cokoli kromě cestujících.
  Hráč poslal na pokusy vůz ČD B 598 (skutečné DXT1 textury) – jede za 750. Nečte se: náklad (freightanim),
  světla, zvuky.
- Kde hra vlaky hledá (`ef4e8ac`, hráč: „ať hledá obvyklou složku simtrains modelů“ + „v newgrf simtrains taky“):
  Trains/Trainset MSTS (registr, Open Rails, obvyklé cesty), `simtrains_path` v `[misc]` openttd.cfg a
  `newgrf/simtrains` pro hráče bez MSTS. GRF s čísly jdou do `newgrf/simtrains/`. Hráč se ptal na šíření: modely
  ani GRF nesmí ven; save a obrázky jsou v pořádku.
- Hráč: bez okna pískoviště je myš cukavá a všechno zpomalené. Příčina (`d608a9f`): otočený pohled při každém
  pohybu čehokoli (vlak, Mat, holky, auto VB) překresloval skoro celou obrazovku, protože nevěděl, v jaké výšce
  věc je. Teď každé označení nese místo ve světě. Rig 1000×700: vlak 670 000 → 61 000 px na snímek, město
  186 000 → 4 300 px, bez duchů. Okno pískoviště jen zakrylo kus mapy (v rigu pomohlo málo).
- Postavy jsou na mapě (mimo kabinu) schválně zvětšené, normální velikost jen v pohledu z první osoby.
- Přepínač velikosti v nastavení grafik: jen „zvětšené / zmenšené“ (`3714835`).
- Lokomotivy ŽSSK 750 (trainset.simtrains.eu, Miku031, modely Ondra Janiš): stránka neuvádí žádnou licenci,
  tedy bez svolení autorů (modelu, skinů i fotek) do hry nesmí.

## 16. 10. – technologie z OpenMW (hráč: „1–9 ano“)

- Schváleno všech 9: kreslení na více jádrech, mezipaměť nehybných objektů kusu mapy, LOD (vzdálené věci
  jednodušeji), kostra a plynulé animace, hledání cesty (navmesh), kolize podle tvaru, stíny a světlo po pixelech,
  instancování (kytky, tráva), aktivní okolí jen kolem hráče. Jen principy, kód OpenMW ne.
- **1. Více jader** (hotovo `9245a60`, `725aea1`, `013b382`): pohled z očí kreslí zem, domy, opar i noc po pásech
  řádků na všech jádrech, pásy si vlákna berou postupně (práce je u horizontu). Domy se připravují každý na svém
  jádru, odvrácené stěny a co je mimo obraz se vynechá. Textury domů se dělají paralelně (první pohled na město
  dřív 2 s). Obraz je na 1 i 4 vláknech bit po bitu stejný.
- **3. LOD domů** (hotovo `013b382`, `fdcfb9c`): kolegovy domy mají 12–25 tisíc trojúhelníků, město bylo půl
  milionu. Vzdálený dům má zjednodušený tvar (rohy sloučené v buňkách 5 cm až 3,2 m); v pohledu z očí se roh smí
  posunout o 1,5 pixelu (dálka je v oparu), na mapě o ¾ pixelu (při 1,5 mizely příčky oken).
- Stálá vlákna kreslení (`baedec6`): založí se jednou a čekají na práci (na Windows je zakládání drahé). Malé domy
  na mapě na víc jádrech pomalejší (synchronizace) – zůstávají na jednom, velké po pásech.
- **2. Mezipaměť domů na otočené mapě** (hotovo `3bf95f4`, „object paging“): dům se pro úhel, zoom a místo vykreslí
  jednou do obrázku s hloubkou, pak se jen kopíruje; postavy a noční okna přes něj. Nejvýš 256 MB. Při otáčení
  a zoomu se neukládá (každý snímek jiný), naskočí 0,2 s po zastavení. Zoom 2: 106 → 22 ms, zoom 3: 141 → 17 ms.
- **3. Vzdálená krajina** (hotovo `4f3fa98`): za 20 dlaždicemi zem až do 96 dlaždic v blocích 1–8 dlaždic, výšky
  z mapy, barva z průměru skutečných spritů (tráva, voda, pole, poušť, popel, lesy), sukně proti škvírám. Domy
  a průmysl jako 3D objekty do 64 dlaždic (zjednodušené), v noci jim svítí okna – města na obzoru. Opar plynule
  s dálkou, mlha od 60 % dosahu.
- **4. Kostra a prolínání** (hotovo `9f83df2`, `e6f4c28`): kolegovy `drak_*_kostra.glb` (klouby, váhy, animace)
  – póza draka se počítá každý tik (chůze 64 snímků místo 8), při přechodu chůze → útok → smrt se pózy 8 tiků
  prolínají kost po kosti. Čínský drak (bez kostry) zůstává na snímcích. Rig: `test3d postava <figura> <snimek>`.
- **5. Hledání cesty** (hotovo `3f2145b`): draci chodí kolem domů, vody a strmých svahů (A* po dlaždicích,
  narovnaná cesta), přistávají jen kde můžou stát. Mat už chodil po síti chodníků. Rig `test3d draci cesta`.
- **6. Kolize podle tvaru** (hotovo `92c7556`): chodec naráží do skutečného půdorysu 3D domů ve výšce těla (park,
  fontána, terasa průchozí; dutý vnitřek plný). Rig `test3d pudorys x y`.
- **7. Stíny a světla** (hotovo `fe31f5f`): ve dne stíny domů od slunce (mapa stínů, přepočet jen při změně okolí),
  v noci okna a lucerny nasvítí zeď a chodník pod sebou po pixelech. Rig `test3d stiny 0`, `test3d lampy 0`.
- **8. Instancování** (hotovo `ecd273d`): pole v pohledu z očí jako 3D – stovky rostlin konopí sdílí jeden model
  a pár obrázků, model jen u nohou; figury se kreslí hromadně na všech jádrech. Rig `test3d pole`.
- **9. Aktivní okolí** (hotovo `1c81c6c`): co se 2 minuty nekreslilo (textury, tvary, pózy, přechody), se pustí
  z paměti a při dalším pohledu vyrobí znovu. Rig `test3d zapomen`.
- Rig (4 vlákna): pohled z očí v městě 116/61 ms → 70/32 ms (1/4 vlákna); otočená mapa nejvíc oddálená
  102 → 68 ms. Na víc jádrech u hráče to bude rychlejší.

## 16. 10. – co dál z Morrowindu

- Hráč vybral: nastoupit do vozidla (ano), kouzla bez teleportů (léčení, levitace), dovednosti rostoucí používáním,
  kompas a malá mapa; zbroj až potom; výdrž, plavání, zvuky ne; truhly později. Kytky už jsou.
- **Mapa pod pohledem z první osoby přes celou obrazovku se nekreslí vůbec** (hotovo `fc46c5c`).
- Hráč: **luk a šíp** z Example Suite – prosba kolegovi `hra/3d/PROSBA-LUK-SIP.md` (luk, nebo kuše se šipkou;
  uzly `uchyt`, `zarez`, stavy `volno`/`napnuto`). Hra: 1 kladivo, 2 luk, levé tlačítko napne a vystřelí.
- Hráč: **„já chci technologie z OpenMW, ne spánek a plavání“** – tedy techniky enginu (kreslení, výkon, animace,
  pohyb), ne herní nápady. Kód dál nebrat (GPL 3), jen principy.

## 16. 10. – oprava

- Hráč: v buildu chyběl břeh pod domem na svahu u vody. Příčina v kódu (`9e6fdfc`): na sever s domy jako objekty
  zůstala podezdívka, ale zem na ní (její děti) jsem vynechával – kolegovy domy vlastní zem nemají, tak byla díra
  (černá, nebo prosvítala voda). Zem na podezdívce zůstává; otočený pohled byl v pořádku.

## 15. 10. – Z OpenMW dál (hráč vybral)

- **Ano:** klávesa E „použít“ a interiéry za dveřmi (truhly, prodavač); noční obloha v pohledu z očí (hvězdy,
  měsíc – počasí ne); draci a boj kladivem; spánek/čekání (T), deník úkolů, tráva a kytky kolem chodce.
- Běžná města: „všech domů“ v okně města necháváme (hráč: „necháme to být“). V mírném klimatu stejně staví jen
  mírné domy (ověřeno v rigu).
- Draci od kolegy (`objekty/tvorove/`) mají animace jako snímky v uzlech (klid, chůze, útok, smrt, let) – boj jde
  udělat hned, bez kostry.
- Hráč: interiéry zatím nejsou – plánované jako samostatný objekt, kam se chodec přesune vstupem do dveří. Draci
  občas přilétnou kamkoli, útočí na chodce, za zabití nic (hráč má draky rád). Zdraví 0 → konec pohledu z první
  osoby, zpět na mapu. Pohled z první osoby přes celou obrazovku (hotovo `ba62032`, obraz jen jednou za krok).
- **Noční obloha** (hotovo): hvězdy (pevně na obloze, mihotají se) a měsíc se září, dělá je hra sama (Example Suite
  tu nemám, stahovat z nového webu nechci).
- **Draci** (hotovo): kolegovi draci (`objekty/tvorove/`) občas přiletí kamkoli (nejvýš 3 najednou, asi jednou za
  dvě minuty), s otevřeným pohledem z první osoby spíš k chodci. Hnědý a černý přistanou, chodí, do 45 m jdou po
  chodci a útočí (rána 12 zdraví), po 6 ranách kladivem padnou a chvíli leží, za zabití nic. Ledový a čínský
  přelétají a jednou se snesou (8 zdraví). Rány jdou přes příkaz `HurtWalker`, obraz při zásahu zčervená. Na mapě
  (otočený pohled, na sever s objekty) i v pohledu z očí, zblízka (do 3 dlaždic) jako skutečná 3D síť, ne obrázek.
- Hráč: **spánek nebude vůbec**. Úkoly může dávat kdokoli, až je vymyslíme. Trávu a kytky domluví hráč s kolegou.

## 15. 10. – Morrowind: tématická města

- Hráč: tématická města Morrowind. **Hliněné domy** (dunmerské, s kopulemi) – dělá je kolega. Stromy Morrowindu jen
  v těch městech. Obecně: kolem každého tématického města jeho stromy na jeho území; Morrowind **popelavá zem**,
  pouštní tématická města **poušť** na svém území. Jména měst **Drakov, Houbovice, Čarodějov**. Počet přes nastavení
  tématických měst (jako Mars).
- Tamriel Rebuilt nebrat: jeho modely smějí jen do modů Morrowindu (licence Bethesdy). Styl napodobit smíme.
- **Hotovo:** kolega udělal 13 hliněných domů a 4 houby (`objekty/hlinene/`). Ve hře: téma města (`Town::theme`, v savu),
  nastavení „Hliněná města“ (výchozí 3), první tři Drakov, Houbovice, Čarodějov. Staví domy mírného klimatu jako každé
  město, ve 3D (otočený pohled i na sever s domy jako objekty) jsou hliněné; stromy na území města (zastavěná část
  a 4 dlaždice kolem) jsou houby podle růstu stromu.
- **Svítící okna** (hotovo): kolega dal do hliněných domů uzel `noc` (svítící okna a lucerny), prázdný `den`
  a body `svetlo_NN`. Hra v noci kreslí `noc` přes dům a nechá ho svítit (noc ho nezatmí), u `svetlo_NN` přidá
  teplou záři; na mapě i v pohledu z očí. Kolega (`PROSBA-NOC-DOMY.md`) dodal noc i do 38 mírných domů (1950,
  ÚV KSČ, po 1955, bývalé budovy3d) – převzato. Záře jen u okna, které je opravdu vidět.
- **Popelavá zem** (hotovo): tráva a hrubá zem na území hliněného města ve 3D šedá (paleta vraků). Houby i v pohledu
  z očí (plakáty z jejich obrázků, zblízka jemnější).
- **Poušť** (hotovo): pouštní tématické město (staví jen pouštní domy v jiném klimatu) má na svém území písek
  z pouštního klimatu (načtený zvlášť, v mírném klimatu jsou na jeho místě sněhové sprity) a stromy jako palmy
  a kaktusy; ve všech pohledech, i ve 2D.
- Území tématického města: zastavěná část (vnější zóna) a 4 dlaždice kolem, nejbližší město.

## 15. 10. – Morrowind: pohled z první osoby, inventář, noc

- **Pohled z první osoby** (hotovo, `251b575`, mlha `895258e`): nabídka mapy → *Pohled z první osoby*. Chodec
  stojí, kam se dívá hlavní pohled; oko 1,55 m. W/S nebo šipky chůze, A/D úkrok, šipky ←→ otočení, pravé tlačítko
  myši rozhlížení, Shift/Běh. Nevejde do domů, budov a vody. Mlha do dálky.
- **Noc** (hotovo, `436ac5c`): nápověda → *Noc* pod konzolí. Mapa ztmavne, vozidla svítí (přední světla s kuželem,
  zadní červená, letadla maják). I v pohledu z očí, světla zakrytá tím, co je před nimi.
  Hráč: „nic nezapomeň, světla“ → zbývá: **lampy u silnic a svítící okna** (kolega dodá uzly `noc`/`den`, `svetlo_NN`).
- **Tatra VB bliká:** rozhoduje GRF (stavy), hra sama ne. Kolega: bliká jen naložená (s 1 osobou). Hotovo u něj.
- **Inventář** (hotovo `e56699b`, klávesa I v pohledu z první osoby): hráč: normální inventář jako v Morrowindu. Věci: **velké kladivo** (hráč prošel celý
  Morrowind s velkým kladivem, má ho od začátku), zbroj, boty, pivo, cigarety, mari, zboží. Budeme přidávat.
  - ukládá se do savu, **platí se penězi firmy**;
  - pivo v hospodě, mari u automatu a v hulírně, cigarety a zboží v obchodech ve městě;
  - **pivo léčí, mari léčí víc**; ostatní se zatím jen nosí. Oblékání a kouzla později.
  - **kladivo v ruce** (hotovo): kolegův model z Example Suite (CC0, `objekty/predmety/`). F tasí a schová (jen když
    ho chodec nese), levé tlačítko nebo mezerník máchne: nápřah nad hlavu, úder dolů před sebe. Truhla je taky
    převzatá, zatím nikde nestojí.
  - v pohledu z očí se náš průmysl (hospoda, lihovar, gymnázium…) kreslí objekty jako domy; dřív plakát ve vzduchu.
- **Build 277: v mírném klimatu nejsou města** (hráč) – vyřešeno: hráč měl aktivní jiný, vadný GRF, který rušil
  domy. Bez něj domy jdou. Slabina kódu zůstává: s nastavením „dělení měst podle GRF domů“ (`economy.split_house_grfs`)
  může jedna vadná sada domů zrušit všechna města – nabídl jsem, že se město pak postaví ze všech domů.
- Dál z OpenMW (kolegův `hra/3d/NAVRH-Z-OPENMW.md`): kostra a animace z glTF (135 animací postavy, ruce, sekání),
  interiéry dveřmi, boj zblízka, draci na mapě (`hra/3d/PROSBA-DRACI.md`).

## 15. 10. – průmysl, domy, noc

- Náš průmysl z kolegových objektů (`060253b`): gymnázium, sochy, automat, chatka; plantáž z dílů pole. Sady
  `budovy3d_5`–`8` (176 MB) i kód renderů z 8 směrů pryč. Hloubka po pixelech v objektu (prapory ÚV KSČ).
- Domy po roce 1955 od kolegy (`82286c4`): slunečníky, deštníky.
- Bramborové pole, lihovar, hospoda jako průmysly hry (`7ae1f57`); při 0° se náš průmysl kreslí objekty jako domy.
- Rožeň v hospodě se točí, střídá čuníka, kravičku, ovečku (`58ca084`).
- Podezdívky pod domy na svahu při 0° (`9e6fdfc`): na snímku byly černé díry.
- Dům 0x1F: kreslí se jen ÚV KSČ; staré `budovy3d_*.grf` si hráč smaže ze složky hry.
- „Gymnázium ve vodě“ byl náhled vedle okna průmyslu (průhledný podklad, tak to hráč chtěl).

## Otevřené

- Oblékání, kouzla, boj (zásah kladivem).
- Města Morrowind, území tématických měst (stromy, zem).
- Vadná sada domů nesmí zrušit všechna města (`split_house_grfs`) – jen když hráč chce.
- Lampy u silnic v noci; svítící okna dalších domů (až je kolega dodá, hra je umí).
- Kostra a animace glTF; draci; interiéry; boj.
- Klínek sousedního domu přes průčelí (#41). Domy z GRF objektem z balíčku (MAXd, #47). Doplňky (maják) podle stavu GRF.
