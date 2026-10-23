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
- Repo `grrrrshadow/cota` (Winlator DL, hráčův Winlator na telefonu) číst jako grrrrf: neklonovat celé, jen
  `git clone --filter=blob:none --no-checkout --depth 1` do scratchpadu a `git show origin/main:<soubor>`.
- **Myš: vanilla režimy neměnit, jen náš režim** (`map_drag.cpp`) a naše otáčení prostředním tlačítkem. Opravy
  Windows/Wine/Winlatoru nedělat ani na ně nespoléhat – „musíme spravit hru, ne předělat deset let starou chybu“.
- **Velikost holek určuje hráč** (17. 10.: „vyser se na kolegu a udělej ty holky velikost… jestli potom někdo změní
  velikost holky, tak ho smažu“). Holky u domů a za volantem: pravá velikost (`meritko` 1 v `objekty/mirne`).
  Hráčovy vlastní velikosti nechat: Pajda V3S, zastávka, socha, automat. Hospoda a pole: hráč neví, nechat jak jsou.
  Při přebírání objektů od kolegy velikost holek nepřebírat, změnu nedělat bez hráče.

## 23. 10. odpoledne – rozhlížení pravým bez dorazu, dvouklik srovná hlavu, deník stojícího auta

- Hráč: *„otáčím se šipkami po velkých skocích, 360° je asi 30 snímků; pravým je to jemnější, ale má doraz na 180;
  dvouklik pravým srovná jen nahoru/dolů, hlava zůstane otočená“*.
- **Doraz na 180 – příčina:** pohled z očí nebral pravé tlačítko jako naše tažení mapy: ukazatel se nevracel na místo
  stisku, dojel na kraj obrazovky (od středu 640 px × 0,005 = 3,2 rad ≈ 183°) a dál nebylo kam. Teď je ukazatel
  přišpendlený jako u našeho tažení mapy (`_look_pinned`, ve Windows i oříznutí do okna kvůli Wine) – otáčí se dokola.
- **Hlava zvlášť od těla:** pravé tlačítko točí hlavou (`head`), dvouklik pravým vrátí pohled tam, kam tělo míří, a
  rovně. Jakmile jdeš, tělo se otočí kam koukáš a jde tam (pravým se dá řídit chůze). Šipky točí celým tělem.
  Rig `test3d chodec rozhled <dx> <pohybů> [chuze]`.
- **Velké skoky šipkami = málo snímků z očí na telefonu** (~10/s). Rychlost otáčení neměním (hráč: příčinu, ne
  následky); čeká se na `snimky.log` z buildu s 54c155c+, kde je vidět, co snímek z očí brzdí.
- **Auto nejelo** (hráč: *„stopnul jsem auto před depem, došel k němu z očí, chtěl jet, ale jen se točilo doleva
  doprava“*): v rigu se zastavené auto šipkou nahoru rozjede (i u depa) – příčinu nevidím. Proto **deník `jizda.log`**:
  když je zmáčknutá šipka nahoru a auto 2 s stojí, zapíše se všechno, co ho drží (zastaveno, porucha, volná jízda,
  omezení, dlaždice před ním a jestli se na ni smí, rozkaz, trasa, okno šipek, co šipka udělala naposled).
  Rig `test3d stopni <typ> <číslo>`.

## 23. 10. – dvojí šipka a pomalé snímky, lidé z očí vyšší, tvorové na mapě 3×, deník snímků

- **Dvojí šipka „fungovalo dvakrát a pak ne“ – příčina:** hra pozná stisk tak, že jednou za snímek koukne, jestli je
  šipka dole (`_dirkeys`, `InputLoop()`). Rig `test3d chodec tuktuk <ms snímku>`: do 100 ms na snímek dvojí stisk
  chodce rozejde (5,7 m za 2 s), od 150 ms se rychlé puštění mezi stisky ztratí a dva stisky jsou jedno držení. Hráč:
  *„oprav to pomalé snímkování, příčinu ne následky“* – stisky z událostí kláves (moje první oprava) **vráceny**,
  řeší se snímky. Pozor: 250 ms v logu myši ze 17. 10. jsou jen interval zápisu řádku „dotaz na systém“, ne snímek
  (na mapě tam zprávy chodí po 20 ms). Na tomhle stroji (rig s OpenGL, obraz z očí na vlákně) hlavní smyčka 60/s,
  sběr z očí 5–6 ms, kreslení na vlákně ~73 ms (klávesy neblokuje). **Na telefonu nevíme – měří to deník.**
- **Deník snímků `snimky.log`** (`frame_diary.cpp`, ve složce hráče): každých 5 s řádek: pohled (mapa/oči/kabina),
  snímků za sekundu, nejdelší snímek a z čeho (buffer, zámek = čekání na tik hry, klávesy, okna, obraz), průměry,
  tiky hry na svém vlákně, sběr z očí (hlavní vlákno) a kreslení z očí (jeho vlákno). Max 5000 řádků.
- **Mat, holky, prodavači z očí o hlavu vyšší** (×1,15, hráč: *„chci jim koukat do očí“*); jen z očí, mapa beze změny.
- **Tvorové na mapě 3× větší** (čarodějnice, draci, strašidla, zvířata, psi; prodavači ne – lidé jako holky), i jejich
  světla a překreslovaná místa; vznášející se (duch) o tolik výš. Z očí beze změny.

## 22. 10. – par14 od kolegy ve hře, strašidlo v ulicích, pes před hráčem

- Hráč: *„strašidla potkám jen Billa…, mushroom warrior… a draka; ostatní strašidla nejsou“*, *„já poslal víc
  strašidel přece“*. Pravda: **par14 kolegy (grrrrf e860869, 740df57, 20. 10., `hra/3d/PAR14-PRO-HRU.md`) se do
  sady hry nedostal.** Převzato beze změny do `objekty/`: strašidla trpaslík (ve dne, kolem hliněných měst, mírumilovný),
  Baba Jaga a stará Baba Jaga (noc, kolem měst mírného klimatu, útočí), čarodějnice s koštětem (noc, útočí); psi
  chodící pes, vlče, černá krysa, pohyby přenesené mezi psy (`sedi`, `lezi`, `klid`, přechody), vlk bez slin; tři
  kreslené krysy (zvířata kolem hliněných měst); nová pochodeň. Springfield (Moe, Lenny, auto Simpsonových) ne.
- **Farmářka (prodavačka psů) ve hře není** – vyřazená už 19. 10. (b00341d); při převzetí par14 jsem ji omylem
  vrátil, hráč: *„tu farmářku jsme dávali pryč přece“*. Zase pryč (záznam z naší kopie `psi/objekty.json` i glb).
  **Při každém převzetí `psi/` od kolegy ji vyřadit.**
- **Válečné kladivo** (`kladivo_valecne`, hráč: *„kladivo iddqd“*): nová zbraň inventáře (WalkerItem 22), dává ho
  `iddqd` jako ostatní zbraně, do ruky klávesou 8, máchá se jako kladivo (dosah 2,7 m, zásah 2).
- **`kdy: "den"`** hra čte (jen ve dne; `noc` jen v noci; bez `kdy` pořád). Strašidla až 3 najednou (bylo 2).
- **Pes před hráčem** (PROSBA-PAR14 bod 1, hráč 19. 10.): jdeš – pes běží před tebe (5 m ve směru chůze) a jde
  před tebou; stojíš – zastaví, otočí se k tobě a střídá sedí/leží/rozhlíží se s přechody (sedání, lehání,
  vstávání), když je model má; rozejdeš se – vstane a běží před tebe.
- **Strašidlo s prostěradlem** (kolegovo `carodejnice/strasidlo`, ghost od remi136): hráč 22. 10.: *„asi neměl
  útočit, proto byl létající. on by měl lítat po ulici jako chodci“*. Náš záznam v `objekty/nase/objekty.json`
  (`strasidlo_ulice`, model kolegy beze změny, jeho index se nemění): v noci se vznáší ulicemi ve výšce chodce
  (`vyska_m` 1,15 – střed těla nad zemí), rychlostí chůze, mírumilovné; `nahrazuje: "strasidlo"` ho stáhne z oblohy.
  Nové klíče hry: `vyska_m` (vznáší se nad zemí), `nahrazuje` (náš záznam místo nočního letce).
- Noční letci (3 čarodějnice, přízrak) létají se chodcem venku ze dvou třetin k jeho městu (dřív náhodné město).
- Draci: útočí jen Cethielův drak (hnědý/černý), jen on má animaci útoku; kráčející drak jen chodí, ledový
  a čínský létají. Návrh (zatím neschválený): ať útočí i kráčející.
- 3D GRF z par14 (Tatra osobní v11, Vagony 3D v3 s Mikádem) hráč ve hře má. Chybí pára z komína Mikáda (uzel `komin`).

## 21. 10. večer – rolovací inventář, chůze dvojí šipkou, semafory v obloucích, auto přes koleje, pád 0xFFFF

- **Inventář se roluje** (`walker_inventory_gui.cpp`): vpravo posuvník, kolečko myši; řádky se kreslí jen v okně
  (poslední řádek byl rozbitý, protože se kreslil přes okraj okna). Dole jde koupit i to poslední (Hospoda, Matylda).
- **Z očí: šipka nahoru (nebo W) dvakrát rychle za sebou** (do 0,4 s) – chodec jde sám bez držení; dolů dvakrát –
  couvá sám. Další stisk nahoru/dolů ho zastaví a (22. 10., hráč: *„v levo v pravo vypne tu chůzi bez šipky“*) i
  doleva/doprava. Zastaví i nástup do auta a když šipky patří jinému oknu. Rig: `test3d chodec dvakrat <mezera>
  <snímky> [bok]`.
- **Autojízda** (22. 10., hráč: *„jo a autojízdu“*): v kabině i za volantem z očí šipka nahoru dvakrát rychle –
  plná rychlost (bez omezení) a auto jede samo i s puštěnou šipkou; šipka dolů brzdí, doleva/doprava řídí (jízdu
  nevypíná). Rig: `test3d autojizda <typ> <číslo>`.
- **Semafory v kulatých obloucích:** dlaždice se semaforem už oblouk nerovná (dřív `IsSimpleTile` semafory
  vylučovalo); semafor stojí vedle zakřivené koleje, posunutý a natočený s obloukem (`RoundTrackSignalAt()`).
  Rig: `test3d oblouky semafory`.
- **Auto mimo silnici přes koleje kdekoli** (`RoadVehFreeCanEnter`), ne jen na přejezdu; vlak ho na kolejích srazí
  jako na přejezdu. Chodec přes koleje chodit uměl už předtím (ověřeno v rigu).
- **Pád „String 0xFFFF is invalid“** (#291, 30. 12. 2010 na začátku dne, bez zásobníku, dmp prázdný): příčinu
  ze záznamu nevyčtu. Hra kvůli neplatnému textu už nepadá: místo něj ukáže „(?)“ a do `texty.log` ve složce hráče
  zapíše číslo textu, den a v jakých textech to bylo (prvních 50). Rig `test3d text 0x...` přeloží čísla z logu.
- Deník karty z #291: země otočené plochy se na telefonu kreslí na kartě, obrázky 3–40 ms, bez chyb.

## 20. 10. – pohled z očí na kartě (GPU krok „pohled z očí“), deník karty

- Hráč: *„z pohledu očí se to má kreslit gpu, všechny objekty přes gpu“*, *„kresli jak se má kreslit, aby to
  necukalo, nehraju to jen já, nepotřebuju hru extra na míru mýmu telefonu“*. Odsouhlaseno už 18. 10. (GPU body 1–6).
- **Hotovo – domy a budovy z očí na kartě** (`ThreeDGpuRenderEye()`): všechny domy a průmysl snímku najednou, se
  stejnou kamerou, texturami, světlem trojúhelníku a vynecháním odvrácených stěn jako procesor; hloubka na kartě
  `blízko / vzdálenost` jako float (32 bitů), zpět obraz a hloubka a vloží se tam, kde je blíž než terén. Tvar domu
  je na kartě jednou (číslo `ThreeDObjectShapeView::uid`). Co karta neudrží, kreslí procesor jako dřív. Rig: obraz
  z karty a z procesoru se liší v 0,1–0,2 % pixelů (hrany).
- **Hotovo – auta, postavy, zvířata a houby z očí na kartě** (`EyeFigureOnCard()`): tvar, díly stavu a textury jako
  na procesoru, rohy maticí (otočení stavu, velikost, zkrácení, náklon do kopce, natočení), světlo v rámci tvaru,
  sklo průhledné přes to, co je za ním, posádka, která chodí. Rig: Tatra 603 vedle chodce,
  karta a procesor se liší v 6–7 pixelech. Pozor na zjednodušený tvar: nemá seznam textur, počet textur je ze
  seznamu textur figury (jinak kola dostala texturu karoserie).
- **Hotovo – krok 1: vlastní kabina, točící se kola a noční svit na kartě.** Kabina na kartě bez místa řidiče (oko
  je uvnitř). Kola maticí na kartě: otočení podle ujeté dráhy a zatočení předních podle zatáčky, stejně jako
  procesor (nejvýš 32 kol na tvar, víc kreslí procesor). V noci svítící materiály do druhého výstupu karty (záře);
  vloží se přes tmu tam, kde je figura nejblíž. Rig, karta proti procesoru: noční houby u Drakova 13–19 px, Tatra 603
  s koly ve dne i v noci 11–38 px, kabina Tatry 148 ve dne i v noci 10–14 px (hra pauznutá, jinak auto mezi snímky
  popojede a liší se celá zem).
- **Hotovo – krok 2: celý snímek z očí na kartě** (`ThreeDGpuRenderEyeFrame()`, v `viewport.cpp` `EyeCard`): snímek
  se dál sbírá jako dřív (průchody trojúhelníků a práce nad celým obrazem), ale kreslí ho karta celý najednou a čte se
  zpět jen jednou na konci. Každá práce snímku má svou podobu na kartě: obloha a zem pod obzorem, trojúhelníky (zem,
  vzdálená krajina, základy, mosty, tunely, plakáty, okna v noci, sklo světel), domy a postavy (sítě na kartě), co
  procesor u postavy nedokáže (víc než 32 kol), dá kartě své trojúhelníky, opar a mlha v dálce, stíny slunce (mapa
  stínů na kartě, jen když se domy změní), noc a záře světel, svit materiálů po noci, světla lamp a čarodějnic,
  výbuchy a kouř, hvězdy a měsíc, věci v ruce, tunel viděný portálem. Textury jen tohoto snímku jdou do jedné
  (atlas), sprity a textury domů si karta drží podle čísla. Barvy počítá stejně jako procesor (celá čísla, ořez), rig:
  karta proti procesoru 5–1023 px (okraje tenkých trojúhelníků a textura pod velmi ostrým úhlem).
  Bez karty, nebo když karta něco neudrží, kreslí procesor jako dřív. Rig: `test3d gpu snimek 0|1`.
- **Opraveno: sklo kabiny uřezávalo, co je za ním proti nebi** (komíny elektrárny, stromy). Procesor zapisoval hloubku
  skla přes oblohu, takže plakáty kreslené až po postavách byly „za sklem“. Teď sklo postav až po všem pevném a jeho
  hloubka se nezapisuje (ani na procesoru, ani na kartě): za sklem je vidět i měsíc a hvězdy, světla za ním svítí.
- **Hotovo – krok 3: snímek z očí se ukazuje rovnou z karty, bez čtení zpět.** Kontext karty sdílí věci s kontextem
  ovladače videa (OpenGL ovladač hry: Windows `win32-opengl` přes WGL, Linux `sdl-opengl` přes EGL; GLX nesdílí).
  Karta snímek nakreslí do jedné ze tří textur a hotovo; okno (chodec i kabina, `BlitEyePicture()`) do obrazovky
  napíše jen „díru“ (pixely s alfou 0) a ovladač videa při každém vykreslení obrazovky položí texturu karty do té
  díry (`OpenGLBackend::Paint()`). Co okno nakreslí přes obraz (řádek dole, tečka, hlášky, jiná okna), zůstane nahoře.
  Červené zranění se kreslí přes obraz jako barva s průhledností (i bez karty, při kopírování obrazu). Kde sdílení
  nejde (jiný ovladač videa, GLX, prohlížeč), čte se zpět jako v kroku 2. Rig: obraz rovnou z karty a čtený zpět jsou
  stejné na pixel (chodec u Sufingpoolu, kabina auta); liší se jen vyhlazené okraje písmen řádku nad obrazem (v díře
  se míchají s černou místo s obrazem, okraj písmen je o trochu tmavší).
  Snímek obrazovky (Ctrl+S) si obraz z karty dočte, díra v něm není. Pojistka: když hra spadne přímo při sdílení
  kontextu, příště a pak už pořád má karta kontext vlastní (deník to opakuje řádkem „sdileni … vypnuto“) a čte se zpět.
  Rig: `test3d gpu ukaz 0|1`, `test3d obrazovka <jmeno> [kresleni] [konec]` (PPM obrazovky, jak ji ukazuje ovladač).
- **Dál:** okno pohledu z očí se teď překresluje celé každý snímek jen kvůli díře – stačilo by kreslit řádek a tečku
  (méně posílání obrazovky na kartu).
- **Deník karty:** `3d_karta.log` je ve složce hry vedle `openttd.cfg` (ne v `save`). Hráčův: zink (Vortek, Mali-G615),
  OpenGL 4.3, obrázky 5–19 ms. Hráč zavírá hru zavřením celého Winlatoru, takže hra nestihne zapsat „konec“ a příště
  si myslela, že spadla karta, a vypnula ji. Teď kartu vypne jen pád před prvními 30 obrázky (jinak by se hra
  nemusela vůbec spustit); pozdější konec si jen schová jako `3d_karta_minule.log`.

## 21. 10. – marihuana: Mat, Matylda, sazenice; Slovenská strela; houby (domluveno)

- Hráč: u prodavače psů nebyla nápověda. **Hotovo** (`b4418a4`): vedle prodavače psů řádek dole píše
  „Prodavač psů: zmáčkni I“.
- Domluveno s hráčem (21. 10.): vše za peníze firmy (jiné peníze nejsou), **všechno, co se dá použít, léčí** (i houby
  +8 a cigarety +5). **Mat** (chodí po městě) prodává **jen mari, kus do inventáře**; u Mata píše „Mat prodává mari:
  zmáčkni I“, v inventáři „Mat prodává:“. **Matylda** stojí u hospody na trávníku se třemi kytkami v květináčích
  a prodává **sazenici marihuany v květináči** (8 £) – věc, která zůstane: v inventáři „zasadit před sebe“ ji postaví
  1,2 m před chodce. Pár vteřin stojí v květináči, pak je zasazená v zemi a roste (mladá → vysoká → košatá, až 2 m).
  Za půl roku E sklidí 3× mari, když se nesklidí do roka, je z ní **kupka marihuanového sena** a E dá seno
  (marihuanové seno, náklad FICR – pro draky, později dračí farma). Kytky se ukládají do savu (verze 380, `WPLT`).
  Nápověda u kytky: „roste, sklizeň za N dní“, „E: sklidit marihuanu“, „E: sklidit marihuanové seno“.
- Květináč a kupka sena jsou naše modely ze skriptu (`objekty/nase/kvetinac.py`, `seno.py`), kolegovy soubory beze
  změny; Matylda a kytky jsou k hospodě přidané v kódu (`AddMatyldaToPub()`). Z očí se teď kreslí i rostliny objektů
  (kytky u hulírny a sochy z očí chyběly).
- Kódy nákladu: věci, které jsou náklad, ho mají v kódu (mari MARI, seno FICR, pivo BEER, zboží GOOD, houby MUSH);
  v inventáři se nezobrazují (hráč: „asi není nutné“).
- **Slovenská strela** (vg_strela kolegy): oko kabiny bylo 2,35 m, přední sklo od 2,38 m – byla vidět jen palubní
  deska. Teď když je oko kabiny pod předním sklem, posune se doprostřed skla (strela 2,66 m); u ostatních vozidel beze
  změny (`CabEyeHeight()`). Kolegův index se nemění.
- **Houby: zrušeno (21. 10. večer).** Hráč: *„bude nový les, budou stát jinde holky a houby, nedělej houbový
  průmysl“*. Houbový průmysl nedělám, čekám na nový les. Platí dál: žádná nová 8bpp auta (Tatry ze hry přestavbou,
  vagony z grrrrf), doručování nákladu chodcem zatím ne.

## 21. 10. – #290: otáčení prostředním obráceně, kurzor a šipky s prodlevou

- Hráč: *„když držím prostřední a táhnu doleva doprava, tak prohoď doleva/doprava, ať se točí na druhou stranu“*.
  Hotovo: tah doprava točí plochu na druhou stranu než dřív (`window.cpp`, `RotateViewByDrag(-delta)`).
- Hráč (#290 = `3ee6dfd`): *„kurzor myši je na nějakým přetíženým jádru, a šipky taky trvá, než se dá do pohybu“*.
  V rigu běžná hra (město, auta, 150 snímků) karta skoro nic nedělá a snímek trvá 0,2 ms – prodlevu tu nevidím.
  Podezření: hlavní vlákno čeká na kartu (fronta karty je jedna: dávky vlákna obrázků, snímky z očí, obrázky
  hlavního vlákna za nimi), nebo telefon vytěžují vlákna. **Domluvit:** kdy to je (plocha / okno z očí / otáčení),
  a jestli to zmizí s vypnutou kartou (Nastavení › Grafika).
- Hráč odsouhlasil pořadí: 1) hlavní vlákno nikdy nečeká na kartu, 2) domy otočené plochy na kartu, 3) terén
  plochy na kartu, 4) plocha bez čtení zpět, 5) kosti.
- **Hotovo – bod 1, hlavní vlákno na kartu nečeká:** při kreslení plochy (`ViewportDoDraw()`) se obrázek, který ještě
  není, zastoupí nejbližším hotovým (jiný úhel, jiný snímek) a vlákno udělá přesný; jeho místo (budova, postava,
  auto hry, efekt) se překreslí, až přijde (`OweThreeDPictureBox()`). Hned se dělá jen úplně první obrázek objektu
  při daném přiblížení. Úlohy hlavního vlákna jdou na kartě přede všemi ostatními (`QueueGpuJob()`), dávky vlákna
  obrázků jsou nejvýš 16. Rig, otočka plochy při 4x: hlavní vlákno udělalo 9 obrázků (dřív 1144), vlákno 1099,
  snímek medián 94 ms (dřív 198); po zastavení a dokreslení je plocha na pixel stejná jako nakreslená rovnou.
  Rig: po otočení před `test3d snimek` dát `test3d dokresli` a `smycka`, jinak jsou na snímku zástupci.
- Hráč: *„2: GPU už jde plynuleji, trochu líp než bez karty; ve srovnání s #279 se kreslení procesorem zhoršilo… v #279
  se otáčím po pixelu a teď o centimetr s prodlevou“*. Změřeno v rigu (otáčení po 1°, celá obrazovka): bez karty
  62 ms na snímek při 1x, z toho domy 48 ms (rohy a řazení 31 ms, pixely 17 ms), při 4x 116 ms, z toho domy 86 ms.
  Při otáčení se obrázky domů nedrží (`TurnedViewStands()`) a procesor domy promítá a řadí každý snímek znovu, dům ve
  více kouscích obrazovky i vícekrát. Od #279 přibyly kolegovy open world domy s doplňky – mnohem víc trojúhelníků
  (12–17 tisíc na dům).
- **Hotovo – bod 2, domy otočené plochy na kartě** (`MakeTurnedObjectPicturesOnCard()`, `DrawBuildingsOnCard()`):
  domy kousku obrazovky, které ještě nemají obrázek, nakreslí karta najednou (vedle sebe do jednoho rámce, barvy
  a blízkost pixelů zpět jedním čtením) a hra si je drží jako dřív – i při otáčení; kousky obrazovky je pak jen
  kopírují. Stejně jako procesor: texel pod středem pixelu, díra doplněná z nejbližšího do 3 texelů, světlo rohů
  přes trojúhelník, odvrácené stěny pryč. Hloubku drží karta přesně, procesor řadil trojúhelníky podle středu
  s tolerancí: rozdíly v rigu (273 px při 1x, 2082 px při 4x) jsou tam, kde se procesor pletl (oknům chybělo sklo,
  šikmý proužek přes zeď). Při otáčení procesor na domy 0,4 ms místo 50 ms; rig nemá kartu (llvmpipe kreslí
  procesorem), takže rychlost karty ukáže až telefon.
- **Bod 3, terén otočené plochy – první půlka hotová: obrázek terénu se drží** (`TurnedGround`, `UpdateTurnedGround()`
  ve `viewport.cpp`). Dřív se každý snímek otáčení znovu kreslily sprity země všech dlaždic do obrázku ze severu
  a ten se pak pokládal na otočenou plochu. Teď se obrázek ze severu drží mezi snímky; dlaždice se do něj kreslí
  znovu, jen když se změní, co hra na ní kreslí (sprity země, kulatá kolej, označení kurzorem), a s ní sousedi
  v políčkách 64×64, kterých se dotýká; černá pod domy se zarovná a kolej po oblouku nakreslí jako v celém obrázku.
  Obrázek je položený s rezervou půl velikosti na každou stranu, při posunu dál se položí znovu a co má, si nechá.
  Země se teď kreslí jen u dlaždic, jejichž zem je v kousku obrazovky vidět (dřív i dlaždice daleko pod obrazovkou,
  sebrané kvůli vysokým domům). Rig: držený obrázek po otáčení, posunech, jízdě vlaku po kulatém oblouku a
  pohybu myši je na pixel stejný jako nakreslený znovu i jako dřívější kreslení (`test3d zem 0|1|znovu`, čísla
  v `test3d cas`). Otáčení po 1°, zem na snímek: při 1x sprity 2,9 → 0,6 ms, při 4x sprity 8,0 → 1,4 ms a stopy domů
  3,4 → 1,5 ms; pokládání na plochu je 6–9 ms a to je druhá půlka: položí ho karta.
- **Bod 3 hotový – zem otočené plochy pokládá karta** (`LayTurnedGround()`, `DrawGroundOnCard()`). Zem kousku obrazovky
  se sebere jako seznam trojúhelníků (obyčejná dlaždice z drženého obrázku, dlaždice se základy ze svého obrázku,
  který se teď taky drží, dokud se nezmění), karta je položí v pořadí od nejvzdálenějších a kousek se přečte zpět;
  pak se na něj kreslí všechno ostatní jako dřív. Karta drží obrázek terénu u sebe a dostává jen překreslené
  obdélníky; když se obrázek položí jinam, karta si ho posune sama (dřív by to bylo desítky MB znovu). Dlaždice se
  základy jsou na kartě v jednom atlasu. Rig (llvmpipe): z 632 tisíc pixelů snímku se karta od procesoru liší v 10–65
  pixelech na hranách trojúhelníků (desetinná čísla karty), na hotovém snímku 2–3 px; po startu jde na kartu
  0–1 MB za snímek otáčení. Bez karty pokládá zem procesor na všech jádrech (stejný obrázek na pixel jako dřív).
  Rig: `test3d zem karta 0|1`, `test3d zem kontrola 0|1`, v `test3d casy` počet kousků a čas karty.
  Jak rychlé je to na telefonu, ukáže až build: čtení zpět je to jediné, co tam můžou stát navíc (bod 4 ho odstraní).

## 20. 10. – #279 se otáčí plynuleji; co jsem po pádech #285 vypnul a nevrátil

- Hráč: *„co má #279 jinak, je plynulejší“*, *„otáčení plochou je plynulý“*, *„hlásil jsem 2 pády a tys všechno
  povypínal… vrátil jsi všechno zpátky?“*, *„dnešní karty mají 8 GB RAM“*.
- #279 (16. 10. večer) ještě neměl grafickou kartu: všechny obrázky 3D objektů na ploše kreslil procesor. Od #285
  je kreslí karta, **každý obrázek zvlášť**: poslat, nakreslit, počkat na kartu, přečíst zpět. Rig, otočení plochy
  o 360° po 5° u Sufingpoolu s auty: 1374 obrázků (auta, postavy, stromy, efekty), karta 1864 ms, procesor 478 ms.
  Na telefonu podle deníku karty 5–19 ms na obrázek – to je to cukání při otáčení. Domy na ploše kreslí dál procesor.
- Po pádech #285: karta zase výchozí zapnutá (`8f33958`), ale strop paměti karty zůstal 128 MB (dřív 384). Teď
  **1 GB**. V rigu při otáčení strop nehrál roli (karta drží pod 1 MB tvarů), cukání je z jednotlivých obrázků.
- Rig: `test3d casy` píše i kolik karta drží, kolik se na ni poslalo a kolikrát uvolňovala.
- Hráč: *„sprav gpu ať maká, gpu má na kreslení víc výkonu než procesor“*. **Hotovo – obrázky plochy po dávkách:**
  karta kreslí víc obrázků vedle sebe do jednoho rámce a čte ho zpět jednou (`ThreeDGpuRender()` bere seznam).
  Postavy a rostliny na domech se posbírají před kreslením snímku plochy a udělají najednou
  (`MakeThreeDPicturesAtOnce()`); vlákno obrázků vozidel se probudí jednou za snímek a vezme všechny čekající (až 64).
  Rig: dávky po 5–11 obrázcích místo po jednom, obraz plochy stejný na pixel jako z procesoru.
- Hráč: *„holky na balkonech se kreslí 60 000 trojúhelníčků“*, *„nejsou skoro vidět, při zoom in 2x je můžeš už
  vypnout“*. Holky mají 12–25 tisíc trojúhelníků (kouřící 12 tisíc na každý z 8 snímků); hra je podle velikosti
  zjednodušuje, ale holka 5 px vysoká měla pořád ~1 900. **Postavy domů (balkony, střechy) se kreslí jen při
  přiblížení 4x a víc**, při 2x a dál ne (`BuildingFiguresSeen()`). Při otáčení na 1x se tak nedělá žádný obrázek
  postavy (dřív 1374 na otočku).
- Hráč: kolega ukazoval auto zmenšené na 10 000 trojúhelníků – děravé plochy, černé. Zjednodušovat modely se
  nevyplácí; co není vidět, nekreslit.

## 20. 10. – pohled 0 s OpenGL: rozpadlé domy, marťanské domy jako stíny, noc jen na silnicích

- Hráč (#287, telefon, karta zapnutá): při pohledu 0 mají 3D domy černá místa a prosvítají přes ně původní domy,
  marťanské domy jsou jen stíny („když má za sebou silnici, ten kousek se vykreslí, když prázdnou dlaždici, jen
  stín“). V noci ztmavnou jen ulice. Při otáčení je všechno v pořádku.
- **Příčina (chyba hry):** OpenGL ovladač kreslí s blitterem 40bpp-anim. Ten si u každého pixelu z palety (tráva
  a původní domy ze sady 8bpp) pamatuje číslo barvy a karta pak barvu dosadí z palety sama. Naše 3D domy, postavy
  a pohled z očí se při pohledu 0 píšou rovnou do 32bpp obrazu a to číslo nemazaly, takže karta ukázala trávu nebo
  starý dům s jasem našeho pixelu (tmavé části = „stín“). Silnice jsou 32bpp, čísla nemají, proto přes ně dům byl
  vidět. Noc ztmavila jen 32bpp obraz a pixely z palety mají jas nulový = plný, takže tráva v noci svítila. Otočený
  pohled si blok předem vyčistí, proto chybu neměl. V rigu nebyla vidět, protože rig kreslí bez OpenGL.
- **Oprava:** `Blitter::RealizeAnimation()` (32bpp-anim a 40bpp-anim) udělá z pixelů obdélníku obyčejné barvy. Hra
  ji zavolá před kreslením 3D objektu při pohledu 0 (obdélník jeho krabice s přesahem a postavami na střeše), před
  obrázkem postavy či vozidla, pohledem z očí a náhledem v nákupu. 40bpp-anim má vlastní `DarkenRect()`: pixel
  z palety se ztmaví jako ostatní, animovaná voda dál teče, jen tmavší.
- Rig nově umí OpenGL: `scena_gl.sh` (Xvfb, `sdl-opengl`, 40bpp-anim; `VD=sdl BLIT=32bpp-anim` pro animaci palety).
  Ověřeno: před opravou stejná chyba jako u hráče, po opravě domy celé, noc tmavá i na trávě, otočený pohled
  beze změny. Pohled z očí se v rigu přes OpenGL otevřít nedá (vanilla kontrola `_screen.dst_ptr` v ladicím buildu).

## 20. 10. – Springfield nebude, limity trojúhelníků

- **Springfield nebude** (hráč: „moe a lenny … nech je v grrrrf a auto pro ježdění ve springfieldu taky nech
  v grrrrf, nepoužijem. já nevím proč dělat jiné domy springfieldu, když tady už domy jsou udělaný“). Moe, Lenny
  a auto Simpsonových zůstávají jen v grrrrf, do sady hry je nebereme. Tematické město `springfield` se nedělá.
  Kolegovi `hra/3d/ODPOVED-SPRINGFIELD.md` (grrrrf 7efe189).
- **Trojúhelníky aut:** limit domluvený není. 17. 10. hráč hranice zrušil („nech si dělat 3D GRF tak, aby to
  odsejpalo“, `hra/3d/HRANICE-PODROBNOSTI.md`): nejdřív se předělá hra, pak změříme, kolik unese, a teprve pak
  se domluví, kde ubrat. Kolegova auta v11 (Tatra osobní) mají 32–83 tisíc (Tesla 82 684, Škoda 1000 MB 77 716,
  GAZ-24 71 281, Velorex 65 423, pickup 64 585), sám je zjednodušil z 2,6 milionu / 934 / 684 tisíc. Pro srovnání
  Tatra 603 má 7 659, Tatra 613 24 292, Trabant 7 782, holka 11 778.

## 19. 10. – #287: rozhlížení myší, jeden řádek dole, barvy psů, větší houby, auta do kopce

- **Rozhlížení pravým tlačítkem** v pohledu z první osoby jde stejně jako posun mapy (hráč: „ať to je v souladu
  s pohybem pravým čudlíkem mapou“). Hráč má náš režim (`scroll_mode = 0`, RMBPinned), kde tah doprava posune pohled
  mapy doprava. V první osobě se dřív tahem doprava otočil doleva a tahem dolů díval nahoru. Teď: doprava se dívá
  doprava, dolů dolů.
- **Spodní panel pohledu z první osoby je pryč** (hráč: „ten návod dej taky pryč, zdraví, u města dej do okna
  inventáře“). Obraz je o něj větší. Návod kláves zmizel, i ve vozidle. Zdraví už inventář měl, pod něj přibyl řádek
  „U města …, dlaždice …, směr …°“.
- **Dole v obraze jeden řádek** (hráč: „udělej to na jeden řádek: zdraví, kde je, a že je u obchodu ať zmáčkne I;
  dlouhý řeči jak se to ovládá tam nepatří“): „Zdraví 100/100 · u města X“ a žlutě, co jde na místě: „Obchod:
  zmáčkni I“, „E: utrhnout houbu“, „E: nastoupit do …“. Ve vozidle místo toho jméno a rychlost. Žádný návod kláves.
- **Auta se naklánějí do kopce** (hráč: „auta se nenaklání do kopce“): vozidlo z 3D objektu má předek tak vysoko
  nad zadkem, jak stojí zem pod jeho předkem a zadkem (`VehicleSlopePitch()`). Naklání se tedy, až předek najede na
  svah, ne skokem na dlaždici. Na mostě, v tunelu, loď a letadlo rovně. Mapa bere náklon po 2° (málo obrázků), pohled
  z očí přesně, vlastní kabina oka zůstává rovně. Chyba po cestě: náklon se počítal před zkrácením auta na délku,
  jakou mu hra dává, takže zkrácené auto stálo strměji než svah. Teď se naklání až zkrácené. Ověřeno v rigu
  (`test3d silnice 603 3 kopec`, `test3d svah`): Tatra 603 do kopce +8°, z kopce −8°, na mapě i z očí.
- **Prodavačka psů farmářka** je ze hry pryč (hráč: „tuhle prodavačku psů přestanem používat“): záznam z `psi/objekty.json`,
  model ze sady hry a autor z README sady. U kolegy v grrrrf zůstává.
- **Psi bez barvy** (hráč: „fenek nemá barvu a nějakej vlk neměl barvu“): devět psů od kolegy má barvu zapsanou
  postaru (KHR_materials_pbrSpecularGlossiness, `diffuseTexture`/`diffuseFactor`), hra četla jen `baseColorTexture`.
  Jde o fenka, vlka lesního, nafoceného psa, baseta, bulmastifa, bulteriéra, jack russella a malamuta. Hra teď bere
  barvu z obou zápisů (`GlbColourTexture()`, `GlbColourFactor()`), i s posunem textury. Ověřeno: fenek, vlk lesní
  i nafocený pes mají barvy.
- **Houby 5× větší** (hráč: „houby jsou moc malý, udělej je mnohem větší“): `MUSHROOM_SCALE` 5, tedy 25–50 cm místo
  kolegových 5–10 cm. Ve dne je teď vidět i z dálky.

## 19. 10. – #286: houby nevidět, obří houby nesvítí, psi nejsou

- Hráč (#286, telefon): „malý houby nejsou vůbec a velký zespod taky nesvítí“, „nejsou psi, opice, v tom buildu něco
  chybí“. **Build je celý:** zip #286 jsem stáhl a porovnal se zdrojem. Má všech 589 souborů sad, psi, zvirata,
  strasidla i houby. Indexy mají jen konce řádků CRLF a hra je čte stejně.
- **Malé houby (chyba hry):** v pohledu z očí se figura dál od oka kreslí obrázkem z mapy. Ten má u 5–10 cm houby
  1–2 pixely, takže houba už od 1,5 m zmizí. Nově se malá věc kreslí po trojúhelnících (zjednodušených podle velikosti
  na obrazovce), když by obrázek byl víc než 3× hrubší než místo na obrazovce, nebo měl méně než 4 pixely. Ověřeno:
  houba je vidět z 2,6 i 5,6 m, ve dne i v noci.
- **Obří houby nesvítí (chyba hry):** sdílená paměť tvarů měla klíč jen podle souboru. Noční podoba stromu
  `<jmeno>/noc` je ze stejného .glb, takže dostala denní tvar, přečtený jako první, bez svítícího spodku. Klíč teď
  rozlišuje noc (`night_glass`), světla snímků i vynechaný uzel. Ověřeno: choroše svítí.
- **Svit po setmění:** dřív se svítící díly kreslily se světlem dne i vlastním svitem, takže barva přepálila do bílé.
  Teď se kreslí jen vlastním svitem v barvě materiálu (choroš #9FE8FF světle modrý, ne bílý).
- **Dům háj:** jeho obří houby nesvítí, protože model nemá svítící materiál. Uzel `noc` jsou jen lucerny ve výšce
  kolem 2 m. Kolegův `domy/houby_noc.py` udělal svit jen čtyřem houbám-stromům. Hráč chce: „ať dům háj svítí i nahoru
  všude“.
- **Zem pod houbami:** kolega dal pod klobouky místa `svetlo_NN` („záře tu osvětluje zem“). Hra je u stromů-hub
  nečte. Na mapě shora tak svit vidět není, spodek klobouku je pod kloboukem.
- **Psi:** v hráčově savu v rigu žijí (pes v ulici, vlci u dveří, prodavač). Pes poslaný 3 m a 12 m před chodce je
  vidět. Na 40 m se pes neukáže, protože jeho obrázek z mapy se nenakreslí. Psi se rodí 4–7 dlaždic od chodce
  (60–100 m), tam má pes na obrazovce pár pixelů.

## 19. 10. – mírné domy jako krabice v nové hře (ITL Houses); deník 3d_domy.log

- Hráč (build #286, telefon): v nové hře jsou mírné domy „původní naše krabice“, hliněná města a Tatry jsou 3D.
  Nastavení 3D domů má v cfg správně (`three_d_houses = 0`).
- **Příčina:** seznam GRF pro novou hru (`[newgrf]` v hráčově cfg) má ITL Houses 2.3. ITL vypne všech 95 původních
  domů a staví svoje. Ty nemají 3D objekty (žádný balíček, jaký mají Tatry), tak se kreslí krabicemi svých spritů.
  Hliněná města staví naše domy, proto jsou 3D. Ověřeno v rigu i na #285: nová hra s ITL má krabice. Hráčův save
  ITL nemá a v rigu je s hráčovým cfg 3D (procesor i karta).
- **Caribbean (TT20) a ITL:** Caribbean bez ITL ≥ 1.4.0 dává fatální chybu (Action 0B Fatal) a hra ho vypne. Hra
  se spustí, ale karibské průmysly v ní nejsou. Chyba, se kterou Caribbean běží, je jiná: mimo subtropické klima
  napíše „intended for the sub-tropical climate“, což je jen upozornění. ITL domy přijímají a vyrábějí náklad pro
  karibské průmysly (překladová tabulka ITL: FOOD, BEER, WATR, FUEL, VEHI, BDMT, RCYC, WSTE …). Proto ho Caribbean
  chce. Caribbean je GPL 3, kolega ho má rozbalený v grrrrf (`rozbalene/caribbean-2.7`, yagl).
- **Rozhodnuto (hráč: „caribien jako firs, domy budovy průmyslu krabicí a bez itl houses, podmínky pryč, bez
  omezení“):** Caribbean jako FIRS, bez ITL. Domy jsou naše 3D, budovy průmyslu krabice (jako u každé GRF sady
  průmyslu). Upravený Caribbean nemá žádnou podmínku. Všech 295 záznamů Action 0B (59 podmínek po 5 jazycích:
  ITL, subtropické klima, inflace, verze hry, nesnášenlivost s ECS, FIRS, NewCargos…) je nahrazeno neškodným
  Action 0D do nepoužitého parametru 0x60. Počet záznamů zůstal, takže všechny skoky vedou tam, kam vedly.
  Jméno „Industries of the Caribbean 2.7 (Decouple, bez podminek)“ a v popisu poznámka o úpravě (GPL 3).
  GRF ID TT20 zůstává, aby šly savy s Caribbean. Skript je ve scratchpadu `skripty/carib_bez_podminek.py`.
  K překladu yaglem chybí původní GRF (obrázky). GitHub autora (2TallTyler) i BaNaNaS jsou ze sítě zavřené,
  takže soubor má poslat hráč.
- Města bez ITL nebudou brát ani dávat palivo, auta, stavební materiál a odpad, takže tyhle řetězce nepojedou.
  Hráč to ví z nabídky (bod 1).
- **Deník `3d_domy.log`** v hráčově složce OpenTTD (`three_d_objects.cpp`, `ReadDiary()`) zapisuje:
  - každý index a počet jeho záznamů;
  - každý soubor, který se nepřečetl, a proč (budova se pak kreslí krabicí);
  - každou GRF sadu, jejíž domy nemají 3D model, s počtem zapnutých původních domů hry (`NoteThreeDHouseBoxed()`).
  První takový problém hra jednou ukáže v okně (`STR_ERROR_THREE_D_BOXES`).

## 19. 10. – psi, zvířata, strašidla a houby od kolegy (par13)

- Kolega dodal (`PSI-ZVIRATA-HOUBY-PRO-HRU.md`): sadu `psi` (31: 8 na prodej, 19 do ulic, vlk u dveří,
  3 prodavači), `zvirata` (15: mírné klima kolem měst, žirafa/slon/lev/opice pro pouštní města, mamut), `strasidla`
  (houbový bojovník, loutka na tříkolce), `houby` (7 ke sbírání, každá i svítící na noc), přízrak k čarodějnicím,
  noční svícení obřích hub hliněného města, místa `strechy` na pouštních domech, Chimata v `pozy`. Všechno v sadě hry;
  autoři v README sady. Pět modelů je CC BY-NC (jen nekomerčně: kočka, vlk, lev, loutka, přízrak), sedm CC BY-SA.
- **Hra čte** `druh` pes / zvire / strasidlo a postavu s `prodavac` (`ThreeDAnimal`): pózy z kostry (`pozy`) nebo
  snímky (`animace` po pózách), `povaha`, `kde`, `klima`, `tema`, `kdy`, `prodej`, `cena`, `leta`, `nazev`. Houby
  (`kategorie` houba) jako seznam s noční svítící dvojicí; strom s `noc` má v noci svítící figuru `<jmeno>/noc`.
- **Zvířata kolem chodce** (`three_d_animals.cpp`, jako Mat a draci: jen obrazovka, kolem chodce, jinak kolem středu
  otočené mapy): v ulicích pár psů a koček (v pouštních městech opice), u dveří leží vlk (vrčí, když přijdeš), na
  střechách pouštních domů opice, kolem měst zvířata podle klimatu (mírné: los, jelen, laň…; poušť: žirafa, slon,
  lev), v noci strašidla (loutka v ulicích, houbový bojovník kolem hliněných měst). Občas se objeví prodavač psů
  s nabídkou 3–5 náhodných zvířat z těch na prodej, chvíli stojí a zase odejde.
- **Povaha** z indexu: `utoci` jde po chodci a útočí, `vyprovokovat` je klidné, dokud ho chodec nezasáhne, pak útočí,
  `mirumilovne` po zásahu utíká. Zasáhnout jde kladivem, meči, šípy, šipkami, kulkami i výbuchem rakety. Zdraví
  a síla úderu podle velikosti (kočka 2 rány, slon 12). Kdo má pózu `smrt`, padne a chvíli leží.
- **Pes chodce**: u prodavače ukáže inventář „Prodává psy (…)“ s jeho 3–5 zvířaty a cenami z indexu (`cena`,
  s inflací); klik koupí (příkaz `BuyWalkerPet`, peníze firmy). Pes je firmy a ukládá se se hrou
  (`CompanyProperties::walker_pet`, verze savu `WalkerPet`); s otevřeným pohledem z první osoby jde za chodcem
  (chůze, při velké vzdálenosti běh, u chodce stojí), Falkor létá. V inventáři „Tvůj pes: … [pustit]“.
- **Houby**: rostou kolem hliněných měst (na nezastavěné zemi, každá třetí dlaždice, druh a místo podle dlaždice,
  nic se neukládá), vidět jsou z očí do tří dlaždic, v noci svítící verze. **E** u houby (do 1,5 m) ji utrhne do
  inventáře (věc „Houby“), dole se ukáže „E: utrhnout houbu (…)“; utržená doroste asi za měsíc. K čemu houby
  jsou, zatím nevím – nosí se.
- Obří houby hliněného města mají v noci svítící spodky (figura `/noc`).
- Tatra osobní v9 s Trabantem (hráčův balíček `Tatra_osobni_3D-v9`, MAXt) ověřena v rigu bez úprav hry: Trabant
  z dílů v `doplnky` (hlavní glb jen délka), kola v dílech, Tatra 603 a 613 – na mapě i z očí.
- Rig: `test3d zvirata` (druhy, houby, kdo je kolem), `zvirata posli <jmeno> [metru]`, `zvirata uder`,
  `zvirata pes <jmeno>`, `zvirata koupit [jmeno]`, `zvirata houba`, `zvirata utrhni`.

## 18. 10. večer – kolegovy tři balíky; psi a zvířata domluvena; svítící a průhledné materiály

- Kolega poslal: Tatru v4 a stadion (sud s pípou, Mat posunutý k sudu, `mat_ridi` pro hasičskou posádku s `meritko`
  0,74, červená kabina jako sady bez GRF), noční věci (strašidlo průhledné a svítící, zbraně kulomet / raketomet /
  raketa, pochodeň a lucerna do ruky, lampy ve stylu Morrowindu, Tatra osobní v9 s Trabantem, Chimata) a prosbu
  o formát psů, zvířat, hub a strašidel z hráčova `test1.zip`.
- Hráč na psy a zvířata: prodavači psů se objevují náhodně, stojí nebo chodí; všichni prodávají všechno, ale každý má
  v nabídce jen 3–5 zvířat náhodně. Strašidla útočí, ducha ale musí hráč vyprovokovat; slon a lev jsou mírumilovní,
  dokud hráč nezačne souboj. Pořadí je jedno – udělat, co má kolega hotové.
- Odpověď kolegovi (`hra/3d/ODPOVED-PSI-ZVIRATA-HOUBY.md`): `druh` pes / zvire / postava s `prodavac` / predmet
  s `kategorie` houba / strasidlo; zvířata s kostrou ve formátu `pozy` (jako postavy), bez kostry jedna póza nebo snímky
  jako uzly; Chimata taky v `pozy`; nový klíč `povaha` (utoci / vyprovokovat / mirumilovne) rozhoduje záznam, ne hra;
  opice na střechách přes uzly `strecha_NN` od kolegy; `noc` a `svetlo_NN` i u stromů a předmětů; `kde` + `tema`.
- Hotová data jsou v sadě hry (`92387b8`): stadion, `mat_ridi`, Chimata, zbraně, pochodeň, lucerna, sada `lampy`.
  Tatra v4 je hráčův balíček (newgrf), hra ho jen čte; rig ho má místo v3.
- **Svítící a průhledné materiály** (glTF `emissiveFactor`, `alphaMode` `BLEND`): hra je čte u všech modelů
  (`tex_glow`, `tex_alpha` tvaru). Svítící část se kreslí s vlastním světlem navíc (bez stínu) a v noci se neztmaví
  – na mapě i na kartě přes značku „rozsvícený pixel“, z očí se po ztmavení noci nakreslí znovu. Průhledné se kreslí
  jako sklo se svou alfou. Maják hasičské Tatry v4 a strašidlo tak svítí samy.
- Opraveno cestou: z očí se textura modelu neopakovala za okraj (strašidlo ji má 16× zvětšenou – kreslily se jen
  tečky); zjednodušený tvar na dálku dělal díry do modelů, kde má každý trojúhelník vlastní rohy (rohy na stejném
  místě a stejném místě textury se teď spojí); průhledné nad oblohou si drží hloubku.
- **Lampy**: doplňky domů mají noční sklo (`noc`) a záři (`svetlo_NN`) – v noci svítí v textuře doplňku a v jeho
  barvě na tom místě (papír modrý ve dne, modrý i rozsvícený). U silnic stojí kolegova `lampa_na_sloupu` tam, kde má
  hra sama pouliční lampy (vanilla `Roadside::StreetLights` na městských ulicích, 4 místa dlaždice): sprite lampy hra
  vynechá a postaví 3D lampu s ramenem nad ulici (otočený pohled, sever s 3D domy, pohled z očí). Rozhoduje hra, ne
  já. Lampy u dveří dá kolega do domů sám (`doplnky`). Rig: `test3d lampy` vypíše dlaždice s pouličními lampami.
- **Pochodeň a lucerna** (kolegovy `pochoden`, `lucerna`, `kategorie` svetlo): nové věci inventáře (od začátku, i na
  prodej v obchodě, iddqd je dá), klávesa **L** střídá pochodeň → lucernu → zhasnout, klik v inventáři taky. Drží se
  v levé ruce dole vlevo (zbraň zůstává v pravé), plamen se střídá po snímcích `plamen_NN`, v noci světlo kolem chodce
  (teplé, 8 m) a samotné světlo se neztmaví; pochodeň při zhasnutí zasyčí (`zhasnuti`). Rig: `test3d chodec svetlo`.
- **Kulomet a raketomet** (kolegovy `kulomet`, `raketomet`, `raketa`, `naboj`): klávesy 6 a 7, náboje (300) a rakety
  (10) v inventáři od začátku, iddqd doplní. Kulomet střílí dávky (klávesou útoku 5 ran, levým tlačítkem myši, dokud ho hráč drží), kulky
  letí rovně z hlavně. Raketa letí rovně 40 m/s (bez gravitace, jak navrhl kolega), za sebou plamen (svítí i v noci
  a svítí kolem sebe) a kouř; kde narazí (zem, zeď, drak, čarodějnice), vybuchne: koule ohně 4 m, světlo kolem asi
  10 m, po ní tmavý mrak. Výbuch zasáhne draky v dosahu a srazí čarodějnice i strašidlo – padají k zemi a u země
  zmizí. Nabíjení 2 s, mezitím se kreslí stav `prazdny` (raketomet bez uzlu `raketa_v_hlavni`). Raketomet se drží níž
  a víc vpravo než kuše, aby nezakryl půl obrazu.
- Opraveno cestou: střely (šípy, šipky, kulky, rakety) se zapichovaly do vzduchu nad nízkými domy – zeď měla pevně
  12 m. Teď platí skutečná výška 3D domu nebo průmyslu (`top`), jinde dál 12 m.
- Planý poplach: rig hlásil pád karty u stadionu („kresleni obrazku 7 (glDrawElements)“), ale byl to můj zkušební
  deník z ověřování hlídače pádů. Deset běhů stadionu s Tatrou v4 na kartě (llvmpipe) proběhlo bez chyby.

## 18. 10. – build #285 shazuje celý Winlator; deník grafické karty

- Hráč: *„#285 padá celej winlator. jednou v nákupním menu, podruhý load save velké zátěže“*, *„spadne to vždycky,
  když kliknu v menu nákupu na naše 3D auto, teď jednoduchá tatra vb“*, *„to není řešení takhle něco vypnout“*.
- #285 = `87a22c8`. Proti #284 přibylo kreslení 3D obrázků grafickou kartou (vlastní vlákno, OpenGL 3.3). Pod
  Winlatorem běží OpenGL (VirGL/Zink) přímo v aplikaci: když spadne ovladač, zmizí celý Winlator a crash log hry
  nezůstane. V rigu (software OpenGL) to nepadá. Paměť v rigu: domy ~450 MB jako v #284 (stará sada ještě víc).
- Hotovo (`8f33958`): **deník karty** `Dokumenty/OpenTTD/3d_karta.log` – každý krok s kartou se zapíše na disk
  dřív, než se udělá (kontext, funkce, překlad programů, rámec, každý tvar/trojúhelníky/textura na kartu, kreslení,
  čtení zpět). Po pádu při dalším startu hra kartu vynechá, nastavení vypne, řekne v okně, u čeho umřela, a deník
  nechá jako `3d_karta_minule.log` → **ten pošli**, podle posledního řádku opravím přesné místo. Karta je výchozí
  zapnutá (nastavení Grafika „3D obrázky kreslí grafická karta“), drží nejvýš 128 MB. Obrázek, jehož trojúhelníky
  ukazují mimo rohy tvaru, kreslí procesor (telefonní karta čtení mimo neodpustí).
- Opravená chyba: **textury postav** (holky, Mat, posádka, čarodějnice) se brala v plné velikosti 1024×1024 i na
  obrázek pár pixelů – velikost textury se hledala v tvaru, který postava se snímky nemá. Textury vozidel v rigu
  26,8 → 10,9 MB.
- Hráč: *„funguje to dobře, dal jsem nový winlator. ten starý neměl opengl hotové. nech implicitně zapnuto gpu“* –
  příčina pádů byl starý Winlator bez hotového OpenGL; karta zůstává výchozí zapnutá, deník a hlídač zůstávají.

## 18. 10. – pohled z první osoby

- Hráč: *„může chodit rychleji ještě“*, *„když dvakrát kliknu na pravej čudl myši tak se mi zas pohled vycentruje,
  jako otáčení mapy“*. Hotovo (`d240195`): chůze 3,0 m/s (dřív 1,6), běh 8,5 m/s; dva kliky pravým bez tažení
  srovnají pohled rovně dopředu. *„pustil jsem 3D domy, je to dobrý, nečuká se to“*.

## 18. 10. – noční létající čarodějnice (prosba kolegovi, hra domluvit)

- Hráč: *„potřebujem v grrrrf noční létající čarodějnice“*, *„udělej čarodějnice velký jak holky na zastávce“*.
  Kolegovi `hra/3d/PROSBA-CARODEJNICE.md`: 2–3 čarodějnice na koštěti, model ve skutečných metrech jako holky,
  v indexu `meritko` 2 (holky na zastávce kreslí hra dvakrát), let jako animace `let_NN` jako létající draci,
  co svítí pod uzlem `noc`.
- Hráč: *„kolem lucerny chci světlo okolo a z ruční svítilny paprsek“*. Kolegovi doplněno: skla pod `noc`, místa
  `lucerna_NN` (záře a světlo na okolí) a `svitilna_NN` + `svitilna_NN_kam` (kužel a skvrna, kam dopadne), barva
  a dosah v indexu `svetla` – jako světla vozidel.
- Hráč: *„jo ať litaj čarodějnice“*. Hotovo ve hře (`68b20d6`): v noci 1–3 najednou (dvě od 4 měst, tři od 10),
  přilétají postupně, od města k městu, měkce zatáčí, 22 m nad zemí a 8 m nad střechou, rychlost z `krok_m`
  animace `let`. Na mapě `meritko`× (2 jako holky na zastávce), z očí skutečná velikost. Ve dne žádné.
  Lucerna: plamen a světlo kolem (na ni a na okolí), svítilna: paprsek jako rozšiřující se klín a skvrna, kde
  dopadne na zem; z očí totéž jako záře a světlo po pixelech. Překresluje se jen krabice jí a jejích světel.
  Zkoušeno na zkušební krabičce v rigu (jen scratch, do repa nejde), modely od kolegy přijdou místo ní.
  Rig: `test3d carodejnice [x y stupně [jméno]]`, `test3d carodejnice tady <stupně>`.

## 18. 10. – stadion: prázdná hasičská Tatra, Mat u grilu; barvy doplňků číslem

- Hráč: *„mat nemůže sedět v hasičské tatře, bude točit pivo u grilu“*, *„na hřišti stojí prázdná tatra, bez
  řidiče“*, *„jinak může řídit mat“*. Hra čte `misto_ridicky` (řidička záznamu se nekreslí, Mat na místě řidiče
  se schová z oka kabiny) a stojící vozidlo u akce domu (`akce.tatra`, sady jako seznam: kabina, mřížka,
  nástavba), vždy prázdné (`16cbe1c`). Díly vozidla až 125 (Tatra 148 v3 jich má 65).
- Od kolegy (grrrrf `4527bc2`): slunečníky a kola jeden doplněk na tvar, barva číslem pro každé místo
  (hráč: *„slunečničky bycykly barvu čislem“*); Mat točí u grilu. Holky dál velikost 1.
- Hráč: *„co ta barva kabiny, to dal blbě v grrrrf“* – stojící Tatra nemá GRF, kabina a mřížka mají v indexu
  oranžovou `CA5E16`; na silnici je přebarví GRF (1C1) na hasičskou červenou. Kolegovi: sada s hasičskou
  kabinou do balíčku Tater a do seznamu sad stadionu.
- Hráč: *„mat točí pivo, má sud?“* – nemá, u grilu není pípa ani sud (póza má pípu 0,3 m před sebou ve
  výšce 0,95 m). Kolegovi: sud s pípou k Matovi. Chybí i póza Mata `ridi` (Tatra na silnici zatím bez řidiče).
- Zpráva kolegovi: grrrrf `hra/3d/STADION-SUD-A-KABINA-PRO-KOLEGU.md`.

## 18. 10. – kolegovy open world domy ve hře; GPU 1–6 odsouhlaseno

- Kolega (grrrrf `44fa5ac`, `hra/3d/DOMY-OPEN-WORLD-PRO-HRU.md`): všech 39 domů přepečených s uličkou 1,5 m u každého
  okraje, dveře dosažitelné z ulice, branky otevřené; slunečníky, stolky, lavičky, kola… vyňaté do nové sady
  `objekty/doplnky` (111 kousků); `okraje_textur` 16; gltfpack; stadiony `interier: false` + průchody; akce
  nového stadionu rozmístěná (rožeň nad grilem, Tatra na hřišti, Mat u stánku PIVO, holky u stolků).
- `e48bfb9`: převzato do sady hry. **Holky meritko 1** (kolega měl všude 2 – velikost holek je hráčova). Kolegovi
  `hra/3d/DOMY-OPEN-WORLD-PREVZATO.md`. Rig: město a stadion s doplňky v pořádku.
- `8addc3d`: `okraje_textur` – soubory se značkou se nepřepočítávají ani nevyplňují (obraz stejný na pixel).
- Hráč odsouhlasil GPU body 1–6 (lak na kartě, domy, pohled z očí a kabina, kosti, terén otočeného pohledu, obraz
  bez stahování zpět). Pořadí 3 → 1 → 2 → 4 → 5 → 6. Hráč pošle výstup `test3d gpu` z telefonu.

## 18. 10. – auta po nákupu připravená, ne až při výjezdu z depa

- Hráč: „auta načítat po nákupu, ne až když vyjede z depa… jezdí chvíli neviditelný… náhled zachovat a používat“.
- Příčina: tvar s díly stavu na mapě, textury v barvě firmy a obrázek se dělaly až při prvním kreslení auta na
  mapě (vlákno na pozadí), do té doby se nekreslilo nic.
- `14dfe5b`: po koupi (zpětné volání nákupu, i vagonu) se hned objedná na vlákně tvar a obrázek auta čelem z depa
  ve zvětšení každého otevřeného pohledu (i okna auta) – při 0° sprite, v otočeném pohledu obrázek. Co načetl
  náhled nákupu, zůstává a kreslí se z toho (tvar drží 2 minuty od posledního kreslení). Rig `auta` dělá totéž.
- Ověřit v buildu: koupit auto, hned pustit z depa – má být vidět hned.

## 18. 10. – kvantované glb (gltfpack) hra čte

- `6c899b0`: čísla všech typů (bajty, shorty, se znaménkem i bez, `normalized`), posun textur
  `KHR_texture_transform`, normály inverzní transpozicí, dům čte první síť mimo `noc` se všemi rodiči (gltfpack
  dává síť do vlastního uzlu pod pojmenovaný). Soubor, který chce `-c`/`-cc` (meshopt) nebo `-tc` (KTX2), hra
  odmítne s hláškou v logu.
- Zkoušeno v rigu s gltfpack 1.3.0 `-kn -km -ke`: Tatra 603 (1,44 → 1,08 MB), 613 (3,33 → 2,02 MB), drak s kostrou,
  všechny mírné domy (65 → 48 MB) – kreslí se stejně (pár pixelů světla jinak). Většinu souboru tvoří obrázky.

## 18. 10. – odpověď kolegovi na `PROSBA-TATRY-DOMY-POSTAVY.md` (hráč odsouhlasil)

- Kolegovi `hra/3d/ODPOVED-TATRY-DOMY-POSTAVY.md`. Hasičská nástavba 138/148 jako podtyp vody (dá se koupit), na
  stadion `"tatra": {"zaznam": "nt_t148", "sada": "nastavba hasici", …}`, **řídí ji Mat** (`poza` `ridi`).
  **Žádný hasič** (ani v `akce`). Zvláštní vozy do `Zvlastni_vozy_3D` (MAX8) – savy nevadí. **Světla nezapomenout.**
  Liaz MTS s kabinou 706. Okraje 16 px (hráč: „proč si hra neudělá sama?“ – dělá, značka jen ušetří čas při čtení).
  Masky nejbližší texel (hra je nemíchá). Maska na díl podle materiálu `<materiál>_maska.png` / `_lak.png`.
  `interier_<díl>`, instance, LOD a) `<díl>_lod1/_lod2`. Jména póz beru; **zachovat pózu College Girl za volantem
  Tatry** (`ridi`); řidiči později v konfigurátoru. Doplňky už hra umí. Open world: ulička 3 m (1,5 m u okraje),
  stadion `interier: false` + mezery + `pruchody`.
- **Hra dodělá (hráč 18. 10.: „běž dělat kvantování a co je potřeba“):** ~~čtení `KHR_mesh_quantization`~~ hotovo,
  ~~značka `okraje_textur`~~ hotovo, masky podle materiálu, `interier_<díl>` jen z kabiny, LOD uzly,
  stojící Tatra v `akce` (záznam + sada).

## 18. 10. – myš podruhé: ukazatel se zase vrací, ale pod ClipCursor (build #284: naráží na kraj okna)

- Hráč (#284): pod kurzorem hry se pohybuje kurzor Windows, naráží na kraj okna a mapa stojí. Winlator má starší
  (bez opravy z `cota`) a ta oprava z Winlatoru půjde pryč: **spravit to ve hře, aby myš fungovala všem**. Myš se
  „kouše deset let, i na PC“.
- Volný ukazatel (`f29a87c`) byl špatně: ve Winlatoru kurzor Windows vidět a u kraje okna tah končí.
- **Nově (zdroje Wine 10.10, `winex11/mouse.c`, `event.c`, `win32u/input.c`):** `SetCursorPos` bez clipu dělá
  krátký grab na kořenové okno (pod `explorer /desktop` okno plochy, proces hry k němu nemá data → puštění zahozeno).
  **Když hra drží `ClipCursor`, Wine drží grab svého clip okna a `SetCursorPos` jen posune ukazatel bez grabu;**
  události clip okna (i puštění) Wine pošle hře (`send_mouse_input()`, hwnd 0 + `clip_window`). Takže: náš režim
  a otáčení prostředním vrací ukazatel jako dřív (kurzor Windows se nikdy neukáže zvlášť, kraj okna nevadí), ale po
  dobu držení tlačítka je ukazatel uzavřený do okna (o 1 px menší než okno – clip velikosti obrazovky Wine bere jako
  žádný). Pustí se při puštění tlačítka, konci připíchnutí a ztrátě fokusu. Vanilla režimy beze změny.
- Zbytkové riziko: první vrácení ukazatele po stisku může přijít o zprávu dřív, než Wine clip zapne (Wine ho zapne
  až při čtení dalších zpráv). Přeloženo MinGW, neověřeno na Windows ani ve Winlatoru – zkouší hráč.

## 18. 10. – kolega a optimalizované domy (doplňky)

- Hráč: „kolega se může pustit do budov optimalizovaných, že jo, co chybí?“ Chybí: kolegovo potvrzení formátu
  doplňků (`hra/3d/DOPLNKY-DOMU-A-POSTAVY.md`, zatím neodpověděl) a **podpora ve hře** (doplňky u domů, `poza`
  u postav) – moje práce. Slité domy nechat, dokud hra doplňky neumí.
- **Hotovo (doplňky):** `"druh": "doplnek"` v indexu (glb jako dům, jedna textura, počátek pod patou, předek +x);
  v domě `"doplnky": [{jmeno, poloha, otoceni, meritko}]`. Při `ThreeDObject::Read()` se doplněk vloží do tvaru domu
  (`LayAddons()`: otočený, zvětšený, světlo z otočených normál), trojúhelníky mají texturu `tri_tex` (0 dům, n doplněk
  n), rohy `corner_tex`; zjednodušené tvary (LOD) rohy různých textur nespojí; kreslí mapa, otočený pohled i pohled
  z očí (`GetTextureOf`/`TryGetTextureOf`), půdorys pro chodce je zahrnuje. Ověřeno v rigu (zkušební doplněk na všech
  domech mírného klimatu, mapa 200°/75° a z očí; bez doplňků se obraz nezmění). Neumí: doplňky průmyslu, `noc`
  a světla doplňku, `barvy` doplňku. Kolegovi doplněno do `DOPLNKY-DOMU-A-POSTAVY.md`.

## 18. 10. – stadion: prase na rožni a hasičská Tatra (domluvit, pak kódovat)

- Hráč: na našem stadionu (ten, co sdílí rožeň budovy – `postavy/rozen`, točí se jako v hospodě) občas **obojí
  současně**: prase na rožni i hasičská Tatra s hasiči. Tatru hra bere z GRF 3D Tater; bez něj akce není vůbec (ani
  rožeň) – ale GRF Tater je ve hře zabudovaný („to je ingame truck“), takže tam bude vždycky.
- Kolegovi napsáno (`hra/3d/PROSBA-POZARNI-TATRA.md`): hasičská Tatra jako objekt v balíčku 3D Tater, hasiči jako
  posádka, trojúhelníky „jako normální Tatra“ (zatím rozpočet osobního auta, Tatra 603 má 7 634), přesný limit až po
  hráčově porovnání V3S a Tatry.
- Jak často: „zhruba od do pozpátku“ – tedy zhruba od dvakrát za měsíc do jednou za týden (náhodně 7–15 dní).
- Jen na našem 3D stadionu (ne s vanilla grafikou). Bude tam i **Mat, který točí pivo**, a **holky z hospody si tam
  sednou** (stejné postavy jako v hospodě).
- Jen **nový stadion `dum_20_stadion`** (holky na tribuně), starý ne. Akce trvá **jeden herní den**.
- Kolegovi: `hra/3d/POSTAVY-MODULARNI-A-STADION.md` (prosba o místa akce v záznamu stadionu, klíč `akce`).
- Hasičská Tatra (hráč): celkově méně trojúhelníků, ale mřížka chladiče pěkná, kola podrobná jak jsou (instance);
  doplněno kolegovi do `PROSBA-POZARNI-TATRA.md`.
- **Hotovo (mechanismus akce):** `"akce"` v záznamu domu (rozen/mat/holky/hasici jako `postavy`; bez `postava` hra
  bere `rozen`, `mat_toci`, sedící holky; hasiči jen jménem) → `ThreeDObject::event_figures`;
  `GetThreeDHouseEvent()` je vrací v den akce (dny v blocích po 11, akce na jednom z prvních 4 dnů bloku podle místa
  domu: rozestup 8–14 dní, jeden den), přidané k domu v otočeném pohledu, na mapě na sever i z očí. Rig `test3d akce
  1` = každý den. **Chybí:** místa od kolegy (`akce` v `dum_20_stadion`), hasiči a hasičská Tatra (model; Tatru
  nakreslit jako stojící vozidlo, `tatra.objekt`).

## 18. 10. – modulární postavy (hráč, domlouváme)

- „Postavy budou zvlášť modulární a jedna postava objekt bude všude s tím, co má dělat – kráčet, kouřit.“ Jedna
  postava (kostra) s pózami/animacemi, volaná všude jménem a pózou: domy, hospoda, stadion, chodci, kabiny.
- „Postavy budeme dávat skrz 3D GRF modulárně“: zabudované (ingame) postavy jsou výchozí a nezbytné pro Tatry;
  **GRF Tater si jen zavolá postavy do kabiny** (žádné vlastní kopie holek v balíčku).
- Později v konfigurátoru **jiní řidiči**, které nabídnou tvůrci GRF – postavy zasunovací do hry.
- Kolegovi napsáno (`hra/3d/POSTAVY-MODULARNI-A-STADION.md`): balíček postav ve hře, jeden glb s kostrou a pózami
  jako pojmenovanými animacemi, stejná jména póz u všech postav, odkaz `{"postava", "poza", …}` všude, kopie holek
  z balíčku Tater pryč, později konfigurátor řidičů. Bez `poza` jako dřív.
- **Hra dál:** (1) `poza` a doplňky domů, (2) akce na stadionu, (3) domy na grafické kartě.
- **Hotovo (pózy):** záznam `"druh": "postava"` s `pozy` (každá póza animace kostry; `animace`, `snimku`, `tiku`,
  `smycka`, `krok_m`), postavy `"<jmeno>/<poza>"`, jméno samo = `stoji` nebo první; odkaz `poza` u `postavy`
  domů/průmyslu i `posadka`/`posadky` vozidel (`FigureNamed()`). Ověřeno v rigu na kostře draka (zkušební balíček
  jen v buildu). Formát kolegovi doplněn do `POSTAVY-MODULARNI-A-STADION.md`.

## 18. 10. – 3D na grafické kartě, krok 1: obrázky vozidel, postav a efektů

- Hráč: „proč kreslíme procesorem a ne normálně grafikou… začni tím, že všechny práce s 3D objekty převedeme na GPU“.
  OpenGL hry (video driver) jen ukazuje hotový obraz z procesoru; všechno naše 3D kreslil procesor trojúhelník po
  trojúhelníku.
- **Hotovo:** `three_d_gpu.cpp/.h`. Karta má vlastní vlákno `ottd:3d-gpu` a vlastní kontext OpenGL 3.3 (Windows:
  skryté okno a WGL, Linux: EGL bez okna), nezávisle na video driveru. `RasterObject()` (obrázky aut na mapě,
  v otočeném pohledu, nákup, náhled, postavy, efekty) kreslí na kartě: stejná kamera, světlo, lak, sklo, posádka,
  noční světla, stejné převzorkování 2× a zmenšení. Tvar, trojúhelníky a textury jdou na kartu jednou (podle čísla
  `ThreeDUniqueId`), karta drží max. 384 MB a pouští nejdéle nekreslené. Lak/rez/barvy doplňků a podrobností se
  malují do textury na procesoru jednou za stav a úroveň (ne pro každý pixel každého obrázku), pouští se po 15 s.
- Obrázek z karty je ořezaný na své pixely (procesor měl prázdný okraj od neviditelných trojúhelníků délky).
- Kde karta není nebo něco neudrží (textura větší, než karta zvládne), kreslí procesor jako dřív.
- Ověřeno v rigu (Mesa llvmpipe, tj. „karta“ na procesoru): PPM Tatry 148, 603, 613 i v noci se od procesoru liší
  jen o 1 odstín (zaokrouhlení) a pár krajních pixelů. Rychlost se v kontejneru měřit nedá (llvmpipe není karta).
- Rig: `test3d gpu 0|1` (procesor / karta a co kreslí), `casy` ukazuje čas karty. `kresli <motor> …` (PPM) zase
  funguje (dřív ho přebila větev `kresli [počet]`), `auta` píše číslo motoru.
- **Dál:** domy (`DrawObjectInto`, obrázky domů) a pohled z očí na kartu; lak a rez rovnou v shaderu (bez malování
  textur). Pak limity trojúhelníků (hráč chce můj názor „kolik“).

## 18. 10. – rychlejší 3D GRF: vzkaz kolegovi (grrrrf `hra/3d/ZRYCHLENI-3D-GRF.md`)

- Hráč odsouhlasil: (1) okraje 16 px kolem ostrůvků v atlasu + značka `"okraje_textur"` v indexu (hra pak přeskočí
  pokrytí a vyplňování – **hra ji musí dodělat**), (2) masky menší a šedé, (3) atlas podle nástavby, (4) sloučit
  materiály se stejným obrázkem, (5) skryté díly pryč a vnitřek kabiny jako uzel `interier` (**čtení jen pro kabinu
  dodělat**), (6) opakované díly jako instance.
- Zamítl (7) ikonky nákupu jako sprity v GRF: obrázek do nákupu dělá hra z modelu, jako u simtrains a BeamNG.
- K probrání s kolegou: (8) LOD z Blenderu (uzly `_lod1/_lod2` nebo zvláštní soubory), (9) kvantování glb
  (`KHR_mesh_quantization`, gltfpack). Hra by je musela umět číst.
- Limity trojúhelníků zatím ne: hráč porovná V3S a Tatru (V3S v rigu nemám, rig má jen GRF Tater).

## 18. 10. – pravé tlačítko zaseklé pod Wine: hra už nevrací ukazatel, dokud je tlačítko dole

- Hráč: pravé tlačítko zůstává zmáčklé, dokud ho znovu nezmáčkne (logy myši 17. 10. 01:45 a 13:29). Rozbor na
  straně Winlatoru (`cota/MYS-REPORT.md`): Wine při každém `SetCursorPos` na chvilku chytí ukazatel (XGrabPointer),
  X server Winlatoru tím zahodí držení tlačítka, a puštění v tu chvilku dostane okno plochy a Wine ho zahodí –
  systém (`GetAsyncKeyState`, `MK_RBUTTON`) pak tlačítko hlásí dole. Hra vracela ukazatel ~50× za sekundu, takže při
  dlouhém tažení se puštění do té chvilky dřív nebo později trefilo. Hráč opravu ve Winlatoru ruší: má se spravit hra.
- **Oprava (jen náš režim a otáčení prostředním):** dokud je tlačítko dole, hra ukazatel nevrací (`SetCursorPos` se
  nevolá). Nakreslený kurzor stojí na místě stisku jako dřív, skrytý systémový ukazatel jde volně a mapa jede podle
  jeho pohybu (`win32_v.cpp`, `_pointer_free`). Po puštění se ukazatel jednou vrátí na místo kurzoru (nic není
  drženo, nic se neztratí). Na samotných Windows se vrátí i u okraje okna; pod Wine nikdy, dokud je tlačítko dole
  (tah tam u okraje okna skončí). Vanilla režimy beze změny. Neověřeno (Windows kód se tu nepřeloží) – zkouší hráč.

## 17. 10. – domy až při kreslení; pokrytí atlasu a 0° rozpracované

- Hráč: „jo udělej“ ke třem věcem: (1) glb domů číst až při prvním kreslení, (2) pokrytí atlasu (50–80 ms na atlas),
  (3) obrázky aut při 0° na vlákně (první auto typu 2–3,5 s, každý další směr 10–20 ms na hlavním vlákně).
- **(1) hotovo** (7e363be): index si u domu pamatuje jen cestu ke glb, tvar a textura se čtou při prvním kreslení
  (`ThreeDObject::Read()`, pohled z očí přes `TryRead()` na vlákně). Dřív start četl 140 domů = 136 MB (178 MB
  souborů), ~0,45 s i s teplým diskem. Teď po pohledu na město 23 domů, 39 MB. Obraz pixel po pixelu stejný (mapa
  i pohled z očí). Rig: `test3d domyprecti` přečte všechny, `pamet` ukazuje „domy přečtené z glb“. Index sám
  (průmysly, masky) dál 317–351 ms.
- **(2) změřeno** (rig `DBG=misc=2`, řádky „3D texture … cover“, Tatra 148): atlas 4096×3824 s 936 tisíci
  trojúhelníky: rasterizace pokrytí 125 ms, **zaplavení prázdna 345 ms**, s úrovněmi 470–620 ms; a dělá se **3×**
  (tři textury stejné velikosti se stejným pokrytím). 4096×1712: 45 + 140–190 ms; 4096×1200: 30 + 115 ms.
  Divný případ 512×1024 s 27 856 trojúhelníky: pokrytí 294 ms (asi opakovaná textura, obří obdélníky).
  **Hotovo (a)**: zaplavuje se jen pruh 8 texelů kolem kreslených, ne celé prázdno: 4096×3824 zaplavení 162 ms
  místo 345, 4096×1712 72 místo 140–190, 4096×1200 41 místo 115. Obraz Tatry 148: rozdíl 9 pixelů z 100 tisíc
  (jen odstín, žádná černá ani díry; s pruhem 4 by bylo 43 px). Pokrytí jednoho rozměru a pokrytí se pamatuje pro
  texturu a masky stejného rozvržení (3 poslední); trojúhelník přes celou opakovanou texturu = všechno kreslené.
  **Hotovo (b)**: pokrytí se rasterizuje po řádcích (rozsah řádku najednou, ne test každého texelu): 512×1024
  280 → 27 ms, 4096×1712 ~50 → 21–30, 4096×3824 150 → 113 ms (tam už rozhoduje počet trojúhelníků, 936 tisíc).
  Obraz: pár desítek pixelů jiný odstín, nic černého. Zbývá (c): tři textury 4096×3824 se stejným pokrytím se
  dělají souběžně, pamatování pokrytí pak nepomůže; celkem velký atlas ~300–380 ms na vlákně na pozadí.
- **(3) hotovo**: sprite auta při 0° se dělá na vlákně jako ikonky nákupu (`ThreeDVehicleObjectSpriteStandIn`);
  mezitím se kreslí nejbližší hotový směr téhož auta (při otočení chvilku starý směr), první auto typu nic. Až
  přijde, překreslí se auta z objektů a okna vozidel (depo, seznamy, detail). Rig (Tatra 148 ze zatáčky při 0°):
  nejdelší snímek 12,6 s → 1,4 s (zbytek je první kreslení mapy), pak 103 ms → 1,7 ms, výsledný obraz stejný.

## 17. 10. – kostičky v pohledu z očí, nákupní menu připravené

- **Auta z kostiček, divně natočená** (hráč v #283): v pohledu z očí se textura, kterou stav nijak nemaluje (kola,
  sklo, doplněk bez barvy), nikdy nezapsala – úloha na pozadí ji našla „holou“ a skončila, další snímek se ptal znovu.
  Model nikdy nebyl hotový, kreslil se plakát (obrázek po 5°). V rigu to nebylo vidět, protože rig načítá hned. Teď
  si hra holé textury pamatuje: 355 úloh místo 1880 za 1500 snímků, medián snímku 2,1 ms místo 5,0. Rig: `casy`
  ukazuje „úloh zadáno“.
- **Ikonka v seznamu nákupu** (hráč: „to je malinký obrys auta, co tam fotíš“): dělala se v zoomu 16x (456×179 px)
  z celého modelu a pak se zmenšila na 25×13 px; první Tatra 138 (692 tisíc trojúhelníků, 136 textur) 8,4 s, každá
  další 200–270 ms, jeden snímek okna 10,7 s. Teď se dělá rovnou v zoomu rozhraní, na vlákně na pozadí; do té doby
  prázdné místo, okno se překreslí, až přijde. Ikonky stránku nad a pod viditelnými se zadají dopředu.
- **Výběr vozidla cukal**: i pro vozidla z 3D objektu se při každém výběru stavěla stará „podoba“ z 8 spritů
  (8 spritů v 16x + model 1,3 s) a vůbec se nekreslila. Pryč.
- **Náhled se točí ze snímků z vlákna**: dřív se každý úhel kreslil na hlavním vlákně (22–45 ms). Teď vlákno dělá
  6 úhlů dopředu, náhled se pootočí, až je další hotový (nikdy nečeká hra). Krok 4,5° místo 3° (hráč: „vynech každý
  třetí snímek, bude se točit rychleji“). Snímek okna s náhledem 3–5 ms; Tatra 148 jeden úhel na vlákně ~57 ms.
- **Náhled nemá nic společného s mapou**: netočí se s pohledem mapy (dřív točení mapou doleva = náhled doleva).
  Průmysl taky. Rozměr náhledu se počítá z dílů stavu nákupu (po „tvarech po dílech“ by se jinak točil mimo střed).
- **Voda přes auto náhledu**: pixely náhledu zůstávaly v animaci palety, takže je voda mapy pod nimi při každém kroku
  animace překreslila. Teď je náhled z animace vyjmutý.
- Hráč dál: glb domů číst až při prvním kreslení (start čte 170 souborů, 178 MB), pokrytí atlasu (50–80 ms na atlas),
  obrázky aut při 0° na vlákně (první auto typu 2–3,5 s na hlavním vlákně).

## 17. 10. – zpracování 3D objektů zrychlené (hráč: „až potom, co optimalizuješ celý ten proces“)

- Hráč k hranicím: **„nemůžeš vzít čísla, co má nejvíc, a tam ubrat… musí vycházet z technických možností hry… až
  potom, co optimalizuješ celý ten proces s objekty. nech si dělat 3D GRF tak, aby to odsejpalo.“** Hranice kolegovi
  odvolané (`hra/3d/HRANICE-PODROBNOSTI.md`: dělej dál jako dosud). Nejdřív hra:
- **Glb po kouscích** (`GlbFile`): z glb se čte obsah (JSON, jednou) a jen ty kusy, které čtené sítě a obrázky
  potřebují; čísla rovnou (bez převodu přes double); zadní světla dílů jen místa, ne znovu celé sítě. Tahač 148: 21+22
  MB místo 43+39 MB, 250 ms místo 527 ms (teplý disk); holky v kabinách 11–15 ms místo 16–47. Obraz pixel po pixelu
  stejný.
- **Tvar jen s díly ukázaných stavů**: dřív měl tvar všechny díly všech stavů (návěs Tatry = všech deset nástaveb, 775
  tisíc trojúhelníků). Teď jen díly stavů, které se kreslí; další díl se přidá, když se objeví stav s ním (tvar se
  udělá znovu, starý se pustí mezi snímky). Pořadí textur stejné vždycky. Návěs: 61 549 trojúhelníků za 35 ms místo
  775 309 za 597 ms; s druhým druhem návěsu na mapě 246 797. Tvary v paměti (rig `pamet`): 218 → 104 MB.
- **Zjednodušení pro malý zoom spojuje rohy v rámci ostrůvku textury** (rohy spojené trojúhelníky), ne jen v 1/128
  textury: Tatra 148 při 16 cm 59 tisíc místo 374 tisíc trojúhelníků; kreslení při 1× 3,5 ms místo 11, při 2× 9 místo
  21. Obraz na všech zoomech stejný (rig `test3d kresli` nechá obrázek každého zoomu `kresli_<objekt>_<zoom>.ppm`).
- Technické možnosti (podklad pro pozdější rozhodnutí o ubírání, jedno jádro, textury hotové): obrázek Tatry 603
  (12 tisíc trojúhelníků) při 4× 2,5 ms; Tatry 148 (692 tisíc) 21 ms při 4×, 134 ms při 16×. Načtení tvaru asi
  1 ms na 1 200 trojúhelníků (teplý disk). V zatáčce je nový obrázek po každých 5° otočení.

## 17. 10. – 3D GRF: rychlejší načítání a menší balíčky (návrh), hranice podrobnosti kolegovi

- Hráč: **„potřebujem GRF optimalizovat, aby je hra načítala snadno, připravený musí být, a šetřit při tom místo“**,
  „3D GRF“, „od načítání po zobrazení na displej“, „co formát a jak to zpracovává hra“.
- Změřeno: 3D GRF samy jsou malé (bez obrázků), váhu mají balíčky objektů. Glb jsou z 85–98 % geometrie v plné
  podrobnosti (32bitová čísla), textury jsou malá část. Tahač Tatry 148 čte při prvním zobrazení `nt_navesy`,
  `nt_tahace`, `nt_podvozky` (120 MB, tvar 408 tisíc trojúhelníků): 2 s z teplého disku, 10,5 s ze studeného.
  Při startu hra čte glb všech domů a průmyslu (170 souborů, 178 MB).
- Rekordy: vagon BMZ 692 761 trojúhelníků, M810 518 522, tahač benzína 191 742, návěs otevřený 185 248, jedno kolo
  podvozku 41 175 (řidička College 11 778, holka PUBG 51 211, holka u vozíku 60 413). Domy průměr 18 tisíc, nejvíc
  36 tisíc – v pořádku.
- Hráč: **„nastav mu hranice, ale u holek jsem mu řek, že je chci hezký… zadek u Tatry nemůže být hezčí než holky“**.
  Kolegovi `hra/3d/HRANICE-PODROBNOSTI.md`: holka do 60 000 (nesahat), díl vozidla 10 000, celé vozidlo 40 000
  (z jedné sítě 25 000), kolo 1 000, sklo 2 000, budova 40 000, atlas vozidla 2048². Spočteno na dnešních balíčcích:
  vozidla 5,38 M → 1,22 M trojúhelníků (−77 %), glb 304 → asi 142 MB.
- Návrh pro hru (čeká na hráče): číst z glb jen díly, které stav ukazuje; geometrie menšími čísly (glTF
  KHR_mesh_quantization + EXT_meshopt_compression, nástroj gltfpack, knihovna meshoptimizer – MIT); textury připravené
  po úrovních; kontrolní součty GRF si pamatovat, glb domů číst až při prvním kreslení.

## 17. 10. – plynulost: hra v tiku nevyrábí obrázky aut, glb soubory hned pryč

- Hráč: **„pro mě je teď každej test utrpení… škube se to, nemůžu myší trefit čudlík a obraz se kouše“** – plynulost
  je teď první. Hlavní příčina (rig): hra při každém kroku auta počítá, kam na obrazovce auto sahá
  (`VehicleSpriteSeq::GetBounds()`), a kvůli tomu vyráběla jeho obrázek pro 0° – v tiku hry, ať se hráč díval odkudkoli
  (otočená mapa, pohled z očí). Tatra tahač s návěsem v zatáčce: 32 obrázků, 25,8 s práce (první typ 10 s, čtení
  textur). Celou tu dobu stála hra i myš.
- Teď se to počítá z rozměrů objektu (krabice všech rohů tvaru a posádka, kamerou spritů,
  `ThreeDVehicleObjectSpriteBounds()`), obrázek se kvůli tomu nedělá. Rozměry si objekt pamatuje i po zapomenutí tvaru;
  dokud tvar nikdo nečetl, odhad z délky (nebo 20 m). Rig: otočená mapa a pohled z očí – 0 obrázků (dřív 32); tik hry
  průměr 0,1 ms, nejdelší 2–4 ms. Odhad vždy obklopí skutečný obrázek (log `sprite=3`: „thought …“).
- Soubory .glb se pustí hned po přečtení (dřív se drželo až 240 MB na 15 s): drží se jen během jednoho čtení
  (`GlbFilesScope`), aby se doplněk sdílený záznamy balíčku nečetl víckrát. Rig `test3d pamet`: držené soubory 0 MB.
- Zbývá při 0°: obrázek auta vzniká, když se auto poprvé nakreslí v novém směru (první typ 2–3,5 s kvůli texturám,
  další 10–20 ms), na hlavním vlákně. To řeší další krok z rozhodnutí hráče – auta kreslit objektem.
- Rozhodnutí hráče (17. 10.): tvary všech glb ukládat menšími čísly; domy nikdy obrázky; v nastavení přepínač
  originální domy OTTD (naše krabice) / 3D domy; textury podle potřeby („asi jo“); glb pustit hned po přečtení
  (hotovo); velikost auta z objektu, ne z obrázku (hotovo); obrázky jen pro depo a nákupní seznam; žádných 8 směrů
  dopředu – „udělat objekt a tím končíme“.
- Rig: `test3d smycka` ukazuje i tiky hry (průměr, nejdelší, kolikrát nad 10 ms).
- **Přepínač v nastavení „Domy ve městech: 3D domy / originální domy OpenTTD (jednoduché krabice)“**
  (`gui.three_d_houses`, Grafika vedle domů při 0°). Originální: žádný dům není 3D objekt – v otočeném pohledu a
  v pohledu z očí krabice z ručních tabulek jako dřív, při 0° původní obrázky. Rig (město 41 domů, otočený pohled,
  myš nad mapou): 3D domy průměr 4,0 ms na snímek, originální 2,3 ms.
- **Domy bez obrázků – změřeno, zatím neudělané, čeká na hráče.** Obrázek domu (`TurnedObjectPicture`) není 8 směrů
  jako u aut, je to dům nakreslený jednou pro aktuální natočení a zoom; při pohybu myši nebo aut se z něj jen kopíruje.
  Bez něj se dům kreslí trojúhelníky při každém překreslení: kreslení domů 48 ms → 648 ms na 150 snímků, průměr snímku
  4,0 → 6,8 ms, pomalých (> 20 ms) 5 → 17, nejdelší 41 → 75 ms. Paměť těch obrázků: 15,8 MB (83 obrázků). Paměť berou
  data domů z glb: 421 MB (40 domů).
- **Auta bez obrázků – změřeno, čeká na hráče.** Nakreslit Tatru 148 přímo objektem trvá (jedno jádro, textury
  hotové): 16× zoom 104 ms, 8× 48 ms, 4× 22 ms, 2× 12 ms, 1× 10 ms – za každé auto a každý snímek. Deset aut na
  obrazovce při 4× = 220 ms na snímek (4 snímky za vteřinu). Obrázek auta je mezipaměť nakresleného objektu (0,1 MB
  za 133 obrázků), dělá se na vlákně na pozadí.
- Tvary vozidel po přečtení pustí rezervu, do které jejich seznamy rostly (`ShrinkShape()`).
- **Textury podle potřeby** (hráč: „textury, jak to dělají jiné hry? tak asi jo“): textura vozidla se dělá jen od
  nejjemnější úrovně, kterou něco kreslí (jemnější bez pixelů, jen velikost); když je potřeba jemnější (pohled z očí
  zblízka), udělá se znovu z PNG – na pozadí, mezitím se kreslí hrubší. Každých 15 s (mezi snímky) se pustí úrovně
  jemnější, než se od minula kreslilo; textura, která se vůbec nekreslila, si nechá jen úrovně do 256 px. Stejně masky
  laku, rzi a doplňků a nalakované textury pohledu z očí (ty se nepouštěly nikdy). Hustota texelů se bere z hlavičky
  PNG, ne z rozbalené textury. Rig (Tatry na mapě 0°, otočené, pohled z očí): textury 625 MB → po úklidu 10 MB, po
  dalším kreslení z očí 170 MB; nalakované 184 MB → 0 → 73 MB.
- Obrázky domů otočeného pohledu zaberou při zoomu 4× 16 MB, při 8× 125 MB (strop 256 MB).
- Rig: `test3d kresli [n]` (jak dlouho trvá nakreslit objekt prvního 3D vozidla na každém zoomu), `test3d pamet`
  ukazuje i obrázky domů otočeného pohledu.

## 17. 10. – kolik paměti berou glb a kolik obrázky

- Hráč: **„máme v paměti pro úhel 0 zbytečné obrázky glb objektů a tu paměť potřebujem“ … „kolik zaberou glb a kolik
  obrázky?“** Rig `test3d pamet` (Tatra 148 tahač s návěsem, Tatra 148, 603, malé město 1990, mapa 0°, otočená, pohled
  z očí):
  - glb: tvary vozidel 618 MB (59 tvarů; tvar nese všechny doplňky a náklady svého souboru), textury po úrovních
    811 MB (68; doplněk Tater je obrázek 4096×4096, rozbalený i se zmenšeninami ~85 MB), masky podrobností 61 MB,
    obrázky textur ze souborů 61 MB, držené soubory .glb až 240 MB (pro rychlé čtení balíčku, po 15 s pryč),
    domy 387 MB (textury, nalakované varianty, zjednodušené tvary), nalakované textury vozidel pro pohled z očí 184 MB.
  - obrázky: sprity 8 směrů pro 0° 4 MB (33), obrázky otočeného pohledu 0,2 MB (125). Cache spritů celkem 131 MB
    z 2048 MB.
  - Takže obrázky pro 0° paměť neberou; berou ji tvary a textury glb (1,5–2 GB). Návrhy čekají na hráče.

## 17. 10. – luk do ruky z inventáře, ulička mezi domy, holky na balkonech

- Hráč: **„kliknutí na věc v inventáři nepoužije tu věc, chci vzít do ruky luk, myší i klávesnicí 1, 2, 3, 4 jako v
  suite Morrowindu“** – v inventáři u každé zbraně `[2: vzít do ruky]`, kliknutím do ruky, u té v ruce `[2: v ruce,
  schovat]` (zelená), kliknutím schovat. Klávesy 1–5 jdou chodci i s inventářem v popředí (`HandleKeypress()`:
  okno chodce nebo inventáře s fokusem dostane klávesy první, před stavební lištou, která má 1, 2, … taky).
- Hráč: **„neprojdu uličkami mezi domy, okolo domů je zbytek dlaždice volný“** (+ snímek uličky). Chodec narážel
  kruhem o poloměru 1,4 m (široký skoro 3 m). Teď 0,4 m (`THREE_D_WALKER_RADIUS_M`), půdorys domů po 1/64 dlaždice
  (23 cm, dřív 46 cm), chůze po kouscích 9 cm (běh nepřeskočí tenkou zeď), přední rovina oka chodce 0,25 jednotky
  (u zdi se neprohlíží skrz). Chodec sám se nezměnil (hráč: „jakého menšího chodce?“ – jen ten neviditelný kruh).
  Rig: `test3d chodec volno [dlaždice]` (mapa, kde chodec může stát, po 0,5 m; X = dům bez 3D objektu),
  `chodec klavesa <c> [inventar]`, `inventar klik <n>`.
- Hráč: **holky na balkonech nemají hlavy** („já řek všechny, ale ne tyhle všechny“). Index mírných domů měl u všech
  70 postav `meritko` 2 – na balkonech (domy 06, 18, 1a, 02 ve výšce 3,2–6,7 m) jim na mapě hlava vjela do patra nad
  balkonem. Hráč: „vyser se na kolegu a udělej ty holky velikost“, „hospoda nevím“ – všech 70 holek u domů má teď
  `meritko` 1 (v rigu: holka na balkoně domu 06 celá i s hlavou). Hospoda, pole, socha, automat, Pajda beze změny.
- Hráč: **při pohledu 0° jely Tatra VB a 138 plynule (#181), v jiných úhlech se to kouše – proč, když jsou to 3D
  objekty**. Při 0° se z objektu udělá 8 spritů (8 směrů) jednou a dál se kreslí jako sprite. V otočeném pohledu se
  kreslí obrázek objektu v přesném úhlu po 5° (plynulé zatáčení) – každý nový úhel, zoom a stav je nový obrázek
  (10–60 ms, první u typu ~0,5 s textury). V #181 se dělaly na hlavním vlákně; od `82b1b7d` na vlákně na pozadí
  (bude v #182). Kdyby se pořád kousalo: dopředu na pozadí udělat všech 72 úhlů typu najednou.

## 17. 10. – pohled z očí mimo zámek hry (hráč: „udělej 1 2 3 4“)

- Hráč: **„jak ti bude jedno jádro počítat hru, druhé načítat auta, když 8 jader kreslí first person view“ … „proto
  se to kouše, všechny jádra kreslí, ale žádný nepočítá hru, co se bude kreslit“.** Pravda, ale ne kvůli jádrům:
  kreslení drželo zámek stavu hry po celý snímek (vlákno hry stálo), a co bylo vidět poprvé, se načítalo uvnitř
  snímku, taky pod zámkem. Změřeno v rigu (4 jádra, 1000×559): 24 ms na 4 vláknech, 35 ms na 1 – pixely jsou jen
  část, zbytek dělalo jedno vlákno.
- Teď (`viewport.cpp`, `EyeFrame`): **1)** pod zámkem jen sběr – dlaždice, jejich sprity do textury země, vozidla,
  postavy, textury – do snímku: seznamy trojúhelníků s texturami a pár úloh přes celý obraz (domy po rozích, stíny,
  opar, mlha, noc, obloha, věc v ruce), v pořadí kreslení. **2)** Zámek se pustí, hra počítá dál; snímek kreslí vlákno
  `ottd:3d-eye` (`EyeFrameThread()`), okno ukazuje poslední hotový – o snímek (40 ms) pozadu. Další snímek se sbírá, až
  je ten hotový převzatý, takže se nic nepíše do mezipamětí, ze kterých se kreslí; obrázky vozidel a postav jsou do
  převzetí přišpendlené (`PinThreeDPictures()`), textury objektů se při kreslení nezahazují ani nezapomínají
  (`EyeFrameDrawing()`), při novém načtení GRF a při konci hry se na vlákno počká. **3)** Jádra: hra 1, načítání 1
  (vlákno obrázků vozidel), zbytek kreslí (`DrawingCores()` = jádra − 2: na 4 jádrech 2, na 8 šest).
- **4)** Načítání na pozadí: co pohled z očí ještě nemá (tvar, textura, zjednodušený tvar, nalakovaná textura domu
  i vozidla), si jen „zkusí“ vzít (`TryGetShape/TryGetTexture/TrySimpler`, `TryGetThreeDEyeVehicleTexture`); co není,
  udělá vlákno obrázků vozidel (`ThreeDLoadInBackground()`, úlohy a obrázky střídavě) a věc se objeví o snímek dvě
  později (dům zatím nenalakovaný nebo celý místo zjednodušeného, vozidlo jako plakát nebo nic). Vozidla kreslená
  trojúhelníky (vlastní kabina, auta kolem) se teď i počítají (rohy, kola, posádka) v úloze snímku mimo zámek
  (`CabFigureReady()` pod zámkem jen vybere hotové, `CabDrawFigureModel()` kreslí): sběr kabiny z 44 ms na jednotky.
  Textury domů (`ThreeDObject`) jsou pod stejným zámkem jako vozidel (`MakeOnce`), zahazování nalakovaných jen mimo
  kreslení snímku. Rig kreslí „hned“ a načítá hned (`_three_d_load_at_once`), aby snímky byly z okamžiku.
- Zbývá: první pohled z očí ve hře, kde ještě nebyly domy objekty, čte indexy objektů (4 s) – šlo by číst při startu
  na pozadí. Vlákno načítání je jedno a fronta města má stovku úloh; víc vláken až po buildu.
- Rig: `test3d chodec`/`kabina` kreslí hned (obraz je z okamžiku, snímky sedí); `test3d chodec vlakno 1` zapne vlákno
  jako u hráče. `chodec`/`kabina cas` hlásí `sber X ms, kresleni Y ms`; `chodec mereni` vypíše sběr pod zámkem zvlášť.

## 17. 10. – luk a kuše od začátku; víc vláken po buildu

- Hráč: **„dej tam luk, šípy, kuši a šipky“** – chodec nové firmy má od začátku kladivo, luk, 50 šípů, kuši a 50
  šipek (`WalkerStartItems()` v `walker_inventory.h`). Meče jen přes iddqd (hráč je nejmenoval). Rozehrané hry si
  nesou svůj inventář v savu – tam iddqd (co už je, nedá podruhé, šípy a šipky doplní na 50).
- Hráč: **„máme všichni stroje minimálně se 4 jádry“** … **„vlákna uděláme po buildu“** – víc vláken na obrázky
  vozidel (polovina jader, 2 až 4) až po hráčově buildu s jedním vláknem.

## 17. 10. – vlákno na pozadí pro obrázky vozidel

- Hráč: **„tak jedno jádro procesoru bude načítat a druhé bude dělat obraz?“ … „vlákno na pozadí dej“**. Obrázky
  vozidel na mapě (3D objekt v úhlu pohledu, `ObjectPicture()`) se dělají na vlastním vlákně (`ottd:3d-pictures`,
  `three_d_vehicle_object.cpp`): hlavní vlákno si o chybějící obrázek řekne a vozidlo zatím kreslí nejbližším
  hotovým úhlem (poprvé, než se načtou textury, chvíli nic); vlákno obrázek udělá (čtení a dekódování textur, raster) a
  časovač po 30 ms hotové obrázky převezme a vozidla, která čekala, překreslí. Nejnovější požadavek první (to, co je
  na obrazovce teď), ve frontě nejvýš 128. Totéž pro „plakáty“ vzdálených vozidel v pohledu z kabiny a z očí; postavy,
  kytky, efekty a náhled v nákupu se dál dělají hned (jsou malé, nebo je okno stejně čeká).
- Co se objektu dělá poprvé (tvar z .glb, textury, masky, zjednodušené tvary, posádka, která chodí) teď dělají dvě
  vlákna: `MakeOnce()` v `three_d_objects.cpp` – pod jedním zámkem jen pohled, jestli věc je; dělá se mimo zámek, kdo
  se zeptal první, druhý na ni počká. Stavy složené z vrstev GRF pod stejným zámkem. Mezipaměť tvarů (`_vehicle_shapes`)
  a hustota texelů pod zámkem; BeamNG tvary po jednom (zipy). Zapomínání nepoužitého (`ForgetUnusedThreeDObjects()`)
  přeskočí kolo, když vlákno něco dělá; při novém načtení GRF a při konci hry (`ShutdownGame()`) se vlákno zastaví.
- Rig: `test3d casy` ukazuje zvlášť obrázky dělané hned a na vlákně (`na vlakne … ms (n, ve fronte m)`). Ověřeno:
  první obrázek tahače 463 ms a návěsu 795 ms na pozadí, hra kreslila dál; `quit` uprostřed dekódování počká na vlákno.
- Co zbývá: první objevení vozidla, které ještě žádné okno neukázalo, je asi půl vteřiny neviditelné (textury se
  teprve čtou). Postavy na mapě se poprvé dělají hned (130 ms). Zatím jedno vlákno (víc po buildu, sekce výše).

## 17. 10. – tmavá auta v dálce, plynulé zatáčení na mapě, škubání, kolečka, náhled v nákupu

- Hráč: **auta moc tmavá, 2 m od auta ještě modrá, dál černá (Tatra 603, 613); Tatry nad 4 m černé linky na předku;
  Liaz bez návěsu úplně špatně** (Liaz jedoucí proti mně na 5, 8, 14 m). Dvě chyby v kódu:
  - Zjednodušování modelu podle vzdálenosti (`Simpler()`) slučovalo rohy jen podle místa v prostoru: rohy jednoho
    místa modelu ze dvou kousků atlasu dostaly souřadnice textury jednoho z nich a trojúhelníky mezi nimi kreslily
    z čehokoli mezi kousky – černé a tečkované. Teď rohy z jiného místa textury (po 1/128) sloučené nejsou.
  - Kolegovy atlasy mají každý trojúhelník na vlastním kousku na černém pozadí (u 603 81 % textury). Hra si teď
    z celého souboru modelu spočítá, které texely trojúhelníky kreslí (`TextureCover()`), ostatní vyplní barvou
    nejbližšího kousku (i masky laku a podrobností) a zmenšené textury pro dálku průměruje jen z kreslených texelů.
    Pryč jsou černé čáry na hranách i zčernání v dálce.
- Hráč: **škube to, když se přívěs nebo návěs v zatáčce láme; proč zatáčíme po 45° jako sprity**. Na mapě se vozidla
  kreslila obrázky z modelu v 8 směrech a každý nový obrázek se kreslil hned (prvních 36 obrázků 2,6 s – načítání
  textur, další 15–60 ms za kus). Teď vozidla na mapě mají plynulé natočení ze stopy jízdy jako pohled z očí
  (`EyeOrigin()`), po 5°, návěs se láme na točnici postupně. Nových obrázků se za snímek (40 ms) kreslí nejvýš 12 ms;
  ostatní se do té doby kreslí nejbližším hotovým úhlem a vozidlo se překreslí o chvíli později. První objevení
  nového vozidla (načtení jeho textur) dál stálo asi půl vteřiny – vyřešeno vláknem na pozadí (sekce výše).
- Hráč: **točení koleček do nastavení, výchozí vypnuto** – Nastavení › Grafika › „Kola 3D vozidel se točí“
  (`gui.three_d_wheels`), vypnuto: kola v modelu stojí (pohled z očí a z kabiny).
- Hráč: **3D částice efektů nejsou možné** – úkol zrušen.
- Hráč: **náhled v nákupu přes celé pomyslné okénko** – vozidlo z 3D objektu vyplní čtverec vedle okna nákupu (strana
  jako výška okna, kolik je místa), úhlopříčka půdorysu dílů, které stav nákupu ukazuje, přes celou šířku.
- Hráč: **střed otáčení uprostřed objektu, ne přední nárazník (návěs Tatry)** – náhled se točí kolem středu půdorysu
  (návěs měl počátek v čepu vpředu).
- Hráč: **proč nemám vodu, mám mít všechny průmysly všech klimat** – průmysly jiných klimat jsou nastavení nové hry
  (Prostředí › Průmysl › Průmysl mírného / arktického / pouštního klimatu / toylandu), výchozí vypnutá; voda je
  pouštní (vodárna a vodojem). Se zapnutým pouštním průmyslem je WATR a Tatra na vodu má cisternu (ověřeno v rigu).
  Do rozehrané hry se přidat nedají.
- Rig: `test3d chodec na <n> <m> <stupně>` (chodec u vozidla, čelem k němu).

## 17. 10. – luk, kuše a meče (kolegovy `LUK-SIP-PRO-HRU.md`)

- Hráč: **„je tam luk a kuše“**. Kolega poslal vlastní luk a šíp (ve stylu Example Suite, GPL-2.0), kuši z Example
  Suite se svou šipkou, oba meče z Example Suite a zvuky luku (CC0). Jsou v sadě hry `objekty/predmety/`, CMake bere
  i `.wav` sad objektů.
- **Klávesy 1–5**: kladivo, luk, kuše, meč, obouruční meč (jen ty, které chodec nese); druhá zbraň se tasí, až se
  první schová. F tasí a schová. Meče se máchají jako kladivo (dosah 2,2 a 2,8 m, obouruční bere drakovi 2), držené
  za úchyt z uzlu `uchyt` a pootočené plochou čepele k oku.
- **Luk**: levé tlačítko držet = napnout (zvuk napnutí, za 0,8 s plně, stav `napnuto`, luk se přesune před oko),
  pustit = výstřel (zvuk, rychlost 14–55 m/s podle napnutí); mezerník napne a podruhé vystřelí. Nový šíp na tětivě
  za 0,5 s. **Kuše**: výstřel 70 m/s, šipka bere drakovi 2, za 1,5 s natažená znovu (stav `vystreleno`, pak
  `natazeno` se šipkou v drážce).
- **Šíp letí obloukem** (gravitace), natočený po dráze a kreslený v pohledu z očí trojúhelníky s náklonem (nikdy ne
  zjednodušený, tenký dřík by zmizel). Špička v drakovi: drak dostane zásah (i letící, v jakékoli výšce); v zemi:
  zabodne se tam, kde ji špička protne, 6 cm hluboko, a zůstane 20 s; ve zdi domu se zastaví; ve vodě zmizí.
  Každý výstřel ubere šíp nebo šipku příkazem `ShootWalkerItem`. Vpravo dole je zbraň a kolik k ní zbývá střel.
- Hra čte stavy předmětu jako figury `<jmeno>/<stav>` (jen ten uzel), polohy prázdných uzlů (`GetThreeDThingPlace()`)
  a zvuky předmětu (`GetThreeDThingSound()`); zvuky hry (klakson i luk) jdou přes jednu funkci `GetGameOwnSound()`.
- `iddqd` dá i kuši, šipky a oba meče (inventář má nové věci na konci).
- Při pohledu dolů byl dole na obrázku pruh oblohy: zem blíž než 1,9 m od oka se ořízla. Chodec má teď blízkou rovinu
  0,4 jednotky.
- Rig: `test3d chodec zbran <1-5> [ms napnutí] [kroků|drzet]`, `test3d draci posli drak_hnedy 2`; `test3d` čte
  i záporná čísla (sklon pohledu dolů). Ověřeno: drak dostal šípy 6 → 5 → 4, šipky 4 → 2 → 0 a padl.

## 17. 10. – úvod bez pohybu kamery, cheat iddqd, luk a šípy v inventáři

- Hráč: **úvodní obrazovka bez pohybu kamery, jen občas otočit plochu**. Kamera se postaví jednou na první místo
  z úvodní hry a dál stojí; plocha se jen občas pootočí kolem středu obrazovky (9 s klid, 3,5 s otáčení o 30–60°).
- Hráč: **cheat `iddqd` v konzoli dá všechny zbraně**; co už je v inventáři, nedá podruhé, šípy doplní, luk dá, když
  chybí. Inventář má nové věci `Luk` a `Šípy` (na konci, čísla věcí se ukládají) a druh věci (zbraň, střelivo, věc).
  `iddqd` (příkaz `WalkerCheat`, jen hra pro jednoho) dá zbraň, která chybí, a šípy doplní na 50; napíše, co dal.
- Luk zatím nestřílel: model od kolegy přišel o chvíli později (viz výš, střelba hotová).

## 17. 10. – po nástupu hned řídíš, „Jeď svou trasu“, letadlo, klakson, světla, otáčení úvodu

- Hráč: **hned po nástupu do kabiny řídíš**, místo tlačítka mimo silnici je **„Jeď svou trasu“** (otočeno). Okno
  kabiny i nástup E v pohledu z první osoby převezmou volant: vozidlo jede volně, kam ho šipky natočí (auto, loď,
  letadlo ve vzduchu; vlak jen plyn a brzda). „Jeď svou trasu“ (klávesa T) ho vrátí k rozkazům: auto najede zpět
  na silnici, rychlostní omezení se zruší. Znovu T nebo tlačítko „Řídit“ volant vezme zpátky.
- Hráč: **šipky vždycky do okna kabiny nebo první osoby**, ne na mapu: berou je okno s fokusem, jinak to nejvíc vpředu
  (`ArrowsWindow()`), mapa se se šipkami neposouvá, dokud je nějaké otevřené.
- Hráč: **zatáčení letadla, šipky nahoru a dolů výška**. Letadlo ve vzduchu letí volně směrem z kabiny
  (`AircraftFreeController()`), vlevo a vpravo zatáčí, nahoru a dolů mění cílovou výšku (`cab_altitude`, uložená
  v savu, verze `CabAltitude`) v mezích letových hladin. Na kraji mapy se samo stočí. T ho pošle zpátky na trasu.
- Hráč: **I houká a troubí, F světla**. Vlak houká, jak houká při odjezdu ze stanice (pára, diesel, elektrika,
  monorail, maglev), loď svou sirénou, auto **klaksonem od hráče** (Pixabay, Automobile Horn 02 od Universfield,
  Pixabay Content License) v `baseset/decouple/zvuky/klakson.wav`, načteným zavaděčem zvuků sad. F rozsvítí nebo
  zhasne světla vozidla (i ve dne, i v noci); jen jak to kreslí tento počítač, do hry ani savu nejde. Ve vozidle
  v pohledu z první osoby platí I a F takhle, pěšky dál inventář a kladivo.
- Hráč: **úvodní obrazovka – občas pootočit plochu**. Při zapnutém 3D se plocha 9 s nehýbe, pak se za 3,5 s plynule
  otočí o 30–60° (většinou stejným směrem, občas zpět). Kamera stojí (viz výš).
- Rig: `test3d kabina klavesa <i|f|t>`, `test3d kabina klik trasa`, `testletadlo <x> <y> <x2> <y2>` (dvě letiště,
  letadlo letí mezi nimi), `test3d titulnoc` píše i úhel.

## 17. 10. – řízení šipkami, nastupování v pohledu z první osoby

- Hráč: **auta nejdou řídit šipkami, posouvá se mapa**. Okno kabiny klávesy vůbec nečetlo. Teď šipky řídí kabinu,
  když má okno kabiny fokus, nebo když je fokus na mapě a nějaká kabina je otevřená: vlevo a vpravo jako tlačítka
  (odbočení na křižovatce, mimo silnici a u lodi zatáčení po malých úhlech, držením opakovaně), nahoru plyn (stojící
  vozidlo se rozjede), dolů brzda. Mapa se šipkami mezitím neposouvá.
- Hráč: **v pohledu z první osoby E nastoupí do auta**, bez dalšího okna kabiny (okno první osoby je zvláštní, hra
  pod ním nekreslí). E u vozidla (do 5 m, auto, vlak, loď) přepne obraz okna na pohled z jeho kabiny, šipky řídí
  stejně jako okno kabiny (cizí vozidlo: jen se veze), panel ukáže vozidlo a rychlost. Druhé E vystoupí vedle
  (vlevo u kabiny, jinak vpravo), vozidlo se vrátí k rozkazům jako po zavření okna kabiny. Splněné #74.
- Akce tlačítek kabiny jsou společné funkce (`CabTurn`, `CabSpeed`, `CabStartStop`, `CabLetGo`, `CabArrows`).
- Rig: `test3d sipky <lurd|0>` (držené šipky pro `smycka`), `test3d chodec u <n>` a `test3d chodec e`; `test3d auta`
  udělá z firmy aut místní firmu; `test3d kabina cas` píše stav řízení.

## 17. 10. – náklaďáky Tatra a vagony ve 3D (kolegovy GRF MAX6 a MAX7)

- Kolega poslal dva balíčky: `Nakladaky_Tatra_3D-v2` (24 záznamů, díly podle vrstev v24) a `Vagony_3D-v1`
  (12 vozů po třech článcích). K tomu světlejší Mat (`mat.glb`, `mat_toci.glb` vyměněné) a balíčky mají vlastní
  složky sad (posádka se hledá nejdřív ve vlastním balíčku).
- Ve hře nově:
  - **maska podrobností** (`maska_podrobnosti`): pixel nese index masky z 2D a natře se barvou z callbacku 1C1
    jako maska 32bpp spritu, i s blednutím a otočením odstínu. Konfigurátor, barva podle nákladu a náhodné barvy
    kontejnerů jdou bez jediného stavu navíc;
  - **objekt na článek a spřáhla**: auto, přívěs i návěs jsou každý svým objektem a další visí na `spojeni`
    předchozího. Délky článků bere hra z GRF (`delky_clanku_grf`, metry z `rozmer_m`). Sdílené konce souprav
    (0x0126, 0x013A) se přiřadí podle sousedů;
  - **stav ze všech článků objektu**: návěs má sady v prvním článku, ale visí za prostřední. U vagonů (stavy
    s `clanek`) rozhoduje jen prostřední článek, jak kolega chtěl. Barvy 1C1 jsou z článku, jehož sady se kreslí;
  - díly doplňků se svými okny (`_sklo`), světly (`noc_<díl>`) a koly (`soubor` u `kola_v_modelu`), postavy jako
    uzly (`uzel`), kontejnery jako uzly s posunem;
  - objekt, ze kterého není vidět ani pixel (prázdný přívěs bez přívěsového podtypu), nemá ani světla. Auto před
    ním pak svítí zadními světly svého modelu.
- **Start s balíčkem Tater trval asi 100 s, teď 7,5 s.** Odhad velikosti nákupních obrázků při startu kreslil všech
  24 záznamů a každý dekódoval atlas doplňku (až 4096×3824) znovu pro každý materiál. Teď mají stejné obrázky jednu
  kopii a jednu texturu pro všechny, kreslí se jen textury viděných dílů a soubory glb se v jedné dávce nečtou
  třikrát. Paměť při startu 2,7 GB → 1,4 GB.
- Rig: `test3d auta … p<podtyp>`, nové `test3d vagony <jméno> [počet] [x y] [náklad] [p<podtyp>] [plny] [v<a>:<o>]`
  (najde si volný pruh země), `test3d okno nakup auto <text>` (filtr nákupu), `test3d stavy` píše polohu článků.
  Extra náklady (STUD, MARI, FICR) jsou jen s `economy.extra_industries`.
- Hráč: **předním světlům 148 jsou černé díry**. Data, ne kód: v kabinách 138 i 148 je sklo světla upečené do
  atlasu černě (Cycles peče barvu skla černě) a leží 0,1 mm před světlým reflektorem. Kolegovi prosba o opravu
  (sklo jako okna do `_sklo`, nebo upéct světlé). Jeho obrázky neupravuju.
- Klíče `let` a `krok` u `lak` vagonů hra nečte: o barvě a blednutí rozhoduje GRF (1C1, registry 0x102–0x104).

## 16. 10. – přenos staré hry (decouple 0.10.4 = Palo123 Decouple 2018, save verze 200)

- Hráč: „dokončíme import z decouple 0.10.4, máme stejný orders, naše příkazy jsou jen podrobnější. Nech vlaky
  v depu a já je pustím po jednom. Auta, lodě, letadla nemusí do depa.“ Starý zdroják jsem si stáhl (povolil), takže
  rozvržení savu i význam rozkazů jsou ze zdrojáku, ne odhad.
- Co se ve starém savu liší od vanilly: jeden bajt navíc u každého rozkazu i u rozdělaného rozkazu vozidla (sbalené
  odpojení/spojení), 14 bajtů navíc u každého dílu vlaku (jejich účetnictví rozdělování), tři vlastní typy rozkazů
  (9 jeď spojit, 10 čekej na spojení, 11 odpojit) a bit 6 v podtypu vozu = čelo odstavené řady mimo depo.
- Hotovo: záznamy se čtou vlastními tabulkami polí (ORDR, VEHS – jen v režimu přenosu), rozkazy se hned po
  načtení přeloží na naše: stanice s odpojením + následný „odpojit“ → naše stanice s odpojením a počtem vozů (jejich
  počet počítal i mašinku), „čekej na spojení“ → přepínač na stanici před ním, „jeď spojit“ (u nich bez cíle – vlak
  hledal nejbližší čekající řadu po kolejích) → naše „spojit ve stanici“ se stejnými filtry (plné/prázdné, náklad,
  počet vozů). Stanice se odhaduje z okolních rozkazů (kde po spojení zastavil → hned další zastávka → poslední
  odpojování před tím → poslední/další zastávka) a každý odhad se vypíše do konzole. Čelo řady → náš volný vůz bez
  čísla vlaku. Všechny vlaky s mašinkou stojí zastavené v nejbližším depu (podle kolejí, u „Neviditelné“ mašinky
  bez výkonu podle kolejí mašinky), řady vozů čekají tam, kde stály, s nakládáním, jaké měly; auta, lodě, letadla
  jedou dál.
- Test naprázdno mimo rig (čistý HOME, GRF ze savu stažené z BaNaNaS, 3 hráčovy vlastní GRF chybí): 74 vlaků do
  dep, 59 řad čeká, 301 rozkazů „jeď spojit“ přeloženo bez jediného vypuštění, hra běží, uložit a znovu načíst jde.
  Varování „invalid length“ jsou od vozů z chybějícího CZTR_Wagons_cargo.grf (jen u mě).
- Nepřesné (hra to vypíše): rozkaz „odpojit“ s mašinkou vzadu a počtem > 0 (u nich počet zepředu, u nás od
  mašinky; v savu se nevyskytl); řada, která u nich po odpojení neměla nakládat (u nás nakládá podle rozkazu).
- Vyzkoušet u hráče: načíst s jeho GRF přes tlačítko v okně načtení (nebo `legacyimport on` + `load`), projít
  konzoli (hlášky „Prenos stare hry“), pouštět vlaky po jednom.

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
- Úvodní hra (pozadí menu) byla v 16.0-beta2 arktická, 15.3 měla toyland. Hráč: „udělej ji sám, mírné klima, s
  kamerou; dlouhý den, pak chvilku noc a dokola“. Hotovo: nová mírná mapa 256×256 (1960, české názvy, bez poruch,
  katastrof a růstu měst) postavená příkazem `test3d uvodnihra`. Kolem města Kamenné Pražiště je osmiúhelník trati se
  2 stanicemi a návěstidly, jezdí parní vlak s vozy a motorák s uhlím, ve 2 městech autobusy. Kamera krouží kolem
  okruhu, aby město a vlaky byly vidět vedle menu. Pozadí menu: minuta den, 4 s soumrak, 15 s noc, 4 s svítání,
  dokola; noc menu je vlastní, hráčovo nastavení noci nemění. Za 20 000 tiků se nic nesrazilo.

- Mody BeamNG (hráč: „zkusíme jezdit módama z BeamNG?“, poslal GAZ Volga 24 z Automation, CC BY-NC-SA 4.0). Domluva:
  osobní auto veze 4 cestující, nákladní cokoli kromě cestujících; barva z modu; světla podle modu („BeamNG má
  světla vyznačená a propracované jízdní vlastnosti“). Hotovo: hra čte mody rovnou ze .zip (složky modů BeamNG,
  `beamng_path`, `newgrf/beamng/`), nic z nich nejde do repa ani buildu. GRF v `newgrf/beamng/` jen s čísly:
  výkon z křivky momentu, hmotnost z uzlů, rychlost z převodů a vzduchu, tah z jedničky. Volga 103 kW, 142 km/h,
  1,34 t. Kreslí se z modelu .dae, barvy z materiálů, lak jedna z 5 barev modu na každé auto, díry pro světla a
  masku z masky BC4, svítí skla podle glowMap, světla z props. Model zjednodušený na 110 tisíc trojúhelníků
  (bez rozdílu v obraze), šest Volg na obrazovce nic nezpomalí. Kabina z kamery řidiče.
- Průmysly původní hry ve 3D (hráč: „máme hotový průmysl modulární“, kolega `PROSBA-PRUMYSL-VANILLA-3D.md`, sada
  `objekty/prumysl_vanilla`): 46 společných dílů a 122 dlaždic podle gfx (128 gfx mírného, subarktického a
  subtropického klimatu; Toyland zůstává ze spritů). Podklad, díly s polohou/otočením/měřítkem, stromy, bloky přes víc
  dlaždic (kreslí kotva). Každý díl se čte jednou a textura je jen jedna (sdílí ji všechna umístění). Kreslí se na
  otočené mapě, na sever (jako domy) i v pohledu z očí, v noci svítí okna. Pšenice na polích farmy: mladá při růstu,
  zralá před sklizní, na svahu po terénu; v pohledu z očí do 10 dlaždic. Rozestavěný průmysl jen podklad, animované
  dlaždice zatím jako klidné, okno průmyslu zatím bez 3D náhledu. Holky na polní cestě tu nejsou (pole farmy nemá cestu).
- Hráč: „chození je pomalé u first person view“. Chyba: krok se počítal jen jednou za snímek, zmeškané kroky se
  zahazovaly, takže při snímku 100 ms šel chodec 0,64 m/s místo 1,6 m/s, při 200 ms 0,32 m/s. Teď se udělají všechny
  zmeškané kroky (každý zvlášť proti zdem) a pak jeden obrázek. V rigu 40, 100 i 200 ms na snímek: 1,60 m/s. Rychlost
  sama zůstává 1,6 m/s chůze a 6,5 m/s běh (Shift); jestli je i tak pomalá, hráč řekne.
- Hráč: „auto je při first person vidět, ale má úhly 45° spritu, i když je to objekt, jezdí divně; 45° ze spritů jen
  tam, kde dokreslujeme z 2D“, „Mat nebyl vidět vůbec, spawnul jsem se vedle něj“, „draka taky zblízka a šetři, když
  jsou daleko“, „všechno, je to stejný princip“. Hotovo jedním pravidlem pro všechno z modelů v pohledu z očí (holky,
  Mat, draci, vozidla, okrasná auta): velké na obrázku (od 28 px) = skutečné trojúhelníky v pravém úhlu, menší =
  obrázek, nad 14 dlaždic hrubší obrázek. Trojúhelníky tak podrobné, jak je model velký (slučování vrcholů po 2–64 cm),
  textury podle vzdálenosti. Vozidla se natáčejí plynule podle toho, kudy jedou (dlouhý model od posledního dílu k
  prvnímu). Lak, blednutí, rez, v noci sklo světel a kužely. Mat a okrasné Tatry žijí kolem chodce (dřív jen u otočené
  mapy) a jsou v pohledu z očí vidět. Při tom opraveno: draci zblízka byli obrázek (posazený tvar neměl rozměr) a
  příkaz rigu `auta` stavěl silnici bez spojení s depy. V rigu: Volga i Tatra 613 projíždí zatáčkou, Mat vedle
  chodce, drak zblízka, noc. Dvě Volgy zblízka 27 ms na snímek (bez zjednodušení 52).
- Délka vozidel v pohledu z očí: na rovné silnici či koleji zkrácená na 0,71 jako na mapě (hra řadí po krocích, na
  úhlopříčce je krok √2 delší). Hráč chtěl plnou délku bez překryvu, po vysvětlení: „nebudem tu délku měnit, už to
  chápu s tou úhlopříčkou“. Zůstává.
- Praga V3S jako 3D objekt od kolegy (`hra/3d/PROSBA-V3S-3D.md`, GRF `V3S_3D` MAX4, strom přestaveb jako v3s.grf).
  Hotovo ve hře: doplňky podle stavu (na korbě jen uzel, který stav jmenuje: uhlí, pytle, plachta, marihuana, zvířata,
  lavice…), barva doplňku číslem přes masku, skupiny posádky podle stavu (studentky stojí / sedí), měřítko postav
  (dvakrát všude, i v pohledu z očí), sklo průhledné, kola v modelu se v pohledu z očí točí a přední
  zatáčí, kabina zevnitř (oko v hlavě řidičky, řidička se nekreslí, spolujezdkyně ano). Stejně jde maják Tatry VB.
  Opraveno: první textura je první materiál souboru (u V3S bylo první sklo a lak šel na sklo). Hráč: „kolegovi musíš
  říct, jestli má něco blbě, protože tak udělá i další“ – odpověď v `hra/3d/ODPOVED-V3S-3D.md`.
- Holky na korbě V3S jsem v pohledu z očí zmenšoval na pravou velikost. Hráč: lavice jsou velké naschvál, je to
  pajda V3S s velkými lavicemi pro velké holky, „nezmenšujte mi ty holky pořád“, a to ani v pohledu z první osoby,
  když jsou jen náklad. Zmenšování je pryč, holky s `meritko` jsou dvakrát všude.
- Special do 3D (kolegova `hra/3d/PROSBA-SPECIAL-3D.md`, GRF `SPECIAL_3D` MAX5: dvoukolák a stavební kolečko, hráč:
  „teď předělame special do 3D“, „mužou kráčet holky“). Hotovo ve hře:
  - holky kráčí podle ujeté dráhy, každá svým krokem (`krok_m`). Kreslí se zvlášť, ne ve tvaru vozíku, protože
    tahající a pomocnice mají jiný krok. Platí to na mapě, v otočeném pohledu i z očí. Když vozík stojí, holka zůstane
    v kroku;
  - `otoceni` ve stavu otočí celý model (tlačený vozík je tažený otočený o 180°);
  - stav z vrstev GRF: hra se zeptá na každou vrstvu jako při kreslení a stavy složí, takže kolečko má barvu korby
    i rámu z konfigurátoru.
  Rig: `auta … plny v1:1` (náklad a volba konfigurátoru v depu), `stav a b c` (vrstvy), `kresli … vrstvy … cm`.

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
- Přenos staré hry: hráč zkusí svůj save se svými GRF; podle konzole doladit odhady stanic u „jeď spojit“.
- Přední světla Tater 138 a 148 ve dne černá: čeká na kolegovu opravu kabin (`nt_kabiny`).
