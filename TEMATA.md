# Témata (hra Decouple, forclaude)

Moje průběžné poznámky: co hráč chce, co jsme rozhodli, co je hotové a co zbývá. Píšu je vlastními slovy, nejnovější
nahoře. Commity jsou z větve `claude/github-connection-check-m6m898`.

## Pravidla práce (trvalá)

- Česky, krátce, tykat. Nejdřív domluvit, pak kódovat; dokud nejsou všechny informace, nezačínat.
- Chyba je v kódu, ne v obrázku: žádné zalepování příznaků. Návrh hráče je návrh, ne hotová věc.
- Build (Windows) dělá hráč, ne já. Commit a push po každém hotovém kroku.
- Před pushem kontrola i clangem s `-Werror=c++11-narrowing` (MSVC bere zúžení double → float ve `{}` jako chybu,
  MinGW jen varuje: build 295 umřel na kmenech stromů, 24. 10.).
- Kontrola MinGW s definicemi jako build na GitHubu, hlavně `-DPSAPI_VERSION=1` (build 297 umřel na
  `K32GetProcessMemoryInfo`, s verzí 1 není deklarovaná; hra linkuje `psapi`, správně je `GetProcessMemoryInfo`).
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

## 27. 10. (20) – Holky jen ze sady hry, balíčky dávají polohu

- Hráč: *„budem davat holky náklad i řidičky grrrrf jen definuje polohu. a tak muže sedět mat nebo colege girl
  konfiguratorem. grrrf da pryč řidičky z volhy kabeioletu , holky z tater ale definuje polohy a ktery ingame holky
  použije. to jsou holky grf ktery bude vždy u hry. duležite je předat i velikost holek.“*
- Hra to umí už teď: posádka v záznamu (`postava`, `poloha`, `otoceni`, `meritko`, `misto_ridicky`) hledá postavu
  napřed v balíčku vozidla, pak v sadě hry `postavy` (ta je u hry vždy). Mat nebo College Girl: dvě skupiny
  `posadky`, stav GRF (konfigurátor) vybere skupinu. Velikost: `meritko` místa, postavy sady hry jsou ve skutečné
  výšce.
- Na kolegovi: smazat glb holek z balíčků, holky Tater (uzly v `nt_holky_*`) rozepsat na místa, pózy na míru
  (`college_girl_ridi_v3s`, `galaxia_v3s`) buď pózy hry, nebo jednou do sady `postavy`. SPECIAL (tlačí, táhnou) zatím
  zůstává. Čeká se, jestli kolegovi píšu já, nebo hráč.
- Hráč: *„holky z nákladů bude mít hra a lavičku na kterych sedí taky a dame na návěs valnik dvě lavičky zasebou se
  vejdou“*, *„napiš mu rady a porady jak na to, ja mu pujdu řict že holky bude ingame“*: grrrrf
  `hra/3d/HOLKY-VE-HRE-JAK-NA-TO.md` (8190104).
- **Místa v kusu nákladu** (three_d_objects.cpp, three_d_vehicle_object.cpp, viewport.cpp): záznam kusu má `posadka`
  (jako posádka vozidla, v rámu kusu). Kusů s místy (lavice) stojí tolik, kolik se vejde, i prázdných, když je stav
  ukazuje. Náklad počítá holky (místa všech lavic), sedají od předu v pořadí míst. Kus bez `soubor` jsou jen místa
  (stojící studentky). Velikost `meritko` místa platí na mapě, kartě i z očí.
- Rig se zkušebními místy na kládách V3S (jen v buildu): holky 2× i 1×, mapa, karta i kresli. Hráč: *„naproti holky až
  k bočnici. jsou uprostřed sedi mimo“*, *„maš v3s naloženou dřevem hlavně“* (klády byly jen náhrada lavice) →
  druhá zkouška naproti sobě u bočnic; kolegovi do lavice: naproti sobě, až k bočnici.
- Kolega (`HOLKY-AUTOSAN-SPECIAL-PRO-HRU.md`), hráč: *„zetor autosan přivěs bude vozit studentky . postavy holek z grf
  pryč, napišeš jen polohu pro řidičku , nebo polohu pro náklad holky na lavičce. mame na zastávce že stojí
  vystupují. to bych chtěl zachovat. tatry, v3s , ušetřime spoustu mista. special nefunguje v2,muže za to asi přechod
  v nakladech .“*
  - Kolega pošle lavice a balíčky bez holek najednou; stojící na zastávce jako kus bez souboru; Autosan na STUD (GRF v2).
  - **Chyba v opravě sdílených dílů** (kolega ji našel): když nový balíček díl převzal, seznam sdílených zůstal
    založený starým objektem a nový první objekt v něm chyběl. Teď se seznam při převzetí smaže a založí znovu.
  - SPECIAL v2 v rigu: dvoukolák s bednou i uhlím a kolečko se kreslí, holky táhnou a tlačí, snímky chůze běží,
    jedou pomalu (rychlost 8). Co přesně „nefunguje“, se ptám hráče.
- Hráč: *„modularně aby tvurci mohli rozšiřovat, pozu musí dodat s objektem.“*, *„kryž nedava objekt dava pozu
  řidiče s autem“*, *„jo použije ingame chuzi. to je rozumny. tak mu to napiš“*: holky jsou hry; balíček dává polohu,
  holku hry a `meritko`; pózu, kterou hra nemá, dodá tvůrce se svým objektem (hra hledá postavu napřed v balíčku).
  SPECIAL bez vlastních holek, chůze ze hry. Místa na lavici kusu teď taky hledají postavu napřed v balíčku kusu.
- Hráč: *„všechny mužou chodit stejně, ja nevim proč by měl ufon chodit jinak než mat“*, *„tak jo, ufon je strašidlo
  ale holky nepotřebujou extra každá animaci jinou“*: jedna společná chůze pro holky a Mata (kostra se stejnými
  jmény kostí, chůze přenesená podle kostí). Kolegovi otázka, jestli postavy s kostrou dodá; do té doby SPECIAL beze
  změny. Strašidla a ufon mají svoje animace.
- Hráč: *„mat je strašidlo ale holky mužou chodit stejně. ať začne kostrama. všechny budou kouřit že jo?“*,
  *„počkej až budou stejny kostry, pak budem přenašet. mat je dekorace jako strašidlo“*: společná kostra a animace
  (chůze, kouření) jen pro holky, Mat si nechá svoje jako strašidla. Kolega začne kostrami holek (grrrrf 562442c);
  přenos animací podle jmen kostí až budou kostry stejné.
- Hráč: *„krok 1 hry autosan tam mumlá“*: kolegovi (grrrrf 3dd5078) krok 1 hry hotový od `f9c590f`, na nic
  nečeká. Autosan s `lavice_studentky_v3s` (6 sedí) a `studentky_stoji_v3s` (8 stojí) vyzkoušený v rigu na kopii jeho
  v1 taru, ZTS s řidičkou hry. Zbývá jeho strana: GRF Zvlastni_vozy_3D v2 (Autosan STUD), dva stavy, holky z balíčku pryč.
- Hráč: *„piše grrrrf že dělá kostry“*. Kolega (cb72b5f): sada `humanoidi` (College Girl, Galaxia, pubg Girl s
  jednou kostrou, 22 kostí jako Mixamo), `humanoid_pohyby.glb` (stojí, sedí, za volantem, chůze, kouří) a `dzoint.glb`.
  Hra: přenos pohybů podle jmen kostí (rotace kostí, kyčle krát poměr výšky kyčlí), předmět v ruce (`predmet`: kost,
  poloha, rotace), snímky z `doba_s` (12 za sekundu), pohyby i z balíčku vozidla (napřed tam). Postava
  `<jmeno>/<pohyb>`, v posádce `postava` + `poza`. Rig: všechny tři holky ve všech pohybech; V3S s `college_girl` +
  `za_volantem` sedí jako dnešní řidička. Staré pózy v `postavy` zůstávají, dokud je balíčky používají.
- Zvlastni_vozy_3D v2 (kolega): Autosan STUD, za jízdy GRF vybere stav s lavicí (sprite 1233), řidičky hry. Rig sedí.
- Hráč: *„special sel až v #304 buildu. piše grrrrf? ma hotovo ? co nema hotove?“*. Kolega: další holky na kostře
  (char16, Vivi, Cute Cartoon, Female Girl; převzato) a Special 3D v3 (7 holek hry v konfigurátoru, jen jejich pohyby s
  vozíkem v balíčku). Oprava: snímek chůze posádky se ukládal jen pro prvních 8 chodících celé posádky; Special v3 jich
  má přes 20 (7 holek × 3 skupiny), Vivi a další stály. Teď se počítají jen chodící, které stav ukazuje. Girl #16 má
  textury v JPEG, hra čte PNG: šedá, kolegovi. Nehotové u kolegy: balíčky Tater, V3S, Vagonů bez holek s pózami kabin,
  staré pózy na kostru, dvě lavice za sebou (Tatry v5).
- Hráč (screenshot #304, TAZ 1203 dodávka oranžová bez detailů): *„dvanacetrojky nejdou, chcybi disky kol a barva
  textura, vb je bilo žlutá a je jenom jednou barvou“*, pak *„cpu gpu bez změny“*. Příčina: textury modu jsou DDS
  BC7 (DXGI 98/99), hra uměla jen BC1–BC4 → žádná textura, auto jednou barvou, kola bez disků. Hotovo: dekodér BC7
  (tabulky dělení a kotev ověřené proti PIL, textury 1024 shodné na pixel); skiny BeamNG: díl `paint_design` s
  `globalSkin` → materiál `<materiál>.skin.<skin>`, paleta (`colorPaletteMap`: červená 1. barva, zelená 2., modrá 3.
  z `paints` konfigurace) a nápisy (vrstva s `baseColorMap` + `opacityMap`) zapečené do obrázku těla, lak GRF se na
  ně nedává. Rig: VB modrobílá s pruhem, VB žlutá s bílými dveřmi a VB, Spoje s pruhem, sanitka s křížem.
- Hráč: *„to pada protože neni dokončeny přechod na gpu tak to musime dokončit a pak to mužem ladit . ale flastrovat
  pady nedokončene přestavby je kravina“*: pády pod zátěží (testy 16–18) neřešit záplatami, dokončit přechod na kartu.
- Kolega (20fa77e): lavice jako kusy sady `naklady` (`lavice_studentky` 8 míst, `lavice_studentky_v3s` 6 míst,
  `studentky_stoji_v3s` 8 stojí, bez souboru), V3S bez holek (tar 30 → 9 MB, v kabině postavy hry). Rig: 6 holek
  na lavicích naproti sobě u bočnic, 8 stojících, řidička hry v kabině.

## 27. 10. (19) – Vagony_3D v8: plošiny vozí kusy sady, kontejnery na svých místech

- Hráč: *„mame hotove vagony3D.grf“*. Kolega (grrrrf `hra/3d/VAGONY-3D-V8-PRO-HRU.md`) cituje hráče: *„vagony3d grf
  taky potom bez nákladů. plošinový vagony rola jsou náklady jak auta valnik jen nemaj bočnice tak tam nejde kupa .
  kontainer nahrazuje plachtu když nejsou obrazky nákladu veze se to v kontaineru“*.
- Balíček `Vagony_3D-v8.tar` (md5 4bc941e2…, GRF `MAX7` verze 8): Sgs, Smmps a Holidays mají stavy `naklad` + `loze`.
- **Sada `naklady`:** pět kontejnerů od kolegy (`kontejner_20_modry`, `_zeleny`, `_cerveny`, `_evergreen`,
  `kontejner_40_evergreen`), barvy z textury, nic se nebarví. Licence CC BY 4.0 a 3D Warehouse jsou v záznamech.
- **`naklad.misto` [x, y]** (kolegův návrh, three_d_objects.cpp, three_d_vehicle_object.cpp, viewport.cpp):
  - stav s `misto` položí právě jeden svůj kus na to místo (na `dno` lože), ať je naloženo kolik chce. Jestli tam je,
    rozhoduje GRF sadou;
  - každá vrstva má svůj kus, takže vpředu může být modrý a vzadu zelený kontejner. Bez `misto` by oba měly kus
    poslední vrstvy;
  - kreslení (obrázky na mapě, na kartě i pohled z očí) bere seznam druhů kusů, ne jen jeden.
- Rig (CPU i GL): Sgs s kládami, bednami na paletách a barely ve dvou vrstvách, Holidays se dvěma 20′ kontejnery různých
  barev, Smmps se 40′ Evergreen. Pád rigu `_globset.IsEmpty()` byl od tratí testu, které se křížily (vůz se nenašel,
  trať zůstala), ne od hry.
- Hráč: *„kolega piše, zvlaštni vozy udělame bez nákladů“*. `Zvlastni_vozy_3D` dostane stavy `naklad` + `loze` jako
  V3S: přívěs Autosan za Zetorem (dnes kupy bednám, cihlám, kládám, prknům, pytlům, sudům, dobytku) a balíky Terrna a
  lesovozu. Hra je připravená (`naklad`, `loze`, `barva`, `misto`), čeká se na balíček. Pozor na délku kusu proti
  loži (klády 3,99 m a prkna 4,0 m se na V3S nevešly) a dobytek jen s `bocnice` > 0.
- Hráč: *„udělej požadavky hry“*, *„napiš mu ať ti napiše co piše mně“*: grrrrf `hra/3d/PROSBA-ZVLASTNI-VOZY-NAKLADY.md`
  (e8ff65e). Kolega má psát do `hra/3d` i to, co píše hráči. Autosan: sady → kusy, korba podle jeho `loze/prives.json`
  asi 3,96 m, takže klády a prkna se nevejdou (kratší kusy nebo delší lože, i pro V3S). Cihly `cihly_sede` s barvou
  `A84832`: která? Terrno a lesovoz: `balik` nebo nový kus. Nákup ukazuje tahač i návěs v jednom stavu, ale stav má jen
  jedno `loze`: uzly `*_oba` nechat jen pro nákup, nebo hra udělá `loze` jako pole.
- Kolega (`ZVLASTNI-VOZY-NAKLADY-PRO-HRU.md`), hráč: *„cihly budou sede i cervene a drevo ti piše od včera skratit“*,
  *„lesovoz vozil dřevo, nic neměniš, přesouvaš naklady . když to nejde do ingame nakladů tak ten naklad zustane
  v grf“*:
  - `klady` a `prkna` v sadě `naklady` jsou 3,6 m (jména stejná); vejdou se na V3S (1 svazek klád nebo 2 prken),
    Autosan i Liaz MTS;
  - Autosan (`Zvlastni_vozy_3D-v1`, nový tar md5 c6241ae4…, GRF stejný) bere kusy hry, cihly barvou podle nákladu
    (BRCK/BDMT), `loze` ve stavu těla; kupy mari a ovoce zůstávají jeho;
  - Terrno (kulaté balíky ve třech vrstvách) a lesovoz (9 m kmeny) zůstávají v GRF, nákup taky.
  - Rig: Autosan se 6 prasátky a 2 svazky klád, V3S s kládami i prkny.
- Kolega (`BUILD-PRO-HRU.md`): za grrrrf build nic nebrzdí. Hráči radí nechat v newgrf jen poslední verze (Vagony v8,
  Tatry v4).

## 26. 10. (18) – Návěs přes tahač, V3S s kusovými náklady, barva nákladu ze stavu

- Hráč přes kolegu (grrrrf `hra/3d/TAHAC-NAVES-PORADI-PRO-HRU.md`): *„tatry maj tahač nakreslený přes navěs v jv a jz
  směru. když jedou sv nebo sz tak je to v pořádku. test nákladů dopadl dobře.“* Kolega: článek spojený za jiným,
  jehož tvar sahá před spojku víc než asi o 0,1 m, *„kresli vždy přes článek, za kterým je spojený“*, v obou pohledech.
- **Pořadí** (three_d_vehicle_object.cpp `DrawnOverBefore()`, vehicle.cpp, viewport.cpp):
  - Přesah se bere z `all[1]` tvaru (kam sahají díly všech stavů). Korba návěsu je díl stavu, rám končí u čepu, takže
    `x1` samotného tvaru přesah neukázal.
  - Pohled na sever: v `DrawRoadVehicleWhole()` se díl návěsu přesune hned za díl tahače.
  - Otočený pohled: tahač a návěs jsou jeden záznam k řazení (`AddTurnedVehiclePictures()`), krabice obou, obrázek
    tahače první. Kreslí se jednou v bloku, ať přijde od kteréhokoli dílu.
  - Rig: Tatra 138 tahač s návěsem v zatáčce, 0° i 30°: čelo návěsu leží přes točnici, kabina zůstává celá.
- **Chyba při dvou balíčcích jedné sady** (three_d_objects.cpp): seznam dílů sdílených více objekty míchal objekty
  starého balíčku (složka `Nakladaky_Tatra_3D-v2`) a nového (`-v4.tar`). První díl návěsu se pak přiřadil ke staré
  verzi a ostatní k nové. Teď sdílené díly jen z balíčku, ke kterému díl opravdu patří. Rig vozil se starou verzí
  klády místo beden zboží.
- **V3S bez kusových nákladů** (kolega `V3S-NAKLADY-PRO-HRU.md`, hráč: *„udělej v3s bez nákladů. pajda studentky a
  kupy nech v grf u v3s“*): nový tar `V3S_3D-v1` (md5 c70b69cf…) má stavy `naklad` + `loze`. Kolegova prosba:
  *„Když je `naklad.barva`, ber prosím ji místo barvy z tabulky.“*
  - `naklad.barva` ve stavu → kus dostane vlastní barevný stav (jen kusy s maskou), přednost před tabulkou nákladů.
  - Rig (`test3d kresli` teď kreslí stav s kusy s plnou korbou jako nákupní seznam): cihly červené i šedé, 6 prasat,
    9 ovcí, 2 kravičky.
  - **Klády (3,99 m) a prkna (4,0 m) se na korbu V3S (3,92 m) nevejdou**, auto se dřevem jede prázdné. Napsáno
    kolegovi (kratší kus nebo delší `loze`).
- Zvířata: `zvirata/objekty.json` od kolegy s `rozmer_nakladu` (prase 1,35 × 0,58, kravička 2,29 × 0,95, ovce
  1,13 × 0,56).

## 26. 10. (17) – Trabant na návěsu a bordel kolem čarodějnice: nastavení OpenGL ve Winlatoru

- Hráč (build #303, test 14): *„tady ve směru jihovychod byl vždycky návěs dump i valnik daleko od tahače. a taky
  vezl trabanta s nákladem“*, *„ten navěs je chyba v grf“*, *„už se mi zobrazovali blbosti kolem čarodějnice, když
  hejbu s nastavenim opengl dějou se divny věci“*, pak *„vratil jsem opengl na 4,6 a už se neobjevil problem podobny
  trabantu na návěsu nebo bordel kolem čarodějnice“*.
- V testu 14 měl Winlator `MESA_GL_VERSION_OVERRIDE=3.3` (karta hlásila „OpenGL 3.3 Core, zink Mesa 22.2.5“). Se
  zinkem na 4.6 („OpenGL 4.6 Core“) se cizí tvar v obrázcích ani bordel neukázal. Hra potřebuje 3.3 core a jinou cestu
  podle verze nemá. Je to tedy chování zinku/Vorteku s vynucenou verzí, ne chyba hry. Hra kreslí na kartě jedním
  vlastním kontextem a čísla tvarů jsou jednoznačná.
- Hláška Mesy o `VK_EXT_depth_clip_enable` hry netýká (depth clamp nepoužívá).
- Paměť: v testu 14 (3.3) z očí 2,3 → 4,8 GB za 3 min, karta 750 MB a 56× uvolňování. V běhu na 4.6 z očí
  2,0 → 2,6 GB za 5 min, na mapě nejvýš 3,4 GB. Hlídat dál.
- Mezera návěs–tahač na jihovýchod: chyba v GRF (kolega).

## 26. 10. (16) – Škoda 1203 z BeamNG: barvy, okna, záslepky

- Hráč: *„1203 z beamng neni vidět na silnici, textury nejsou v náhledu v nakupnim menu, jen divny barvy aut“*, pak
  *„sou vidět u tebe ale stojej za prd taky“*, *„divny barvy, okna,“*, *„ja měl materialy hotovy , moje scripty pro
  tyhle 1203 jsou v grrrrf“*, *„materialy jsou zakomentovane a script foti něco jineho“*. U záslepek kombi: *„to je
  okno sklo pod bočnimi panely“*, *„tam ma byt jen plech bez oken vespod pod panelem“*.
- **Materiály** (beamng.cpp, three_d_objects.cpp):
  - Lakované materiály (BeamNG `instanceDiffuse`) jsou vedle sebe v jedné textuře těla (atlas), maska laku jako
    druhá. Lak je tam, kde je maska světlá, v jasu obrázku první vrstvy; guma, chrom a světla zůstanou. Dřív byl
    každý trojúhelník buď celý lakovaný, nebo celý ne (divné barvy).
  - Sklo (`translucent` bez `alphaTest`): `opacityMap` říká, kolik toho zakrývá. Dřív z ní byly díry a sklo bylo
    neprůhledné. Sklo bez mapy zakrývá 77/255.
  - Alfa obrázku neprůhledného materiálu se nebere (BeamNG ji tam nečte): u `main_d` dělala díry do chromu a gumy.
- **Jen přední strany trojúhelníků** (nové, jen auta z BeamNG):
  - Záslepka zadních bočních oken kombi (`S1203_cargo_blind_RL/RR`) je jeden plech: vnější a vnitřní strana leží na
    stejných bodech, jen otočené. BeamNG kreslí jen stranu k divákovi, my obě, a tmavá vnitřní strana prosvítala
    přes lakovanou vnější. To byly ty „okna/sklo“ a tmavé tečkované trojúhelníky.
  - Tvar si teď nese stranu každého trojúhelníku (`ThreeDVehicleShape::facing`). Počítá se z normál až po
    zjednodušení tvaru, protože spojením rohů se může tenký trojúhelník obrátit. Materiály `doubleSided` zůstávají
    oboustranné.
  - Zadní strana se vynechá ve všech čtyřech kresleních vozidel: obrázky pro mapu procesorem (`RasterObject`) i
    kartou, pohled z očí procesorem (`CabDrawFigureModel`) i kartou.
  - Ověřeno otočeným znaménkem: auto je pak vidět naruby.
  - Auta kolegy (glb) nemají `facing`, kreslí se jako dřív.
- Rig: kombi (motor 265) i Volha z obou stran, mapa i oči, procesor i karta. Smoke beze změny. Nevidění na
  telefonu se v rigu neukázalo; nejspíš to byly díry z alfy `main_d`.

## 26. 10. (15) – pěšky přes most nad vodou

- Hráč: *„nemužu jit pěšky přes most, vyjdu nahoru do kopce na most a dál po rovince nemužu . pěšky jsem byl na
  silničnim mostě.“* (A dál: *„byl jsem krmit draky vlastní vypěstovanou marihuanou ze sazenic od matyldy a byl jsem
  se psem v lese. pustim build, skusime náklady a převedem zbytek grrrf na ingame náklady“*.)
- Příčina: chůze pouští na vodu jen pobřeží a nehleděla na to, že chodec stojí na mostovce nad ní. Přes most nad
  dolíkem se přejít dalo, přes most nad vodou ne. Teď platí: kdo stojí na mostovce nad dlaždicí, toho to, co je
  pod mostem (řeka, moře, kanál), nezastaví. Pod mostem u vody se dál chodit nedá.
- Rig: `test3d kanal x1 y1 x2 y2` (kanál pod mostem z `test3d trat`), chodec přejde most z 70,23 na 77,23.
- Další krok: hráč zkusí náklady Tater, pak převedeme zbytek GRF kolegy (V3S, vagony, Autosan) na náklady hry.

## 26. 10. (14) – kusové náklady Tater dává hra (sada `naklady`, stav `naklad` + `loze`), kabina 138

- Hráč: *„mame hasičskou tatru na hřiště a přechazime na ingame náklady, tatry už maj náklady zvlášť“*. Kolega
  (grrrrf `1bccac0`, `hra/3d/TATRY-138-NAKLADY-PRO-HRU.md`): nový tar `Nakladaky_Tatra_3D-v4` (GRF stejný),
  kabina 138 vlastní (`kabina_138`), kusy nákladu už nejsou uzly, ale stav `naklad: {sada: "naklady", kus: …}`,
  a stav korby má `loze` (dno, x, y, bočnice, max_vyska).
- **Sada `objekty/naklady`** je v baseset (`media/baseset/decouple/objekty/naklady`). Každý kus (`druh: naklad`) je
  postava `naklady/<jmeno>` s maskou barvy; barvy z jeho tabulky `naklady` jsou stavy postavy (jako lak vozidla).
  Dobytek je zvíře ze `zvirata` (`pase_00`). Místo na korbě dává `rozmer_nakladu` v záznamu zvířete; dokud ho
  kolega nedá, je to velikost zvířete + 0,2 m.
- **Skládání:** kus z vrstev stavu, `loze` z korby; mřížka celých kusů od předku ložné plochy, uprostřed, rovně
  nebo napříč (`otaceni`), podle toho, kolik se vejde víc. Vrstvy do `max_vrstev`. První vrstva vždy, další jen pod
  `max_vyska` (paleta beden 1,124 m je o 4 cm vyšší než laťky valníku, jinak by se nevešla žádná). Kolik kusů:
  podle naloženého (`naloženo / kapacita`, aspoň 1, když něco veze); v nákupu plno. Barva podle nákladu vozidla
  (`BRAM` hnědé pytle), jinak výchozí barva kusu. Zvířata jen na korbě s bočnicemi.
- Kusy jsou součást stavu vozidla (složený stav nese kus, počet a náklad), takže obrázek se mění sám, když se
  naloží nebo vyloží. Kreslí se na mapě (procesor i karta) i z očí (procesor i karta).
- Rig: bedny (GOOD), prasata (LVST), klády (WOOD), bedny na přívěsu fox, brambory v pytlích, pivo v sudech, korba
  s rukou; cisterna bez kusů. Obrázky aut v rigu chvíli trvají (hráč: *„chvilku trva než se auta začnou kreslit“*),
  rig čeká `test3d dokresli`. `test3d auta` kupuje první auto toho jména, které firma smí koupit, a říká, proč nákup
  nevyšel.
- **Hráč si musí vzít nový tar** `nakladaky3d/Nakladaky_Tatra_3D-v4.tar` z grrrrf (md5 `d0f36b98…`), kabina 138
  přijde s ním.

## 26. 10. (13) – srub: okenice a lucerna na E; rozhledna s podestou; pastviny; minové kladivo; Zetor a kolečko na E

- Hráč: *„co nemá hotové v grrrf?“* → seznam (pastviny, okenice, rozhledna s podestou, společné náklady, Zetor
  a kolečko, minové kladivo). Pak: *„společné náklady znamená předělat všechny grf, takže se to odkládá na větší akci
  úpravy grf. minové kladivo ať bouchne. 5 postavíš tam kolečko ze special a já ho budu řídit. klávesou E si vezmu
  kolečko normální a budu řídit. normální kolečko a zetor z grf rozmístíme“*, *„to dělá hra E nastoupit do vozidla.
  jen ho tam zaparkuj“*, *„jo kolečko se může spawnout znova, traktor, tatra hasičská na hřišti, to jsou dekorace“*,
  *„traktor je normální traktor E budu řídit ale tatra z hřiště nevyjede. úspornější bude normální tatra než dělat
  zjednodušenou další“*, *„zetor taky vozidlo E. dekorace můžou fungovat“*.
- K srubu: *„co tam svítí ve srubu. já si tam donesu lucernu svoji“*, *„je tam lucerna? jak ji zhasnu. to mám lepší
  svoji lucernu“*, *„tak tu lucernu E seberu a je to“*.
- **Hotovo:**
  - Převzato z grrrrf (`98beda0`): `les_paseka_srub.glb` (okenice `okno_00`, `okno_01`), `_interier.glb`,
    `draci_farma.glb` (podesta rozhledny), `prumysl/objekty.json` (klíč `okna`).
  - **Okenice** jsou křídla jako dveře (uzly `okno_NN` vedle `dvere_NN`). E otevře a zavře, vždy ven (od středu
    interiéru), zevnitř i zvenku. Lišta: *„E: otevřít okno“* / *„E: zavřít okno“*.
  - **Lucerna na stole:** u ní (1,2 m) lišta *„E: vzít lucernu“*. E dá do inventáře lucernu a interiér je pak tmavý
    (bez nočního uzlu a světel, `GetThreeDInteriorAt()`). Neukládá se, po načtení hry je lucerna zase na stole.
    Tělo lucerny je zatím v nábytku a záře oken venku v jednom uzlu s lucernou u dveří → kolegovi: lucerna jako
    uzel `lucerna`, záře oken zvlášť.
  - Rozhledna zříceniny: rig od země na plošinu 11,18 m.
  - **Minové kladivo** (par16, `kladivo_minove.glb`): zbraň do ruky (iddqd), máchá se jako kladivo, v okamžiku úderu
    vybuchne 1,8 m před chodcem jako raketa (4 m, draci, čarodějnice, zvířata; chodci nic). Rig: `chodec zbran 10 300 2`.
  - **Pastviny** (`pv_pole_pastvina`, zvířata `krava`, `ovce`, `prase` z grrrrf `98beda0`): pole farmy s dobytkem
    (LVST). Při zakládání pole je každé třetí pastvina 2×2 až 3×3 dlaždice, jen na rovině v jedné výšce, jeden druh
    zvířat. Pole se přes pastvinu nezakládá. Stará farma pastviny dostane, jak zakládá pole dál. Ve hře je to pole
    se značkou v `m8` (druh, místo v pastvině, rozměr; `IsPasture()`), nic se neukládá navíc. Bez 3D je to tráva
    s plotem hry (plot i proti poli vedle). Ve 3D na mapě: kolegova zem, plot na okraji pastviny s otvorem uprostřed
    strany, přístřešek na první dlaždici, koryto na poslední, zvířata (na mapě stojí, z očí se pasou a občas koukají,
    každé jinak). Rig: `test3d pastvina [pokusů]`.
  - **Zetor a kolečko u farmy** (klíč `vozidla` u dlaždic farmy v `prumysl_vanilla/objekty.json`): stojí jako
    dekorace z GRF (Zetor z `Zvlastni_vozy_3D` u dílen, stavební kolečko ze `SPECIAL_3D` opřené o kravín, `naklon`
    se `stred_naklonu`). Bez balíčku na farmě nic nestojí.
  - **E u nich** (do 2,5 m): lišta *„E: vzít si Stavební kolečko“*. E → tvoje firma dostane opravdové vozidlo
    toho GRF zadarmo (příkaz `BuildYardRoadVehicle`), stojí na místě dekorace, mimo silnici, a hned v něm sedíš
    a řídíš jako v každém autě. Dekorace zůstává, jde si vzít další. Z dvora farmy se dá vyjet (volná jízda smí
    po dlaždicích téže farmy, dovnitř zvenku ne). Ověřeno v rigu: kolečko jede krokem (2 km/h), Zetor (traktor
    s vlečkami) vyjel na pole 40 km/h.
  - Hráč: *„zetor má černý neprůhledný skla kabiny asi“*. Oko řidiče z balíčku (`kabina`, 2,25 m) sedí v přední
    hraně střechy, před ním neprůhledná plocha modelu, ne sklo. Hráč: *„joj tak dej kameru před kabinu, kolega zetor
    předělá“*. Hotovo obecně: když oko z kabiny vidí před sebou do 1,5 m neprůhlednou část modelu dřív než sklo,
    posune se těsně za ni (`CabEyeAhead()`); u dobrých modelů se nic nemění. Až kolega Zetor předělá, převezme se.
  - Hasičská Tatra na hřišti zůstává dekorace (stojící Tatra z GRF, nevyjede).
  - Náklon dekorací nejvýš ±60° už při čtení (obrázky mapy víc neumějí; kolečko má v balíčku 65°).
  - **Převzato z grrrrf (`eb9e9b3`, `8cba34a`):** srub bez lucerny – uvnitř ani stůl, ani svit, ani světlo; venku
    bez záře oken a dveří, zůstalo jen uhlí ohniště (`svetlo_00`). Kolečko u kravína `naklon` 60, osa blíž ke zdi.
    Zetor: čelní sklo celé v `zts_sklo` (nový tar `Zvlastni_vozy_3D-v1`, md5 `a009f41f…`, GRF stejný) – z kabiny je
    vidět ven sklem, stěrače, kapota; kamera se už neposouvá (`CabEyeAhead()` zůstává pro jiné modely).
  - **„E: vzít lucernu“ z hry pryč** i se zhasnutým interiérem (`GetThreeDInteriorAt`, `TakeThreeDLamp`): hráč
    *„lucernu pryč a hotovo“*. Navíc by se nabízelo u každého světla interiéru (lampy v kině). Ověřeno v rigu
    v noci: srub uvnitř tmavý, okna a dveře venku nesvítí, okenice dál na E.
  - Hráč: *„lucernu dej pryč s celou tou září. mám v inventáři svoji lucernu“*. Lucerna i záře jsou v kolegově
    modelu (tělo slité v `nabytek`, svit `noc` + `svetlo_00` v interiéru; venku záře oken `les_noc_okno`
    a `svetlo_00`–`02`). Prosba kolegovi `hra/3d/PROSBA-SRUB-BEZ-LUCERNY.md`: vyhodit je, uhlí ohniště nechat.
    Ruší prosby o uzel `lucerna` a `noc_okna`. Až pošle, převzít; „E: vzít lucernu“ pak zmizí samo (není světlo).
  - Kolegovi do grrrrf `hra/3d/ODPOVED-PASTVINY-OKNA-ZETOR.md` (`b273059`): co je hotové, prosby lucerna jako uzel
    `lucerna`, záře oken srubu zvlášť (`noc_okna`), Zetor: čelní okno celé do `zts_sklo` a oko `kabina` níž.
- **Odloženo:** společné náklady (sada `naklady`) až s velkou úpravou všech GRF.

## 26. 10. (12) – Krmení draků na farmě (E → nabídka, mari kouř, pivo oheň z tlamy, seno nic)

- Hráč: *„chci krmit na farmě draky, když k němu přijdu dám mu mari a zakouří se kolem draka a dám mu vodku, pivo
  a bude oheň kolem draka nebo radši kouř a oheň z tlamy“*. Domluva: *„1 E vyskočí nabídka krmit draka a tam bude co
  mám co mu můžu dát pivo a mari a když vyberu tak okno výběru zmizí a drak zakouří nebo vypustí oheň. 2 bez vodky,
  3 podívaná jenom 4 seno taky budem krmit senem a po senu neudělá nic ani kouř ani oheň“*; *„jo dej krmení draků“*.
- **Hotovo:** u draka farmy, který odpočívá na svém místě (ne ten, co krouží nebo leze do jeskyně), ukazuje lišta
  *„E: nakrmit draka“* (do 4 m od jeho těla, na stejné úrovni – ne drak na střeše hradu). E otevře okno *„Krmit
  draka“* s tlačítkem pro každou věc, kterou chodec nese (pivo, mari, marihuanové seno, s počtem); nic nenese →
  *„Nemáš nic, co mu dát“*. Klik: jedna věc ubude (příkaz `FeedWalkerItem`, nic neléčí), okno zmizí.
  - **Mari:** 8 s kouří – z tlamy dýchne obláček kouře každé 2 s, kolem celého těla oblak, stoupá.
  - **Pivo:** chvilku pije, pak 2,5 s chrlí oheň z tlamy nahoru do vzduchu (sloup, ať je vidět odevšad a nešlehá
    chodci do očí), nad ním tmavý kouř, pak trochu kouře z tlamy. V noci oheň svítí kolem (jen každý šestý plamen,
    ať to není drahé).
  - **Seno:** sní, nic dalšího.
  - Jen podívaná, žádný vliv na hru. Okno se zavře, když chodec odejde, nastoupí, drak odletí nebo Esc (Esc zavře
    jen nabídku, ne pohled z očí).
- Rig: `test3d chodec drak <n> <m> <úhel>` (chodec před n-tým odpočívajícím drakem, dá mu po 2 kusech, když nemá),
  `test3d chodec klavesa e`, `test3d chodec krmit pivo|mari|seno <kroků>`. Ověřeno na dračím hradu: zepředu
  i z boku, nabídka mizí po výběru i po odchodu na 12 m.

## 26. 10. (11) – Škoda 1203 z BeamNG (par15): kola, textury, lak podle masky

- Hráč (screenshot z #300, nákup v depu): *„dvanáctsettrojky se nepodařily kolečka a textury“*, odkaz na release
  grrrrf `par15` (zip `vehicles-…`: `vehicles/S1203/` a `vehicles/common/S1203_wheels/`). Pak: *„v #301 ten
  screenshot auta vypadá stejně, nejsou kola a textury“* a *„to máš vzadu plechy místo oken, to je několik aut
  v jednom. barvu nedáš, tam je 6 karoserií na sobě a šest oken“*.
- **Příčiny (rig s jeho zipem, stejný obraz jako u něj):**
  - Kola leží v `vehicles/common/S1203_wheels/` (díly, `wheel.dae`, materiály, textury). Hra četla jen složku
    auta, takže kola neexistovala a nevznikla ani náhradní.
  - Na místo kola posouvá slot podvozku (`nodeOffset`) výrazem s proměnnou:
    `$=case($trackwidth_F == nil, $trackoffset_F+0.18, $trackwidth_F)`.
  - Materiály jmenují `…color.png`, ale soubory jsou `.dds`, u kol obráceně. BeamNG si příponu přehodí sám.
  - Karoserie má lak až ve druhé vrstvě s maskou (`opacityMap`): lak je tam, kde je maska bílá. Pryž, chrom
    a světla jsou v textuře první vrstvy. Hra natírala celý materiál jednou barvou.
- **Hotovo (`beamng.cpp`):**
  - díly, modely a materiály se čtou i z `vehicles/common/` modu (zip i složka);
  - modely odtud jen tehdy, když síť chybí v modelech auta; každý model má vlastní jednotky a osu nahoru;
  - proměnné dílů (`variables`, výchozí hodnota) a `vars` z `.pc`;
  - výrazy `$=` (+ − * /, závorky, porovnání, `nil`, `case`, `min`, `max`, `abs`);
  - `nodeOffset` slotu posune uzly i flexbody dílu a jeho poddílů, x od středu na obě strany;
  - textura s druhou příponou (`.dds` ↔ `.png`); pneumatika bere texturu druhé vrstvy, když je v první prázdná
    `common/null.dds`;
  - lakovaný materiál s maskou laku: trojúhelník se středem na tmavé části masky má texturu první vrstvy,
    ostatní lak (stejně jako díry podle masky).
- **Rig:** 1203 kombi (COM) i mikrobus mají kola (pneumatiky, poklice) na svých místech, sklo a detaily.
  Kreslí se jen díly zvolené konfigurace, jedna karoserie (`S1203_body_74`). Plechy vzadu jsou u COM díly
  `S1203_blind_side_RL/RR`, které `COM.pc` dává místo zadních bočních oken; mikrobus (`passenger.pc`) má okna.
  Smoke beze změny.
- Čeká: jestli se COM má jmenovat jinak než „kombi“ (zeptat se hráče); krmení draků (domluveno, kódovat).

## 26. 10. (10) – rozhledna zříceniny: chodec vyjde až na plošinu

- Hráč: *„dračí farma s dřevěnou rozhlednou, zaseknu se nahoře poslední kousek na plošinu rozhledny, na druhé farmě
  kamenné schody jsou v pořádku a posed v lese má schody taky v pořádku“*. Pak: *„jo tak ať si tu rozhlednu schody
  dřevěné předělá, když tam nestačí něco jednoduše spravit, co tam vymýšlíš“* a *„ať to převezme od tebe, když si to
  spravil“*: oprava zůstává ve hře, kolegovi jen zpráva.
- **Příčina (rig, trojúhelníky modelu):**
  - Schody (y 4,69–5,69 m) končí v rohu osmiúhelníkové plošiny 11,18 m.
  - Koncový sloupek zábradlí (y 5,02–5,12) stojí v šířce schodů a pravá půlka schodů vede kolem šikmé hrany
    do vzduchu.
  - Volně je 0,58 m mezi sloupkem a madlem. Mřížka kolizí (0,23 m) z toho nechala 0,23 m, chodec má 0,4 m.
- **Hotovo:**
  - Kolize nahoře na budovách po 0,12 m (`FOOTPRINT_UP_CELLS`).
  - Chodec obejde, co mu stojí trochu stranou (`FirstPersonMove()`): šikmý krok, nebo nejdřív úkrok bokem
    do 0,3 m, když odtamtud jde dál. Klouzání podél zdi jen tam, kde se po ní opravdu postoupí (dřív se nulové
    klouzání počítalo jako pohyb). Rig chodí stejně.
- **Rig:** zřícenina od země na plošinu a přes ni k protějšímu zábradlí. Hrad 7 m a posed 2,84 m beze změny.
  Smoke beze změny.
- Kolegovi `hra/3d/ODPOVED-ROZHLEDNA-ZRICENINY.md` (grrrrf `7ca4ace`): co v modelu je, co hra dělá a jak schody
  stavět příště.
- Čeká: okna srubu (dveře jdou, okna ne), domluva krmení draků.

## 26. 10. (9) – první blok mapy 10 s: rejstřík objektů na vlastním vlákně, domy se chystají na pozadí

- Hráč: *„co to dlouhé načítání jednoho bloku 10s, to si pokračuj, ne že to zahodíš“*. V deníku z telefonu
  `mapa … nejdel 10710 ms` a `hodiny … nejdel 11323 ms`: první vykreslení mapy po načtení hry.
- **Příčina (rig, měřeno po částech):** první blok mapy 705 ms. Z toho čtení a malování 3D domů a průmyslu na
  hlavním vlákně a **čtení rejstříků `objekty.json` (204 ms)**, které se dělalo až při prvním kreslení domu.
  Z rejstříku 150 ms ze 173 dělají průmysly: `prumysl` a `prumysl_vanilla` čtou při čtení rejstříku glb svých dílů.
  Sprity z GRF to nejsou (2 ms).
- Hráč k rejstříkům: *„north house bez sněžný čáry se načítaly a nevědělo se co s nimi naco“*, *„používáme
  pouštní, ale arktický můžou být jen se sněžnou čárou“*. Změřeno: rejstřík subarktických domů stojí 0,3 ms,
  jejich glb se čtou až při kreslení. Brzdil průmysl, ne sníh.
- **Hotovo:**
  - Rejstříky se čtou na vlastním vlákně hned po skenu GRF při startu hry (`StartReadingThreeDObjects()`).
  - Mapa na ně nečeká (`ThreeDObjectsIndexRead()`): do přečtení kreslí domy, průmysl, lampy a pšenici jako sprity
    a pak jednou překreslí celou obrazovku. Co jiného je potřebuje (z očí, kabina, zvířata), počká na vlákno.
  - Sken tarů GRF počká na vlákno.
  - Každý dům a průmysl se před prvním kreslením chystá na pozadí (`ThreeDObjectReadyToDraw()`): tvar, textura
    a nabarvení. Mezitím je vidět jeho sprite a jeho místo se překreslí, až je hotový.
- **Rig:**
  - První blok 705 → 113 ms.
  - S uměle zpomaleným rejstříkem (4 s) se první blok nakreslil za 77 ms jako sprity. Po 10 s se obraz liší
    od normálního běhu o 4 pixely animace.
  - Úvodní obrazovka OK. Smoke beze změny.
- Deník hráče (11:27–11:32, starší build): *„hejbe se to pěkně“*, mapa i oči ~110–120 snímků/s. Při načtení je
  pořád `mapa nejdel 10221 ms`, to opravuje tenhle krok. V 11:31:07 blok 973 ms: 95 obrázků objektů vozidel
  na hlavním vlákně (977 ms), zbývá.
- **Nové od hráče (čeká):**
  - Dřevěná rozhledna dračí farmy: zasekne se nahoře, poslední kousek na plošinu. Kamenné schody druhé farmy
    a posed jsou OK.
  - Srub: dveře jdou zavřít, okna nejdou otevřít.
  - Krmení draků (domluvit): *„dám mu mari a zakouří se kolem draka, dám mu vodku, pivo a bude oheň kolem draka
    nebo radši kouř a oheň z tlamy“*.

## 26. 10. (8) – otočenou mapu skládá karta, krok 1: zem rovnou z karty, nic se nečte zpět

- Hráč: *„už je to dobrý, cota radil ať nedávám 22.2.5 a s ním to funguje“* (nastavení vyřešené: Zink 22.2.5),
  *„pokračuj gpu přechod“*, *„nelogický věci“*, *„otočenou mapu ať skládá karta“*.
- **Plán:** (1) zem otočené mapy kreslí karta do vlastního obrázku pohledu a ovladač videa ji ukáže pod
  obrazovkou hry (jako snímek z očí); (2) všechno stojící (domy, auta, krabice, mosty, sprity) kartou najednou,
  jinak by nesedělo pořadí. Pak procesoru zůstanou nápisy a okna.
- **Krok 1 hotový:**
  - Karta drží obrázek zemi hlavního pohledu (`ThreeDGpuGround::into_view`, `ThreeDGpuViewGround()`),
    blok do něj klade na své místo, žádné `glReadPixels`.
  - Pohled se posune → karta posune obrázek (dvě kopie přes pomocný obrázek). Co je položené, se pamatuje po
    pixelech jako u „zem z pohledu“ (`TurnedCardGround`): otočení, přiblížení, změna zemi = znovu.
  - Blok na obrazovce je díra (alfa 0). Co stojí, kreslí procesor do díry jako dřív.
  - Blitter 40bpp-anim nad dírou počítá i alfu (`ComposeOver`, `DarkenOver`), takže stíny, okraje a písmo jsou
    nad zemí z karty správně. Nad neprůhledným pixelem vychází stejně jako dřív.
  - Ovladač videa skládá obrázek karty s obrazovkou jako „přes“ (`s.rgb + (1 − s.a)·karta`) a zem kreslí pod
    obrázkem z očí (`under`).
  - U screenshotu, bez sdílení kontextu (vypnutá HW akcelerace) a u jiného blitteru zůstává stará cesta se čtením
    zpět. Blok, kde zem položil procesor, je celý neprůhledný.
  - **Noc kartou taky:** ovladač videa obrázek zemi ztmaví měsíčním světlem (`light`, jako `DarkenRect`
    zbytku), světla lamp a aut se přičítají a průhlednost nechávají (`AddNightLight`, `AddNightBeam`). Rig: noc
    karta proti čtení zpět stejně, úvodní obrazovka 90 s celou dobu z karty.
- **Rig:** `test3d zem ukaz 0|1`. Sdílení kontextu v rigu jen s `SDL_VIDEO_X11_FORCE_EGL=1` (jinak SDL jede přes
  GLX a karta se nesdílí). Otočení 30° a 52°, posun, úvodní obrazovka 90 s se střídáním dne a noci: obrázek
  stejný jako se čtením zpět; liší se jen pole, zvířata a datum, které se mezitím změnily samy.
- **Zbývá (krok 2):** stojící věci kartou (krabice, mosty, domy z obrázků s hloubkou, auta, sprity), pak domy
  shora bez čtení zpět. Měřeno na telefonu (test 6, otáčení): zem 155 ms na snímek (krokem 1 pryč), stojící
  věci dohromady ~50 ms (sprity 18, plakáty 17, modely 10).

## 26. 10. (7) – deník z telefonu (test 5–8): domy kartou rozbité = Zink 25.1.9, ne jádra

- Hráč: *„asserty odstraníme na konec“*, *„ano další velký krok uděláme“* (skládání mapy kartou, #134),
  *„potřebuju spravit nastavení“*. Test 5–8: domy a průmysl kartou rozbité, procesorem dobré; myslel, že to
  začalo zaškrtáváním jader 0–3 ve Winlatoru a nejde vrátit.
- **Příčina je verze Zinku, ne jádra.** Test 4 (domy dobré): `zink (Vortek)`, **Mesa 22.2.5**, OpenGL 4.6.
  Test 5–8 (domy rozbité): `zink Vulkan 1.3 (Vortek)`, **Mesa 25.1.9**, override 4.5 a pak 4.3. Jádra jsou
  zpátky (session.txt: affinity `0xff`, všech 8). Cota vidí tutéž poruchu u 25.1.9 i se starým buildem (PRO-HRU
  §5.6: Mali nemá `VK_EXT_depth_clip_enable`, podezřelé R8 textury), rozbor u nich běží.
- **Nastavení:** Winlator → kontejner → grafika → OpenGL Zink, verze **22.2.5**, GL verze jako v testu 4 (4.6).
  Nový build má obrazovku ve vlastní paměti (`2c5fe7b`), takže OpenGL ≥ 4.4 už nevadí.
- **Vypnutá HW akcelerace a karta:** dva různé vypínače. HW akcelerace = jak se hotová obrazovka dostane na
  displej (OpenGL / bez). „Drawn by the graphics card“ = 3D objekty kartou. Proto test 8 kreslil kartou i bez HW
  akcelerace (kontext karty bez sdílení).
- **Nový build na mapě:** test 7 mapa většinou 25–50 snímků/s (průměr 20–40 ms), build #300 v testu 4 měl
  dlouhé úseky po 3–4 snímcích/s. Občas snímek 0,4–0,9 s (`okna`). Start: jeden tik 32 s (zámek 21 s,
  okna 11 s).

## 26. 10. (6) – přechod na GPU: deník a zpráva z cota

- Hráč: *„bež dělat gpu, build už něži. naklady odložime. temata denik přechodu na gou a zprava z cota“*, pak
  *„cota hlavně identifikoval nějaky nelogicky věci“*: 207 ms procesoru kolem pohledu z očí, domy shora čtené
  zpět z karty, zem shora po „vše z karty pryč“ kreslená procesorem. Sada `naklady` odložena.

### Zpráva z cota (PRO-HRU.md §5–§8, 25.–26. 10.)

- **2–4 fps = nekešovaná paměť karty (§7, potvrzeno).** Od OpenGL 4.4 hra kreslila obrazovku rovnou do bufferu
  karty a Vortek ho na telefonu dává nekešovaný: každé čtení pixelu procesorem ~250 ns. Čtou plátno očí
  (`RealizeAnimation`), otočená mapa, míchání obrázků, noc (`DarkenRect`) a posun. Proto ~207 ms na snímek
  z očí bez ohledu na scénu (cena roste s plochou). **Řeší `2c5fe7b`** (obrazovka ve vlastní paměti), hráčův build
  **#300 = `090bc46`** ho nemá. Do nového buildu cota radí OpenGL pod 4.4 (VirGL 4.3).
- **Start:** obří tik 22–33 s (`OnNewGRFsScanned` bez přerušení), první snímek titulky 11,2 s čekání na 3D vlákno.
- **Dlouhé snímky brzdí hru:** tiky z 37/s na 4–13/s.
- **Jádra:** hra běží jen na 4 velkých jádrech (A78), `hardware_concurrency` hlásí 8.
- **VirGL (§8):** černé domy = chyba jejich hostitele (dva výstupy shaderu bez `layout(location)`), opraveno
  v buildu 13. „Vše z karty pryč“ = GL_OUT_OF_MEMORY, protože jejich guest ovladač sdílí jeden socket mezi
  GL vlákny bez zámku (my kreslíme ze dvou: video + 3D). Opravují, hra zatím správně přejde na procesor.
- **Zink 4.5:** modrá ulice, rozbité textury (Mali nemá `depth_clip_enable`), rozbor u nich běží.
- **Release build / asserty vypnuté:** návrh cota. Rozhoduje hráč: asserty dávají při pádu přesné místo.
- **Do deníku chtějí:** plochu překreslenou za snímek a časy fází startu.

### Co z toho bylo nelogické a proč

| co | proč | stav |
|---|---|---|
| 207 ms kolem pohledu z očí | čtení pixelů z nekešované paměti karty; plátno očí navíc četlo pixely zbytečně | `2c5fe7b` + dnes `ClearAnimation` |
| domy shora čtené zpět z karty | mapu skládá procesor ze spritů, obrázek domu se čte **jednou** a drží se | zmizí až se skládáním mapy kartou (#134) |
| zem shora: karta, pak „vše z karty pryč“ | GL_OUT_OF_MEMORY od chyby VirGL guestu, procesor je záloha | oprava u cota; v #300 se zem kladla a četla každý snímek, od `c903708` se drží, dokud pohled stojí |

### Deník přechodu na GPU (co kreslí karta)

- 18.–21. 10.: pohled z očí celý na kartě (#151), ukázaný bez čtení zpět (#152), obrázky budov po dávkách (#153),
  zem otočené plochy kartou (#154).
- 25. 10.: dávky obrázků budov bez čekání hlavního vlákna, domy při otáčení z posledního obrázku (#190).
- 26. 10.: obrazovka ve vlastní paměti (`2c5fe7b`), položená zem pohledu držená (`c903708`).
- **26. 10. dnes:**
  - plátno očí jen smaže bajty animace, pixely nečte (`Blitter::ClearAnimation`);
  - start na 3D vlákno nečeká, do té doby kreslí procesor (`ThreeDGpuAvailable`, čeká jen rig `test3d gpu`);
  - po snímku delším než 100 ms dožene herní vlákno zmeškané tiky, nejdéle 100 ms (`VideoDriver::SleepTillNextTick`);
  - vlákna podle jader, na kterých hra smí běžet (`GetUsableCores`: Windows `GetProcessAffinityMask`, Linux
    `sched_getaffinity`).
  - Rig (llvmpipe): z očí při 3–4 snímcích/s hra stihne 19–24 tiků/s.
- **Zbývá:** 2D mapa (sprity terénu, vozidel, budov) je pořád procesor. To je jediné, co zruší 2–4 fps při pohybu
  mapy. Skládat otočenou mapu kartou (#134, domluvit), vozidla objektem (#127), domy bez obrázků (#123), sběr
  scény z očí na kartě (#150).

## 26. 10. (5) – blednutí aut BeamNG, oprava kolegy (stříška posedu, pata schodů), chůze na úzké schody

- Hráč: *„blednutí dáme hned, a máme to opravené“*.
- **Blednutí** (kolega `BLEDNUTI-1203-PRO-HRU.md`): každá barva laku z modu má 21 stavů, jeden na celý rok stáří
  (`paint_years`). Rok po roce 1/20 cesty k `(barva + B8B8B2) / 2`, po 20 letech konec. Bledne jen lak (materiál
  barvy auta), sklo, chrom, světla a holka ne; v nákupu nová. Všechna auta BeamNG. Rig: Volha nová / 10 / 20 let.
- **Převzato `15fdb06`:** stříška posedu v 5 m (2,16 m nad plošinou), u paty schodů zříceniny odsunuté kvádry.
- **Chůze:** výplň „kam chodec dojde zvenku“ počítá s tělem 0,2 m (`THREE_D_WALKER_BODY_M`), ne s kruhem 0,4 m –
  metrová chodba schodů zříceniny byla jinak plná. Chodec je úzký (0,2 m) i u paty schodů (schod pod kruhem). Rig:
  zřícenina 0 → 10,9 m, posed 0 → 2,84 m a po plošině, hrad dál 0 → 7 m.
- Rig: `test3d trojuhelniky <x> <y> <x0> <y0> <x1> <y1> <z0> <z1>` (cm) – trojúhelníky objektu průmyslu v krabici.

## 26. 10. (4) – les a obě dračí farmy od kolegy (1513879): hrad i zřícenina, drak do jeskyně pěšky, schody posedu

- Hráč: *„ušetříme když náklady budou ingame?“* (kolega: 35 MB nákladů v GRF → sada `naklady` 220 kB),
  *„náklady po buildu, už mi tam dej les, dračí farmu“*, *„dračí farmu máme dvě“*.
- **Převzato `1513879`:** `draci_farma.glb` (zřícenina se schody na rozhlednu, louče), `les_paseka_srub(_interier).glb`
  (prkenné schody na posed, srub bez mezer), `objekty.json` kolegy beze změny (zřícenina zase `"prumysl"`),
  `draci_hrad_interier.glb` pryč (síň bez interiéru).
- **Dvě podoby jednoho průmyslu:** dva záznamy se stejným `prumysl` bez `varianta` → varianta 0 a 1, průmysl si
  vybere podle náhodného bitu 0 (jako socha kámen/bronz). Dračí farma je zřícenina nebo hrad. Rig `postav` staví tu
  podobu, kterou jmenuje.
- **Drak do síně bez dveří:** síň nemá `dvere`, drak vlétá podle toho, kam jeho místo míří (`otoceni` ven), 6 m před
  místem ve 3 m. **Drak do jeskyně** (`"kde": "jeskyne"`): přistane 12 m před místem (před ústím), dojde dovnitř
  animací `chuze` (8 s), ven stejně (`FarmDragon::walking`, „jde“ v `test3d draci`). Rig: obojí ověřeno.
- **Chůze na schodech a hradbách:** nad zemí (stoupá po podlaze) má chodec poloměr těla 0,2 m jako v interiéru
  (schody zříceniny 1 m mezi zábradlím, posed 0,8 m). Rig: posed 0 → 2,64 m. Na plošinu posedu nevejde: stříška je
  1,06–1,66 m nad podlahou plošiny (chodec potřebuje 2 m) – kolegovi. Pata schodů zříceniny je nedosažitelná
  (u průchodu leží kvádry) – kolegovi. Hrad dál 0 → 7 m.
- Rig: `chodec volno [dlazdic] [cm]` jemná mapa, `:` = střed volný, kruh ne.
- Náklady (sada `naklady`, skládání, `loze`) po buildu; blednutí viz 26. 10. (5).

## 26. 10. (3) – 1203 po letech: Škoda 1203 → TAZ 1203 → TAZ 1500 a TAZ 1.9D; roky z BeamNG; jeden GRF na auto

- Hráč: *„1203 škoda pak 1203 taz a nakonec taz 1500 a 1900D?“*, *„to udělej hezký když to píšeš extra pro 1203“*,
  *„jestli máš z beamng rok tak ho použij“* (u ostatních aut), *„to se má rozbalovat typ auta na dodávku, valník“*.
- **Éry 1203** (`S1203_MAKES`, jen složka `S1203`): Škoda 1203 od 1968 (35 kW), TAZ 1203 od 1981 (35 kW),
  TAZ 1500 od 1993 (1433 cm³, 40 kW – odhad, zdroj výkon neuvádí), TAZ 1.9D od 1996 (diesel VW 1,9, 47 kW).
  **Nic nekončí** (hráč: *„nikdy se nezastaví výroba. de luxe by bylo dostupný jen 4 roky. to ne.“*): každá
  konfigurace je auto každé éry, od roku éry nebo od svého prvního roku z modu (De Luxe od 1969), model life 0xFF.
  Rychlost podle třetí odmocniny výkonu. Jména česky: kombi, dodávka, valník, sanitka, pohřební, De Luxe, mikrobus,
  VB, VB žlutá (police2: nová maska, žlutý nátěr), Spoje.
- **Roky z BeamNG u všech aut**: uvedení od `Years.min` z `info.json` / `info_<konfigurace>.json` (prop 0x00),
  `max` se nebere; bez `Years` od 1920.
- **Varianty OpenTTD** (prop 0x26): v nákupu se rozbalí typ (Škoda 1203 kombi ▸ dodávka, valník, VB…, stejně TAZ 1203,
  TAZ 1500, TAZ 1.9D). Hráč má i tak „vozidla nikdy nezastarají“ (`never_expire_vehicles = true`). Rig 1997: 40 aut.
- **Jeden GRF na složku auta** (`S1203.grf`, GRF id jako dřív u výchozího auta): všechny konfigurace a éry v něm,
  čísla od 200 (výchozí konfigurace první, takže uložená výchozí 1203 je teď Škoda 1203 kombi). Hráč zapíná jeden
  GRF, ne deset. Výchozí auto čte i `info_<default_pc>.json` (kombi dřív dostalo všechny náklady).
- V releasech grrrrf (16), forclaude a cota 1203 z BeamNG není; v indexu jen tisknutelné modely (stl) a VWT1.
  Hráčovy 1203 v grrrrf: VWT1 všech 18 od 1. 1. 1951, osobáčky/pokusy TAZ 1203 plachta od 1. 4. 1972.

## 26. 10. (2) – BeamNG: varianty jako samostatná auta, náklady podle 1203, College Girl řídí 1203

- **Varianty:** každý `.pc` složky auta (kromě `default_pc` z `info.json`, bez něj kromě prvního `.pc`) je samostatné
  auto v nákupu, jméno + „Configuration“ z `info_<název>.json`, díly podle svého `.pc`, vlastní GRF
  (`<id>_<název>.grf`). Výchozí auto má GRF id jako dřív (savy drží).
- **Náklady** (hráč: *„dej jim normálně náklady valník, dodávka, předlohu máš dvanáctsetrojky. VB bude vozit
  1 pasažéra nebo 1 studentku“*): VB/policie/sanitka PASS a STUD po 1; osobní/lux/bus lidi; valník a pickup seznam
  naší 1203 s plachtou bez lidí; dodávky, COM, spoje, pohřební seznam 1203 s plachtou (pošta první). Ostatní auta
  BeamNG dál všechny náklady, ale **bez ROLA** (Rola jen vozy hráče).
- **College Girl za volantem jen v 1203** (hráč: *„colege girl ať řídí“*, pak *„v každým autě se sedí jinak, dej ji
  jen do 1203“*): sedí podle kamery řidiče z jbeam (`camerasInternal` „driver“). Ověřeno na kopii Volhy přejmenované
  na S1203 (Volha sama holku nemá); skutečnou 1203 v rigu nemám, sezení zkontrolovat ve hře.
- Rig: `test3d motory auta [text]` píše i náklad, kapacitu a přestavby.
- Hráč: *„rola nikdy nemohla být na žádném autě, to je vagon na přepravu aut. bude odtahovka jo? auto který bude
  vozit auta, potom někdy“* – odtahovka později. Kontrola: žádné z 91 aut hry Rolu nebere.
- **Odpověď kolegovi** `hra/3d/ODPOVED-DRACI-HRAD.md` (grrrrf `3b915b0`): prosby 0–4 hradu ano, jeskyně zříceniny
  podle hráče (výklenek, ne nová místnost; rozhledna se schody, louče, drak zvenku), posed schody jako `podlaha`,
  otázka na mezery mezi kládami srubu.

## 26. 10. (1) – chůze nahoru: hradby, střecha síně, most; auto přes most; draci do síně; kolegův hrad z modulů

- **Hráč: „1–5, pak build a po buildu GPU“** (1 let draka do síně, 2 kolegův hrad z modulů a zřícenina s rozhlednou
  a jeskyní, 3 chůze po schodech a hradbách, 4 noční světla, 5 odpověď kolegovi). Během toho: posed „bude chodit do
  schodů“ (žádný žebřík), „a na most“, „autem po železničním offroad“; autům BeamNG všechny náklady.
- **Převzato `78fd302` a `60ba2ab`:** hrad z modulů (věž a zeď 4×, otočené uzly – čtení glb bere u uzlů celé TRS),
  zřícenina s rozhlednou a jeskyní. Zřícenina v naší kopii dál bez `"prumysl"` (farma = hrad).
- **Chodec stojí ve výšce** (`FirstPersonEye::stand_z`, `FirstPersonStandZ()`): nejvyšší z toho, na čem se dá stát,
  nejvýš krok 0,45 m nad tím, kde stál – `podlaha` obalu průmyslu i interiéru (schody, ochozy, střecha síně),
  mostovka nad dlaždicí, jinak zem; na rampě mostu rampa jako pro auto (propad na rampě z testu 4 pryč). Jinam
  postavený (dál než 2 m od posledního místa) stojí na přízemí jako dřív (`FirstPersonGroundZ()`).
- **Překážky podle výšky** (`Blocks(x, y, nohy)`): nahoře půdorys toho, co stojí od kolen po hlavu nad nohama,
  po krocích 0,25 m (`footprints_up`) – zdi pod nohama nepřekážejí, cimbuří ano. Rig: chodec vyjde po schodech
  0 → 3,4 → 7 m, jde po zadním ochozu, zastaví ho rozhledna; mapa volna v 7 m ukazuje cimbuří síně a rozhlednu.
- `podlaha` se kreslí (schody jsou jen v ní) a čte se i u dílů lesa (schody posedu, až je kolega udělá).
- **Most:** chodec po rampě 0,4 → 2,3 m, po mostovce 3 m nad tratí, dolů. **Auto mimo silnici** najede na hlavu
  mostu po jeho ose, jede po mostovce (i přes železnici pod ní), bokem z ní nesjede, přívěsy nahoře s ním
  (`RoadVehFreeBridgeDeck()`); rig: autobus 8 → 16 → 16 → 8.
- **Draci farmy přilétají a odlétají** (12 s, `FARM_LAND_TICKS`): na nádvoří shora, na střechu síně do 7 m, do síně
  („kde": "sin") před vjezd, jím ve 3 m dovnitř a na slámu; zase ven stejnou cestou.
- **Noc v materiálech:** noční trojúhelníky každý ve své textuře (lucerna rozhledny a louč jeskyně zříceniny mají
  vlastní atlas, kreslily se z velkého).
- **BeamNG:** auta přestavitelná na všechny náklady hry; kola, která mod nechává na dílech BeamNG (1203:
  `S1203_wheel_F/R`, pneumatiky, poklice v žádném souboru), hra kreslí vlastní (pneu a disk) na náboj.
  Varianty 1203 (VB, valník, dodávky – `cotamje.md` z testu 4) jako samostatná auta: viz 26. 10. (2).
- Simtrains: nákladní vagony bez vození aut (Rola jen vozy hráče).

## 25. 10. (10) – sdílené textury přes uri, interiér dračího hradu a srubu hajného

- **Textury přes `uri`** (`274dfe9`, kolegova volba 1): obrázek glb může být soubor vedle .glb (cesta relativně ke
  glb, ve stejné složce nebo taru). Hra ho čte `GlbImageBeside()` na obou místech, kde čte obrázky.
- **Interiér a dveře průmyslu** (`921fa3d`) jako u domu: `interier` a uzly `dvere_NN` u průmyslu (síň dračího hradu).
  Zblízka (do `CAB_INTERIOR_TILES` + velikost) se kreslí obal bez krabice interiéru (`ThreeDObject::outside`,
  `CutOutInteriorBox()` – trojúhelníky celé uvnitř krabice interiéru ±5 cm pryč), interiér a křídla dveří; zdálky
  celý obal i se zavřenými dveřmi. Chůze, E u dveří a „uvnitř“ přes `GetThreeDObjectWithRoomsAt()`.
- **Srub hajného** (`les_paseka_srub`, díl lesa otočený po čtvrtotáčkách): interiér, křídlo dveří a obal bez
  interiéru se čtou pro každé otočení zvlášť a otáčejí se kolem středu dlaždice stejně jako díl
  (`TurnGlbMeshQuarters()`, `TurnNightQuarters()`). Rig: všechna 4 otočení (`test3d postav x y paseka <bity>` –
  rig dřív stavěl s náhodou 0, srub byl vždy na (1,1)); E otevře dveře, uvnitř postel, stůl, sud. Rig příkaz
  `chodec dum` umí i srub a hrad.
- Viděno v rigu: mezi kládami zadní stěny srubu zevnitř prosvítá světlo (asi mezery v modelu) – zeptat se kolegy.

## 25. 10. (9) – od kolegy: přebalené sady, strašidla v lese, světla pozpátku, dračí hrad

- **Hráč: „si to dodělej“** (všechno od kolegy, i sdílené textury přes `uri`).
- Převzato `35ac7da`: 418 glb přebalených gltfpackem `-cc` (sada hry 368 → 211 MB), nový srub hajného.
  `b59ea9c`: `strasidlo` a `prizrak` v sadě strasidla (místa v lese je jmenují ze sady strasidla). Výšku jejich počátku
  bere hra z `vznaseni_m` (dřív jen `vyska_m`).
- **Světla pozpátku (slíbeno 23. 10., kolega dodal ve Vagony v5):** uzly `svetlo_predni_vzad_NN`/`svetlo_zadni_vpred_NN`
  a skla materiálů `svetlo_predni_vzad`/`svetlo_zadni_vpred` hra dřív brala jako obyčejná přední/zadní (bílé sklo
  zadního čela svítilo i při jízdě vpřed). Teď zvlášť (`night_kind` 0–3, `front_back_lights`, `back_front_lights`):
  vůz otočený (jede zadkem napřed) svítí, vede-li, bíle zadním čelem (s kuželem za sebe), a je-li poslední, červeně
  předním. Platí pro každý vůz s těmi uzly, i bez motoru (Btax 780). Rig: 810 a Btax ze Vagony v7 – 50/48 skel pro
  jízdu pozpátku, 3+2 a 2+2 místa.
- **Dračí hrad** (`4c6e74d`) je vzhled dračí farmy místo zříceniny (hráč: „ten hrad bude hezčí“): v naší kopii
  `prumysl/objekty.json` zřícenina bez `"prumysl"`. Hra četla u průmyslu z glb jen první kořenový uzel – z hradu
  jen `zem`, u automatu chyběl plevel pod `krovi`. Teď průmysl čte všechny uzly kromě noci, `den`, sena (zvlášť)
  a křídel dveří (`all_roots`). Rig: hrad 4970 trojúhelníků, hradby, schody, louče v noci.
- Zbývá od kolegy: chůze po schodech a hradbách (`podlaha` v obalu průmyslu do 7 m), interiér a dveře průmyslu
  a lesa (síň, srub), drak na střeše síně (stojí už z `z` místa) a vlétání do síně, žebřík posedu (`zebriky`),
  sdílené textury přes `uri`.

## 25. 10. (8) – úvodní menu na telefonu: zem otočené mapy z paměti pohledu, deník s rozpadem

- **Hráč (měřil fps): „stmívání, rozednívání má 0,4 fps, noc 14 fps; stačí dvě auta, jeden přízrak a jsou 4 fps
  při pohledu shora“; na prázdných dlaždicích 120 fps (telefon přepnut na 120 Hz, hra strop 64 – nastavení
  obnovovací frekvence).** Jeho `snimky.log`: celé překreslení otočené mapy ~2 s; s auty ~10 bloků mapy za snímek,
  každý 11–86 ms, z toho 7–16 ms čekání na kartu, která kladla tutéž zem znovu (karta zem otáčí a hra ji čte zpět).
- **Oprava:** `TurnedGroundOfView` (viewport.cpp) – zem položená kartou (nebo procesorem) se drží v pixelech pohledu,
  dokud se pohled neotočí, nezazoomuje a zem se nezmění (`_turned_ground_version`: změny držené země, jiný obrázek
  dlaždice se základem). Blok kreslený znovu (auto projelo, přízrak, stmívání) si zem zkopíruje, karta nic.
  Posun pohledu: co oba mají, zůstane. Rig: obraz s pamětí i bez ní stejný pixel po pixelu (den i noc); překreslení
  celé obrazovky 10,5 → 3 ms; úvodní menu mezi otáčením 598 z 598 bloků ze zemi z paměti, 60 fps.
- **Deník (`snimky.log`):** nové sloupce `otocena` (sprity, stopy, položení země, sběr, plakáty, modely ms na snímek,
  kolik bloků vzalo zem z paměti; i modely při 0°) a `obrazky` (kolik obrázků objektů vzniklo, co dělala karta) –
  druhý log hráče: při 0° bloky s pohybem 30–90 ms a karta drží víc a víc (268 → 882 MB), podezření na pořád nové
  obrázky; další log to ukáže. Rig: `test3d cas <n> <š> <v>` překreslí jen kousek, `test3d zem pohled 0|1`.
- Okno z očí: obraz do okna bez tónu (zranění) kopírován po řádcích místo po pixelech přes `Colour` (na telefonu okno
  3,3× za snímek po 61 ms). Proč 3,3× za snímek (rig 1,2×), ještě nevím.
- Hráč: auta shora malá, ale kreslená s detaily a holkou za volantem – detaily stojí jen při výrobě obrázku; jestli se
  obrázky dělají pořád znovu, ukáže nový log, pak zjednodušení malých (posádka pryč pod pár px).

## 25. 10. (7) – hra čte glb zabalené gltfpackem -c (meshopt)

- **Hráč: „jo určitě gltfpack“ (menší 3D GRF a sada).** Hra čte `EXT_meshopt_compression` i
  `KHR_meshopt_compression` (gltfpack `-c`, `-cc`, `-ce khr`): dekodér meshoptimizer 1.3 (MIT, jen tři soubory
  dekodéru) v `src/3rdparty/meshoptimizer`. Zabalený bufferView se rozbalí jednou při prvním čtení
  (`GlbFile::ViewPiece()`), kostry (`ReadGlbAccessor()`) taky. Filtry OCTAHEDRAL/QUATERNION/EXPONENTIAL/COLOR.
  Rozbalení je rychlé (GB/s), čtení z disku menší. Dál se nečte: Draco, KTX2/Basis (`-tc`), WebP – hra bere jen PNG.
- Rig: celá sada (425 glb) zabalená `-c -kn -km -ke` i `-cc -ce khr`: farma s draky (kostry), město zhora i z očí
  stejné jako s původními (rozdíl jen text zprávy dole). Sada 379 → 258 MB; obrázky jsou 223 MB z 379, ty gltfpack
  bez `-tc` nezmenší (zmenšit textury musí kolega sám). 215 souborů ze 425 nemá ani kvantování.
- Kolegovi napsáno: přebalit všechno gltfpackem `-cc -kn -km -ke` (jména uzlů, materiály, extras zůstanou).

## 25. 10. (6) – duch s prostěradlem u kostela

- **Hráč: „kolem kostela zvýšený výskyt ghost prostěradlo, jeden ke každému kostelu; bude mizet, chodit do kostela
  a okolo kostela“.** Nové místo strašidel `Place::Church` (three_d_animals.cpp): ke každému kostelu v okolí středu
  (do `CHURCHES_KEPT` = 4) jeden `strasidlo_ulice` (kolegovo prostěradlo, v noci, mírumilovné). Kostel = dům
  s `BuildingFlag::IsChurch` (všech podnebí). Chodí kolem kostela (0,7 dlaždice od zdí), občas jde ke dveřím
  (dveře 3D kostela, jinak strana k silnici) a vejde – zmizí na 600–1200 tiků; občas zmizí, kde stojí; vyjde dveřmi
  nebo se zjeví jinde kolem kostela. Zmizelý se nekreslí a nedá se trefit; udeřený utíká jako ostatní. Ve dne
  není (jako ostatní noční strašidla), zbouraný kostel – duch zmizí. Rig: kostel 84,40 – chodí, zmizí, vyjde dveřmi.

## 25. 10. (5) – dračí farma bez hradu, škola a automat neviditelné: jedna chyba

- **Hráč (#300): „kde je dračí farma, já chci starou… byl to hrad zřícenina“, „škola a automat se postavily, byly
  tam, ale nebyly vidět“.** Hrad jsem nedal pryč – model od kolegy je pořád ten jediný (zřícenina s nádvořím, 3×3,
  shodný s grrrrf). Příčina: naše průmysly (`ReadIndustryObject`) se čtou hned při startu, ale bez cesty k souboru;
  od 23. 10., kdy dostaly společné stromy a keře jako doplňky, je hra při prvním kreslení „dočetla“ znovu
  (`Read()`) – a bez souboru přečetla jen doplňky a přepsala jimi celý tvar i texturu. Zbyly keře a stromy. Týkalo se
  všech našich průmyslů s doplňky: **dračí farma, gymnázium (škola), automat, hospoda, socha**; lihovar a chatka
  doplňky nemají, ty byly vidět. Oprava: průmysl si pamatuje svůj soubor (`glb_path`), při prvním kreslení se přečte
  celý i s doplňky jako domy. Ověřeno v rigu: hrad s nádvořím a draky, škola s vlajkou a automat stojí (stará verze:
  tráva a keře), les s pasekou beze změny, kouřová baterie = základna.
- Hráč s kolegou hrad předělají („ten hrad bude hezčí“) – hra vezme, co kolega pošle.

## 25. 10. (4) – #300: obraz bez čtení z paměti karty, Ctrl+šipka chůze

- **Z očí ~4 snímky/s, škubání i při pauze (hráč, test 4 #300: „u konce jsem zmáčkl pauzu a pohyboval hlavou, stejně
  to škubalo“, „ze začátku to běhá líp, pak to škube“):** z snimky.log: kreslení z očí na kartě ~40 ms, ale „okno
  oci“ (položení obrázku z očí do okna) 3× za snímek po ~67 ms = ~200 ms. Tam se jen nuluje díra pro obraz z karty
  a před tím čtou barvy animace palety (RealizeAnimation) – na pixel skoro nic. Příčina: obrazovka hry je paměť
  **namapovaná z karty** (persistent PBO, OpenGL ovladač z OpenTTD) a každé **čtení** z ní je na zinku/Vorteku (Mali)
  strašně pomalé. Sedí na to i noc na úvodní obrazovce („když začne stmívání, zpomalí se“): noc čte a ztmavuje každý
  pixel plochy (DarkenRect). Oprava (video/opengl.cpp): hra kreslí do **své vlastní paměti** (vid_pixels,
  anim_pixels) a kartě jen **zapíše** změněný obdélník (CopyScreenRect) – z paměti karty se nečte nikdy. Ověřeno
  v GL rigu (sdl-opengl, Xvfb, 40bpp-anim): mapa, noc, otočená plocha a pohled z očí stejné jako stará verze (rozdíl
  jen pár desítek pixelů animací). Na telefonu to potvrdí sloupec „okno oci“ v příštím snimky.log (má spadnout
  z ~67 ms na pár ms).
- **Ctrl+šipka nahoru/dolů = chůze bez držení (hráč: „dvakrát šipku nahoru stále nefunguje… zkusíme Ctrl+šipka“):**
  dvojí stisk se poznával podle držení klávesy při kroku chodce; při ~4 snímcích/s byly kroky tak daleko od sebe,
  že dva rychlé stisky hra neviděla. Ctrl+šipka se bere z vlastní události klávesy (OnKeyPress), na rychlosti
  snímků nezáleží. Šipka držená s Ctrl chůzi hned zase nezastaví; zastaví ji další stisk šipky jako dřív.
- **Rig:** `testzatik` u opravdového ovladače videa (GL rig) pouští příkaz v hlavním vlákně (QueueOnMainThread), ne
  ve vlákně tiků – jinak volal OpenGL bez kontextu a kreslil mimo zámek obrazovky (assert dst_ptr). Null rig beze
  změny. GL rig: `scena_gl3.sh` s příkazy v `AFTER_NEWGAME` (game_start.scr se v GUI nespustí včas).
- **Černá plocha po návratu z očí** (test 1) se podle hráče už neopakovala.
- **Zbývá z testu 4 (chyby):** propadání silnicí na nájezdu mostu, chůze po mostech, nohy čarodějnice zblízka
  neviditelné, 1203 z BeamNG není v nákupu, škola a automat postavené, ale neviditelné, paměť karty roste
  (110 → 800 MB). **K domluvě (přání):** posed v lese (žebřík), stará dračí farma – zřícenina hradu s jeskyní a
  rozhlednou (schody, louče), bouda v lese (sud a pivo bez Mata, postel, židle u stolku), auto přes železniční most.

## 25. 10. (3) – paseka (tráva, kapradí, klády), rožeň, záloha letu draků z #299

- **Paseka v lese (hráč: „dvě desky skrz sebe po celé pasece a jeden kmen je červený“, „dělá to ta tráva“):**
  příčina – budova (ThreeDObject) měla jen **jeden** obrázek: první materiál z glb. Model paseky má materiálů víc
  (tráva, kapradí, klády, zem) a každý se svým UV; všechny trojúhelníky ale četly obrázek prvního materiálu.
  Tráva a kapradí (dvě zkřížené desky) tak měly cizí obrázek a místo průhledných stébel byly vidět plné desky;
  kláda trefila UV pruhem červenou kytku v obrázku země. Oprava: každý materiál budovy má **vlastní obrázek**
  (číslo 0 vlastní první, 1..A doplňky, A+1.. další materiály; `material_pngs`/`material_flats`, dělají se líně
  při prvním kreslení přes `MaterialTexture`, sdílí se podle bajtů png jako ostatní). Ověřeno v rigu z očí
  i na otočené ploše, hráč: „jo to je hezkej les“.
- **Rožeň u stadionu (hráč: „tyčka končí moc brzo před vidličkou“):** příčina – postavy u domů (prase na rožni,
  holky, dělníci) se z očí kladly na **celé jednotky** světa (~0,93 m), rožeň tak ujel až o stopu vedle vidlic.
  `AddCabFigure` teď bere přesné souřadnice (double). Data: rožeň `poloha z` 0,95 → 1,01 a `meritko` 1,0 → 1,08
  (osa ve výřezu vidlice 0,99 m + poloměr tyče; tyč ±0,95 skrz vidlice ±0,98..1,02). Naše `objekty/mirne` se tu
  liší od kopie v grrrrf (kolegova má 0,95/1,0). Hráč pak „tak to je jedno ten gril“ – hotové je to stejně.
- **Záloha letu draků z #299 (hráč: „litali moc hezky… zálohuj to, kdybychom se k tomu chtěli vrátit… litali
  v kruhu a tím to bylo… tak by lital jenom jeden“):** #299 = commit **1290626**. Tam `Flyer(k)` bral náhradníka
  z **všech** letců indexu („let“), volal se jako `Flyer(k + id.base())` u farmy a `Flyer(a.vehicle)` u útoku na
  vozidlo – proto nad farmou kroužil i velký modrý a čínský. Od f224485 je to `FarmFlyer(obj, k)` jen z letců té
  farmy (mista_draku). Návrat: `git show 1290626:openttd/src/three_d_dragons.cpp` a vrátit ta dvě volání; kroužení
  samo (dráha) se nezměnilo. Co je hezčí, řekne #300.
- Vedlejší: šedé koruny stromů v prvním snímku karty po načtení – stejné i bez téhle změny (zástupný obrázek,
  než se dodělá textura stromu), ne regrese.
- Kontroly: build, clang narrowing 0, MinGW 0, kouřová baterie zakl spojeno=1, mezera/depoodpoj 0 = základna.

## 25. 10. (2) – zem bez čekání, čarodějnice, dračí farma bez „Falca“, logy do save, Esc z očí

- **Zem otočené plochy bez čekání (#190, k buildu #300):** pokládání země bloku kartou (warp drženého obrázku
  + čtení zpět) drželo hlavní vlákno ~100+ ms na překreslení. Teď `LayTurnedGround` zemi kartě jen **zadá**
  (`ThreeDGpuDrawGroundStart`), hlavní vlákno mezitím sbírá a třídí objekty bloku (vozidla, chodce, čarodějnice,
  řazení), a `FinishTurnedGround` si výsledek **vyzvedne** těsně před skládáním objektů do bloku – čekání na kartu
  se schová za práci procesoru. Pixely jsou bitově stejné (ověřeno: 2 běhy, liší se jen text lišty). Bez karty
  pokládá procesor hned jako dřív. Rig: `polozila karta 987, procesor 0`, žádný pád.
- **Deník snímků: rozpad „okna“ (#165, #195):** z hráčova logu #299 bylo v očích ~220 ms/snímek v „okna“, ale oči
  samotné jen zlomek – zloděj neznámý. Nové sloupce: `hodiny` (hodiny oken, i krok chodce se sběrem), `bloky`
  (kreslení špinavých kusů), `mapa` (bloky plochy), `okno oci` (vykreslení plátna z očí), `zem` (čekání na kartu
  u země). Příští snimky.log řekne, kdo těch ~200 ms bere.
- **Logy na jedno místo:** snimky.log, 3d_karta.log (+minule), 3d_domy.log, texty.log, jizda.log nově do složky
  **save** vedle uložených her a souborů pádu (`FioDiaryPath`; hráč: „ja nepolezu do windows blazince“).
  log.txt (záznam anomálií) a crash* tam už byly.
- **Čarodějnice (hráč):** zatáčka rychle 3°/tik a pak **rovně** k cíli (dřív 0,8°/tik – dlouhý oblouk sypal
  obrázek za obrázkem natočení; „zatáčet musí, ale ne furt jinam“). Z očí **4× větší** (EYE_OF_MAP 0,5 → 2;
  „vidím jen světýlko… ať jsou větší v dálce jako draci“ – draci jsou v dálce velcí, protože velcí jsou).
  První úsek letu vede **nad chodcem**, když je venku u jejího města („ať proletí tak blízko, abych je viděl“).
  Světla i tělo pro zásahy rostou s ní. Rig: facing 0→-50 za pár tiků, pak drží celé rameno.
- **Dračí farma bez „Falca“ (hráč: „vubec by neměl byt falco na dračí farmě, to je pes“):** náhradník za
  nelétajícího draka (drak_kraci) se bral ze **všech** letců indexu – nad farmou tak kroužil obří ledový (modrý)
  a čínský had (ten „Falco“; pravý Falkor je pes v objekty/psi a na farmu nepatří). Kolega přitom psal „čínský
  a ledový jsou na farmu velcí“. Teď náhradník jen z letců **té farmy** (mista_draku: hnědý/černý); let nad
  rozbitým vozidlem taky. Velikosti nechány („draci z first person i zezhora dobrý“).
- **Z očí:** Esc zavře pohled (kurzor je na telefonu líný, ven to chtělo myš); šipky: první fáze otáčení
  ~10°/s po 0,35 s, pak rozjezd do plné 115°/s za 1,1 s („šipkama o velký kus, nezatočim kam chci“).
- Kontroly: build, clang narrowing 0, MinGW 0, determinismus snímku (jen text lišty), kouřová baterie
  zakl/mezera/depoodpoj = stabilní základna.
- **Hotovo (1c6e9cd, nižší model):** v možnostech hry u poruch „draci způsobují poruchy“ ano/ne (výchozí ano)
  a „dosah draků z farmy“ (výchozí 12 polí, až 96 po 4). **Zbývá:** spomalení při letu čarodějnice – hráč si
  zobrazí počet snímků, rozpad „okna“ v deníku ukáže.

## 25. 10. – přechod na GPU: dávky domů bez čekání, při otáčení poslední obrázek domu

- Hráč: *„telefon je ok, detekuje mali, jede zink virgl. dodělej ten přechod na gpu, domy a co ještě není
  hotové.“* Nalezený nedodělek: obrázky budov otočeného pohledu sice kreslila karta v dávce (#134), ale hlavní
  vlákno na ni **čekalo** – render + čtení zpět (barvy a float hloubky) při každém pootočení, na zinku je čtení
  zpět to pomalé. **Teď:** dávka jde na vlastní vlákno (`ottd:3d-mapa`), hlavní vlákno nečeká; hotová dávka se
  vyzvedne každý snímek (UpdateWindows) a překreslí obrazovku. Než dorazí, dům se položí ze svého **posledního
  obrázku ukotveného za severní roh** (posun + přiblížení hloubky) – při otočení o stupeň skoro k nerozeznání
  (měřeno: 1,7 % pixelů jinak), přesný obrázek ho o snímek později vystřídá. Bez obrázku (první spatření) se
  kreslí po trojúhelnících jako dřív; bez karty se nic nemění.
- Rig: `test3d snimek` kreslí „obrázek okamžiku“ (synchronně) – snímky jsou dál deterministické (2 běhy stejné
  md5). V rigu s kartou: první kreslení nového úhlu modely ~50 ms (přímé), po dovezení dávky ~5 ms (kopie
  obrázků), 343 obrázků v paměti. GL rig (llvmpipe): snímky úhlů 30/31 správné, bez pádu. Baterie: zakl,
  mezera, depoodpoj beze změn. Zahazování textur (DropPaintedTextures, ForgetUnused) počká, dokud je dávka na
  cestě – stejná pojistka jako u vlákna očí.
- Pozn. ke GL rigu: konzole se tam neloguje a hra se dnes nahrávala přes 300 s – pády „XIO“ v logu byly jen
  timeout skriptu, ne chyba hry; `scena_gl2.sh` má limit 560 s a snímky přes `test3d obrazovka`.
- **Co v přechodu na GPU ještě zbývá:** zem otočené plochy se pořád pokládá s čekáním (karta → čtení zpět na
  hlavním vlákně, po blocích); vozidla na mapě obrázky po 45° (#127); domy úplně bez obrázků = skládat celý
  otočený pohled na kartě bez čtení zpět, jako snímek z očí (#123, velký krok – vyžaduje i plakáty vanilla
  spritů na kartě); u očí sběr scény a příprava trojúhelníků na procesoru (#150).

## 25. 10. – vypínač stínů, plynulost otáčení z očí, úvodní otáčení nebrzdí myš; co zbývá na GPU

- Hráč: *„světla chci ale stíny nejsou důležité… dej vypínač a nadále zlepšuj“*. **Teď:** nastavení Grafika →
  **Stíny v pohledu z očí** (`gui.eye_shadows`), výchozí vypnuto. Vypnuté stíny = žádné přepočítávání mapy stínů
  při chůzi městem (dřív 105–207 ms každých pár metrů → trhání). Noční světla (lampy, okna, vozidla, pochodeň)
  jsou samostatná a zůstávají. Rig `test3d stiny <0|1>` přepíná tohle nastavení.
- Otáčení šipkou z očí *„po velkých skocích“*: otočka má rozjezd – ťuknutí otočí o stupeň a kus (jemné míření),
  držená šipka se do 0,8 s rozjede na plných 115°/s. Na pomalém stroji tak první viditelný skok není 29°.
- Pravé tlačítko *„jemné ale gumovější“*: obraz z očí se kreslí na vlákně a je vidět o snímek později, takže při
  plynulém otáčení hlavy byl vždycky tam, kde ruka už nebyla. Teď se obrázek dělá s pohledem předpovězeným o
  většinu otočky minulého snímku – plynulý tah zpoždění dožene; prudké zastavení o snímek přestřelí a srovná se;
  skok pohledu (dvojklik zpět, přemístění chodce) se nepředpovídá. Rig kreslí hned a nepředpovídá nic.
- Úvodní obrazovka: otáčení plochy stojí, dokud se hýbe myš (po 1,5 s klidu jede dál) – kurzor už není otáčením
  zpomalený a New Game / Load Game jde trefit (hráč: *„myš kurzor je tak spomalená že se nedá trefit čudlík“*).
- **Co zbývá v přechodu na GPU** (hráč: „ať se hra hejbe pěkně“; proto je otáčení plochy pořád pomalé):
  - Otočený pohled (mapa): na kartě je jen terén (GPU bod 3). Domy, průmysly a vozidla jsou obrázky vyrobené
    procesorem a při každém novém úhlu se vyrábějí znovu – otáčení plochy proto stojí stovky ms na snímek.
    Zbývá: objekty otočeného pohledu kreslit kartou přímo (úkol #134), vozidla na mapě objektem (#127),
    domy bez obrázků (#123).
  - Pohled z očí: snímek už kreslí a ukazuje karta (#151, #152), ale sběr scény a příprava trojúhelníků zůstává
    na procesoru (#150 rozpracované); mapa stínů taky (teď výchozí vypnutá).
  - Telefon: podle snimky.log trvá snímek z očí ~260 ms, z toho kreslení z očí jen ~45 ms – zbylých ~200 ms sedí
    v „okna“ na hlavním vlákně a není vysvětlených (#165). To je teď největší brzda na telefonu; sám přechod
    na GPU ji nevyřeší, příčinu je potřeba najít.

## 24. 10. večer – pád při výjezdu z depa (MSTS jednotka, vynucená jízda na červenou, build 298)

- Hráč: *„nutil jsem ho vyjet z depa na červenou, model z MSTS, špatně poskládaný lokomotivy asi jsem dal
  obráceně“*, *„narazila do zdi, bylo vidět že vyjela do zdi depa“*, *„do vrat ven z depa měla červenou“*. Záznam:
  `krok ROZBITY … couva ano, clanek 0 z 6`, mašina venku, pět dílů dál schovaných v depu, assert
  `IsValidDiagDirection(exitdir)`.
- **Příčina:** v depu je natočení vozů jen druhá polovina toho, který konec vede (vede hlava → čelem k vratům,
  vede zadek → čelem ke zdi). Mašina odtažená z čela **otočeného** vlaku zůstala čelem ke zdi, ale už ne
  otočená; zbytek vlaku dostal novou hlavu, která otočená nikdy nebyla. Rozjetá jela do zdi, u zdi se otočila
  s prvním dílem už venku a zbytkem schovaným → vyjela sama, ostatní zůstali v depu, za tři dlaždice pád.
- **Teď:** `FaceTrainOutOfDepot()` natočí vozy celého vlaku v depu podle toho, který konec vede – po vzetí
  vozu z otočeného vlaku (oba vlaky, odtažený i zbytek) a ještě jednou těsně před výjezdem (to spraví i vlak
  uložený v savu a cokoli jiného; zapíše `mirilo … vozidel jinam` do záznamu). Zbytek otočeného vlaku zůstává
  otočený, ať je odtažená jednotka jakákoli (dřív jen u překlopitelných).
- Rig: `testpresun <vlak> novy [pozice]` (jednotka do vlastní řady). Scéna `depoodpoj`: dvě čtyřdílné mašiny,
  otočit, přední odtáhnout, vynutit výjezd – dřív přesně hráčův záznam a assert, teď obě poloviny vyjedou celé.
  Pozor na slepou uličku: natáčet vozy po každém spojování / přetažení v depu rozbije autoreplace – ten přesouvá
  vozy stejným příkazem a „couvá“ dá nové mašině (natočené jako stará) až po posledním přesunu. Ochrana u vrat to
  hned ohlásila ve třech scénách (`mezera`, `odtahotoc`, `odtahvyhybka`). Teď jen po vzetí z otočeného vlaku;
  baterie starý build proti novému beze změny.

## 24. 10. večer – les s pasekou místo normálního lesa, houbový guláš v hospodě, pád stínů z očí

- Hráč: *„to má nahradit normální les“*, *„v minulém buildu už normální les nebyl vůbec“*, *„nemáme staré savy“*.
  **Teď:** s rozšířeným průmyslem se normální (vanilla) les nestaví ani negeneruje, na jeho místě je náš les
  4 × 4 se jménem **„Les“** (stejná místa, četnost, klimata; jeho sprity hra dál načítá pro 2D). Bez rozšířeného
  průmyslu zůstává vanilla les. Rig: nová mapa s rozšířeným průmyslem má jen `les s pasekou`.
- Hráč: *„houby bude přijímat hospoda a bude prodávat houbový guláš do inventáře“*. **Teď:** hospoda bere náklad
  houby (MUSH) vedle alkoholu; 30 dní po dovozu hub prodává **houbový guláš** (věc inventáře, cena 6, léčí 30 – víc
  než pivo). Rig: bez hub `koupit 26` chyba, po `dovoz … houby` ok, `pouzit 26` zdraví 50 → 80.
- **Pád vlákna z očí** (rig, u hospody): mapa stínů z objektů, které ještě nemají načtený tvar (žádné
  trojúhelníky), počítala velikost z rozsahu ničeho → přetečení → výjimka „vector larger than max_size“ a konec hry.
  Mohlo se stát i na telefonu hned po otevření pohledu z očí mezi nenačtenými domy. Teď je mapa jeden texel bez stínu.

## 24. 10. večer – les s pasekou od kolegy (náš průmysl 4 × 4), náklad houby, zelený mimozemšťan

- Hráč: *„máme les?“* – kolega poslal `LES-PRO-HRU.md` (díly `les_zem`, `les_cesta`, `les_paseka`,
  `les_paseka_srub`; zajíc, veverka, ježek) a zeleného mimozemšťana. Převzato (Putin dál ne – hráč: *„putina
  nedávej“*).
- **Průmysl „Les s pasekou“** (`IT_GAME_FOREST`, s rozšířeným průmyslem): 4 × 4 jako vanilla les (kde smí stát,
  jak často, klimata mírné a subarktické), dřevo pořád; bere studentky a 30 dní po dovozu dává i **houby (MUSH)**.
  (Večer hráč: nahrazuje vanilla les, jméno „Les“ – viz výš.)
- **Náklad houby (MUSH)** nový: jako ovoce, rychle se kazí, třída express; z vozidel hry je berou poštovní auta a
  vagony (přestavba), kolegovy osobáky ho jmenují.
- **Skládání:** paseka na prostředních 4 dlaždicích (každá severním rohem ven, na jedné srub hajného), cesta od
  strany, kde je nejvíc silnice (bez silnice náhodná strana), jinde lesní zem různě natočená. Díl = celá scéna glb
  (`ReadGlbScene()`, jako interiér) otočená o čtvrtotáčky, stromy a keře vložené jako doplňky. Stromy stojí hned.
- **Holky** z dílů po dovozu studentek (mapa, z očí, náhled u okna). **Houby** na místech `houby` pořád, každá
  zvlášť utrhnutelná. **Zvířata** na místech `zvirata` z jejich `druhy` (laň, jelen, liška, vlče, zajíc,
  veverka, ježek), až 5 kolem chodce. **V noci strašidla** na místech `noc_postavy` – zatím jen kobold;
  `strasidlo` a `prizrak` jsou figury čarodějnic, ne strašidla (prosba kolegovi). Zajíc a veverka skáčou (`skok`
  bere hra jako chůzi). Chodec projde kapradím a trávou, srub, posed, klády a ohniště blokují.
- Rig: `test3d postav <x> <y> paseka` (rovnou dostavěný), `test3d dovoz … stud`, výpis `makes WOOD … MUSH …`.
  Ověřeno: dřevo bez holek, houby jen s holkami (0 → 15), houba utržena, zvířata na místech. Snímek z očí v rigu
  nejde (softwarová grafika je pomalá), mapa ve 3D ano.

## 24. 10. večer – houby a zvířata v lese, Billy po kolejích

- Hráč: *„stromy, houby a zvířátka mají být v lese furt, holky přibydou, když je přivezou, a pak les produkuje
  houby; ty houby tam rostou celou dobu, holky je jen sbírají“*.
- **Houby v lese:** na každé dlaždici lesa (`ThreeDForestTile()`, dlaždice průmyslu jen se stromy) roste houba
  pořád, jakýkoli druh (jinde dál jen kolem měst svého tématu); E ji utrhne, za měsíc doroste. Rig: v lese
  `zvirata utrhni` → hřib utržen.
- **Zvířata v lese:** nové místo `les` – až 3 zvířata (jelen, laň, wapiti, los, medvěd… podle klimatu) na
  dlaždicích lesa kolem chodce, i daleko od měst. Navigace bere les jako průchozí (dřív byl průmysl zeď a zvířata
  se odsunula ven).
- **Holky a náklad MUSH** zatím ne: vanilla les zůstává vanilla (vozí dřevo); holky s houbami přijdou s novým
  lesem od kolegy jako náš průmysl (#178).
- Hráč: *„Billy na tříkolce má jet po kolejích jako mašinka, nemá jet za mnou útočit, má projet okolo“*;
  *„ostatní strašidla se vyskytují kolem kolejí častěji, abych je potkal, straší normálně, jenom Billy jezdí po
  kolejích“*. **Teď:** Billy jezdí po trati dlaždici po dlaždici jako vlak (`PickRailRide()`: z hrany na hranu,
  zatáčky, výhybky – rovně, kde to jde; na konci trati, u tunelu a mostu se otočí), bez zastávek, nikoho nehoní a
  po zásahu jede dál; v ulicích už není. Ostatní strašidla u kolejí (dál až 3 navíc) se procházejí a útočí jako
  jinde. Billyho poznám zatím podle jména (`billy_tricykl`) – v indexu nemá nic, co by říkalo „jezdí po kolejích“.

## 24. 10. večer – dvě božská oka, les bez stromů

- Hráč: *„máme navíc jednu ikonku božího oka, v levém horním rohu; zavřel jsem křížkem first person, ona se
  neodmáčkla a podruhé zmáčknout oko nešlo, ta druhá fungovala; tu zamáčkávací vlevo nahoře dáme pryč“*.
- **Příčina:** oko (okno lišty číslo 1) se při nové mapě nezavíralo – `HideVitalWindows()` zavřela jen lištu a
  stavový řádek, `ShowVitalWindows()` pak udělala druhé oko (vlevo nahoře, na místo se posouvá jen jedno). Klik
  na tile zrušil zaměřovač u toho druhého, takže kliknuté oko zůstalo zmáčknuté. Teď se oko zavírá s lištou;
  rig: 4 nové mapy po sobě, vždy `oci 1`. Oko už netočí pohled na sever (to je jen pro stavění, oko je na koukání).
- Hráč: *„v lese nerostou stromy, jsou tam dlaždice pole bez kytek; stromy můžou vyrůst hned, ne jako na poli,
  kde brambory rostou až se studentkami“*. **Příčina:** stromy lesa (kolegovy `doplnky` na `pv_gfx_016`) jsou
  objekt bez vlastního souboru; hra ho brala jako „načtený“ (`IsRead()`) a stromy do něj nikdy nevložila. Teď se
  doplňky načtou. Stromy lesa (dlaždice jen se stromy, bez `dily`) stojí hned, i když se les ještě staví
  (`ThreeDOriginalIndustryTileStands()`): mapa, z očí, náhled u okna, chůze mezi kmeny.
- Rig: `test3d postav <x> <y> les` postaví vanilla les (nechá ho stavět jako hra).

## 24. 10. – trainset.simtrains.eu chce heslo

- Hráč: *„copak je s adresou https://trainset.simtrains.eu/ chce heslo, před 14 dny to bylo bez hesla, koukni do
  diskusí co se děje“*. Zjištěno: **celá doména simtrains.eu** (trainset, fórum railsimulator, msts, forum, hlavní
  stránka) vrací HTTP 401 s heslem serveru (Basic, realm „DOOMikovo“, za Cloudflare) – zavřel to provozovatel pro
  všechny, nejde o účet hráče. Archive.org má poslední otevřený snímek 10. 8. 2026. V diskusích nic: forum.msts.cz
  (Dan J. tam píše) o tom vlákno nemá, vlastní fórum simtrains je za stejným heslem. Hry se to netýká, vlaky
  simtrains máme v `newgrf/simtrains`.

## 24. 10. – božské oko: druhá řada ikon pod lištou

- Hráč: *„udělame druhou řadu ikonek pod hlavní lištu ikon, pod otazníček dáme ikonku božského oka a přesuneme tam
  first person view a crosshair bude místo kurzoru, kam hráč klikne crosshairem tam se objeví ve first person view,
  pravej čudlík myši akci zruší“*, *„noc zůstane v menu pod otazníčkem, jenom first person view odtamtud dáme pod
  ikonku“*. Obrázky dal hráč: oko v trojúhelníku 48×48, žlutý zaměřovač 32×32.
- Hotovo: okno druhé řady (`GodEyeWindow`, třída hlavní lišty, číslo 1 – je nahoře jako lišta, „zavřít vše“ ho
  nezavře) přesně pod otazníkem a stejně velké; jde s lištou při přesunu i změně velikosti rozhraní
  (`PositionGodEye()`). Klik na oko: pohled z první osoby přes mapu se zavře, kurzor je žlutý zaměřovač, tlačítko
  stisknuté; klik na mapu = pohled z první osoby s chodcem uprostřed té dlaždice (nebo nejbližší místo, kde stát
  může); **pravé tlačítko zaměřovač zruší** (jen ten náš, režimy myši hry beze změny), Esc taky.
- Pohled z první osoby je pryč z nabídky pod otazníkem, noc tam zůstala; v nabídce mapy zůstal.
- Grafika (ikonky z icons8.com, hráč: *„icons8.com jsou ikonky“*; v CREDITS): oko 20×20 a 40×40 (zmenšené z 48×48) a kurzor 32×32 zavěšený za střed, 32bpp, jako sprity
  0x0118 a 0x0119 v `openttd.grf` (yagl, skript `openttdgui_oko.py`, `GUI_ICONS_32` v `openttd_yagl.py`).
  Červený zaměřovač bomb zůstal.
- Rig: `test3d oko` (kde je oko a otazník), `oko klik`, `oko prave`, `oko <x> <y>`.
- Hráč: *„ta paměť 6 giga není pro objekty? tak to napiš do nápovědy že paměť pro sprity už není tolik potřeba
  protože zin16 je zastaralá technologie nahrazená 3D objekty“* → nápověda nastavení „Paměť pro sprity“ (cs i en):
  jen sprity základní sady a NewGRF, ne 3D objekty; sprity 16× (zin16) jsou zastaralé, nahradily je 3D objekty;
  výchozích 2048 MB stačí.

## 24. 10. – pád při přiblížení na draka: obří obrázky a tisíce póz

- Hráč: *„úhel 0°, jen jsem koukal na draka nad mořem a ve chvíli kdy jsem přiblížil zoom ještě víc tak to
  spadlo“*, *„pokaždý spadne i hudba z aplikace pod androidem a firefox byl zavřený tak ten pád je hluboko do
  systému“* → Android zabíjí aplikace, došla paměť celého telefonu. Log (stále build 296): 35 min hry, konec zase
  při čtení obrázků z karty (`cteni obrazku 116990-117005`), mapa se po pohledu z očí zpomalila na 6–10 snímků/s.
- Příčina (rig přes zink, 16× zoom zapnutý jako u hráče, `zoom_min = 0`):
  - **Obrázek tvora větší než obrazovka**: čínský drak 24,75 m × 3 na mapě × 48,8 px/m × 2 (vyhlazení) = rámec
    **5608×4188**, přes 90 MB čtených z karty naráz, 47 MB držených v paměti.
  - **Tisíce póz**: draci s kostrou mají na mapě pózu každý tik (hnědý klid 156, let 36, ledový 120), k tomu
    přechody mezi animacemi a natočení po 10° → nový obrázek skoro každý tik.
  - **Zásobník obrázků jen podle počtu** (1024), ne podle velikosti → u přiblíženého draka gigabajty.
  - Trojúhelníky problém nejsou: na snímek hnědý/černý 1262, ledový 2474, kráčející 4034, čínský 5000 (hráč chtěl
    draky optimalizovat přes grrrrf – není co, kolegu jsem neotravoval).
- Hráč: *„proč kreslí tisíc obrázků? animace letu stačí 4 obrázky“* → hotovo:
  - Na mapě má každá animace tvorů (draci, draci farmy, zvířata a strašidla, čarodějnice, UFO) **nejvýš 4 pózy
    za kolo** a **bez přechodů** (`ThreeDMapPose()`); pohled z očí zůstává plynulý. Rig, 2 draci 900 tiků na 4×:
    dřív 325 nových obrázků (23 MB, fronta 129), teď 62 (1,9 MB) a pak už žádné.
  - Obrázek postavy na mapu nejvýš **2048 bodů** napříč; větší se udělá o zoom hrubší a na mapě se zdvojí
    (`FigurePictureZoom()`, jako vanilla zdvojuje sprity z 8× na 16×). Rig: největší rámec 2048×1864 místo
    5608×4188.
  - Zásobník obrázků i podle velikosti: **128 MB** (`PICTURE_BYTES_KEPT`), při přeplnění zbude polovina; obrázky
    právě kresleného bloku mapy se nikdy nemažou. Při běžném zoomu je 312 obrázků ~3 MB, strop nic neomezuje.
- Hráč: *„paměť si už jednou omezil a pak jsme paměť vraceli“* – tenkrát se snížila paměť karty kvůli starému
  Winlatoru; teď se karta (1 GB) ani sprity nemění, jde o obří obrázky a nekonečné pózy.
- Pozn.: hráčovo nastavení `sprite_cache_size_mb = 6016` (cfg z 19. 10.) a zapnutý 16× zoom – na telefonu může
  cache spritů sama narůst do gigabajtů; nechávám na hráči.

## 24. 10. – pád na telefonu při stavění trati (build 296)

- Hráč poslal logy: *„neotáčel jsem plochou, nebyl jsem ve first person view, furt 0° a stavění tratě“*, karta
  zink (virgl). Crash log hry nezůstal → zmizel celý Winlator (ovladač, nebo Android kvůli paměti). Hráč: *„ten pád
  ale nemusí být vina karty že jo“* – nemusí, deník karty ukazuje jen, co karta dělala ve chvíli konce.
- Co je v logu: karta skončila uprostřed čtení dávky 9 obrázků zpět (`cteni obrazku 27305-27313 zpet
  (glReadPixels)`). Poslední 5 s běžela hra 5× rychleji (935 tiků), jinak ~45 snímků/s bez záseků. Dva tiky po 31 s
  na začátku = načítání hry.
- V rigu přes **zink nad lavapipe** (stejná vrstva jako telefon, `LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=zink`,
  i s kontrolní vrstvou Vulkanu) žádná chyba karty. Ale dávka obrázků se skládala do jednoho rámce omezeného jen
  výškou 16384: při přiblížení u města **2048×10378** najednou (rámec ~190 MB na kartě, 85 MB čteno naráz, hra přes
  1,4 GB). Na telefonu karta sdílí paměť s hrou – nejpravděpodobnější příčina, jistá ne.
- Opraveno: rámec obrázků i budov plochy nejvýš 2048 řádků (`FRAME_ROWS`), víc rámců za sebou; vyšší obrázek sám
  má vlastní. Rámec budov se po velkém zase zmenší. Rig: nejvýš ~2460×2250, špička hry 1,19 GB.
- Diagnostika: `snimky.log` má na každém řádku `pamet` (soukromá paměť hry) a `karta` (co karta drží); krok čtení
  obrázků i budov v `3d_karta.log` píše velikost rámce a paměť hry a karty – další pád ukáže, jestli to byla paměť.
  Rig: `test3d casy` píše největší čtený rámec.

## 24. 10. – UFO: katastrofy a noční talíř (kolega PROSBA-UFO)

- Hráč kolegovi: *„ufo talíř pro katastrofy 3D a v noci může létat jako strašidla a neničit koleje, ať svítí paprsek
  světla dolů, ufona udělej zelenýho“*. Mně: *„1 a 2 ano“* (katastrofy talířem, noční talíř nad městy).
- **Sada `ufo`** (`ufo_talir.glb`, `druh: ufo`): let ze snímků `let_00–07` bez paprsku a ovce (snímky je vynechávají
  přes seznam uzlů), paprsek a ovce jako samostatné postavy z jednoho uzlu.
- **Katastrofy:** malé a velké UFO původní hry se v otočeném pohledu, na 0° s 3D domy i z očí kreslí talířem (malé
  `meritko` 1, velké 2; na mapě 2× větší, malé ≈ dlaždice jako původní sprite). Sprite i stín se nekreslí. Co katastrofa
  dělá, je beze změny. Při sestupu (malé na auto, velké na koleje) svítí paprsek.
- **Noc:** jeden talíř létá od města k městu (přiletí chvíli po setmění, ráno zmizí), nic neničí. Nad městem buď visí
  s paprskem (někdy zvedá ovci), nebo přistane u města na volné rovině a kolem chodí ufon (`posadka`). Světlo lemu
  a paprsku se skvrnou na zemi v noci (jako lucerna a svítilna čarodějnic).
- **Paprsek** je kužel z modelu natažený vždy přesně k zemi (zvětšený celý, kužel má pak stejný úhel).
- **Opravené chyby po cestě:** materiál `BLEND` bral průhlednost jen z čísla materiálu a textury se při čtení
  ořezávaly na „pixel je / není“ → paprsek byl nahoře bílý neprůhledný a dole nic. Teď má BLEND průhlednost číslo ×
  textura (obrázky na mapě procesorem i kartou, z očí procesorem i kartou); ostatní textury (listí s maskou) beze změny.
  Úroveň zmenšení textury se teď počítá pro každou texturu modelu zvlášť (dřív podle první).
- **Strašidla:** převzat zelený `ufon` (kolem měst v noci). `klima` může být seznam (bity). **Putin** (par16) vynechán:
  forclaude je veřejné repo a fotka je cizí (kolega: jen soukromá sada) – rozhodne hráč. **Mexický mimozemšťan**
  (`mimozemstan.glb`, do ulic v noci): hráč chce zeleného, kolega ho přebarví (jeho model), převezmu pak.
- **Draci:** převzat let hnědého a černého; na farmě teď létá drak svého místa, když má `let`.
- Rig: `test3d ufo [x y stupně [visi|ovce|pristan]]`, `test3d ufo katastrofa male|velke x y`.

## 24. 10. – dračí farma jako průmysl hry

- Hráč: *„dáme interiér, dračí farmu“* (domluveno 23. 10.: FICR + STUD, draci létají a dělají poruchy, se studentkou
  přistanou). Model `draci_farma` od kolegy (hrad 3×3, `mista_draku`, `seno` s uzly `seno_00–07`).
- **Ve hře:** průmysl ve městě (jeden na město, 3×3, střed hrad), bere konopné vlákno (FICR, jako seno) a studentky.
  Nic nevyrábí. Jméno „Dračí farma“ / „Dragon Farm“.
- **Seno:** po dovozu FICR 8 kup, každé 4 dny o kupu míň (po měsíci prázdno). Kupa je uzel modelu sám (čte se jako
  křídlo dveří). Na mapě se kupy kreslí spolu s hradem do jedné hloubky – dřív je obrázek hradu přemaloval; stejně
  teď i křídla dveří kina.
- **Bez studentek (30 dní od posledních):** draci se střídají – část sedí na svých místech, ostatní krouží nad farmou
  (`let`). Jednou za ~6 dní porouchají vlak, auto nebo loď do 12 dlaždic od hradu (běžná porucha OpenTTD) a nad
  vozidlem krouží útočící drak. Rig: 4 autobusy u farmy, 5 poruch za 47 dní.
- **Se studentkami:** všichni draci sedí na `mista_draku` (klid), holky u nich, žádné poruchy.
- Hnědý a černý drak nemají animaci `let`, létají proto ledový a čínský.

## 24. 10. – interiér kina (první interiér ve hře)

- Hráč: *„dáme interiér, dračí farmu“*. Kolega poslal kino podle `PROSBA-INTERIERY.md` (`KINO-INTERIER-PRO-HRU.md`):
  `dum_27_kino_interier.glb` (vestibul s pokladnou, sál s 55 sedadly, plátno, opony, vnitřní dveře, lampy) a obal
  kina s průchody a křídly vchodových dveří `dvere_00–02`.
- **Jak to hra dělá:** interiér je 3D objekt ve stejném rámci jako dům (`interier`: true / soubor / {soubor, postavy};
  bere se jen, když soubor existuje – `true` mají v indexu skoro všechny domy). Čte se celý kromě `noc` (lampy
  v noci jako u domů) a `dvere_NN`. Každé křídlo dveří (z obalu i z interiéru) jsou tři objekty: zavřené, otevřené na
  jednu a na druhou stranu (otočené kolem počátku uzlu = pantu).
- **Z očí:** do 3 dlaždic od domu se kreslí i interiér (vidět dveřmi), křídla dveří jak stojí. Uvnitř (v obrysu
  interiéru) se obal domu nekreslí – okny je vidět ulice. Postavy z indexu interiéru (pokladní, tři holky v sále).
- **E u dveří:** otevře je směrem od chodce, znovu E zavře (nápověda dole „E: otevřít/zavřít dveře“). Stav dveří se
  neukládá. Na mapě se křídla kreslí taky (dřív by u kina byly díry).
- **Chůze:** zavřené křídlo zastaví, otevřeným se projde. Uvnitř zdi a sedadla podle tvaru, podlaha (`podlaha`)
  nepřekáží a chodec na ní stojí (pódium 0,6 m, schody). Jen přízemí (podlahy do 2,5 m). Na pozemku domu
  s interiérem je chodec užší (kruh 0,2 m místo 0,4 m) – průchod v obalu je po rámech dveří jen ~0,7 m a uličky
  mezi řadami sedadel jsou úzké.
- **Opravená stará chyba:** rozměry (`extent`) objektu se počítaly z vektoru, který už byl přesunutý → byly vždy
  nula (pouliční lampy se proto řadily podle nulového obdélníku). Teď se počítají ze skutečných rohů.
- Rig: `test3d chodec dum <x> <y> <m x> <m y> <směr>`, `test3d pudorys <x> <y> interier [jemne]`.

## 23. 10. noc – společné stromy a keře (kolega), lihovar 10 barev

- Kolega (`STROMY-SPOLECNE`, hráč: *„křoví máš doufám pár variant a rozmísťuješ je, šetříš, u každého domu,
  průmyslu stejné“*): stromy, keře a smrky už nejsou upečené v každém domě, jsou to společné doplňky `strom_v0–3`,
  `ker_v0–2`, `smrk_v0–1` (barva koruny číslem, každé místo jiná). 41 domů přepečených: 580 000 → 330 000
  trojúhelníků. Velikosti holek u domů nezměněné. Převzato.
- **Strom blokuje chůzi jen kmenem** (`kmen_r` v záznamu doplňku × `meritko`), koruna ne; keř tvarem jako dřív.
  V půdorysu domu (`test3d pudorys`) jsou stromy jen tečky kmenů.
- `doplnky` u záznamů průmyslu hra četla už dřív (stejná funkce jako u domů). Nově i u dlaždic `prumysl_vanilla`
  (objekt jen z doplňků, jako pouliční lampa), takže kolega může vanilla stromy nahradit `strom_v*`/`smrk_v*`.
- Lihovar zase 10 částí (nová část nádrže), staré sochy `socha_kamen`/`socha_bronz` ze sady pryč.

## 23. 10. noc – socha jedna s barvou, lihovar 7 barev, rafinerie jako lihovar, nové lucerny

- Převzato od kolegy (`SOCHA-BARVA`, `PRUCHOZI-PRUMYSL`, `TATRA-OSOBNI-V12`, `LIHOVAR-7-CASTI`): socha, gymnázium,
  hospoda, chatka, automat, lihovar (světlejší atlasy, průchody v plotech), dračí farma (zatím jen model, průmysl
  hry ještě není), lucerna na svíčku (skleněná krabička místo „svítilny na baterky“), papírová a kovaná lucerna,
  pochodeň s ohněm, `college_girl_za_volantem` (řidička Volgy cabrio – vybírá ji GRF), kobold, dinosaurus, zelená
  pečárka. **Moe a Lenny (Springfield) vynechaní**, farmářka dál pryč.
- **Socha jednou:** `socha.glb` s barvou číslem, `varianty` kámen 0 / bronz 1. Hra bere novou sochu místo
  socha_kamen/socha_bronz (ty zatím v sadě zůstávají). Kámen/bronz podle dlaždice (ne podle průmyslu – barvy se
  počítají i na vlákně kreslení, kde se na průmysl sahat nesmí). V rigu jedna kamenná, druhá bronzová.
- **Barvy číslem až 10 částí** (hráč: *„tak nech ten s deseti částmi“*): do 7 částí 4 bity na část (16 barev),
  nad 7 částí 3 bity (8 barev). Kolega mezitím lihovar zmenšil na 7 částí – obojí funguje.
- **Rafinerie jako lihovar** (`sdili`, hráč: *„rafinerie a lihovar můžou sdílet, stejné zařízení – destilace“*):
  na rafinerii (původní hra) stojí lihovar 3×3 na čtverci jejích políček nejblíž středu. Kolega navrhl kolem
  asfalt (`pv_dvur_asfalt`) – hráč: *„dej to pryč ten asfalt kolem lihovaru, vypadá to blbě“*, *„destilace ropy,
  nádrže“*: na zbylých políčkách jsou pece, nádrže a kolony rafinerie z `prumysl_vanilla` na udusané hlíně
  (`pv_dvur_hlina`). Mapa, pohled z očí, náhled u okna i chůze (díly blokují tvarem).
- **Nové lucerny do levé ruky:** papírová (Morrowind) a kovaná – věci inventáře od začátku, koupit v obchodě,
  L je střídá s pochodní a lucernou, jdou dát pod 1–9. Barva a dosah světla teď z indexu (`svetlo`: pochodeň 10 m,
  bliká; lucerna 6 m; kovaná 7 m, bliká). Papírová lucerna zatím bez barvy číslem (věci v ruce barvy neumí).
- `pruchody` hra nečte: chodec chodí podle tvaru, mezera v plotě je průchod sama.
- Zbývá: dračí farma jako průmysl (#177), nový les s MUSH (#178), vagony v5 světla, snimky.log, jizda.log, texty.log.

## 23. 10. večer – les: náhled a vstup, houby, lucerna, vidle, dračí farma

- **Náhled 3D u okna lesa** (hráč: *„les nefungoval 3D náhled u okna průmyslu“*): náhled znal jen průmysl z jednoho
  objektu a dvě pole, vanilla les (`prumysl_vanilla`, stromy po políčkách) se neotevřel. Teď skládá políčka jako mapa.
- **Do lesa nešlo vejít – příčina:** každé políčko průmyslu bez celého objektu (les a ostatní vanilla průmysly
  z dílů) bylo pro chodce zeď. Teď blokují jen budovy svým tvarem a stromy kmenem (0,3 m, rostliny pod 4 m vůbec);
  koruna nepřekáží. V rigu chodec prošel lesem z 72 na 78. Smrky vanilla lesa mají větve až k zemi – kolega je předělá.
- **Houby:** v rigu E utrhne houbu a v inventáři přibude (0→1→2). Houby rostou na trávě a mezi stromy (každé třetí
  políčko), v průmyslu les zatím ne (přijdou s novým lesem od kolegy). Utrhnout jde ta, u které dole svítí nápověda E.
  Velké houby kolem hliněných měst jsou stromy, ty se netrhají.
- Hráč: *„kde je lucerna, normální skleněná krabička na svíčku; místo lucerny žlutá svítilna na baterky“* – model
  `lucerna` (Roblox Lantern) tak opravdu vypadá; prosba kolegovi o lucernu se svíčkou. *„Čísla zbraní nejsou
  potřeba, to si přiřadí hráč jako v Morrowindu; chci světel a zbraní hodně; levá ruka světla, pravá zbraně.“*
- **Světla aut prosvítala autem** (hráč: *„světla aut jsou vidět skrz auto“*, *„bylo to hodně zblízka, když jdu
  nastoupit“*, *„auto světla by měla svítit jedním směrem, dopředu/dozadu, svíčka do všech stran“*) – příčina dvojí:
  záře lampy se kreslila jako skvrna přes všechno kolem, když byl střed lampy vidět (např. oknem auta), rozlila se
  přes karoserii před ní; a lampa neměla směr, takže přední světlo bylo vidět i zezadu (u vzdálených aut, kreslených
  jako plochý obrázek, i skrz auto). Teď: přední světla svítí dopředu, zadní dozadu (vidět zepředu/zezadu a z boku,
  ne zezadu přes auto), záře lampy (auta, okna) nesvítí přes nic, co je blíž než lampa (karta i procesor); kužely na
  silnici a lucerny/svíčky do všech stran jako dřív.
- **Rychlé klávesy 1–9 jako v Morrowindu** (hotovo): v inventáři najedeš myší na věc a zmáčkneš 1–9 – věc je pod tou
  klávesou (číslo před jménem v inventáři; jedna věc pod jednou klávesou, stará se přepíše). Z očí klávesa: zbraň do
  pravé ruky (znovu = schovat), světlo do levé (znovu = zhasnout), pivo/marihuana se použije. Pevná čísla zbraní pryč,
  na začátku je pod klávesami nic. Ukládá se do savu (verze 381, `walker_quick`). L dál přepíná světla, F tasí.
  Rig `test3d inventar klavesa <věc> <1-9>`.
- **Vidle** (kolegovy `predmety/vidle.glb`, par15) jako zbraň do pravé ruky (WalkerItem 23), dává `iddqd`, máchá se
  jako kladivo (dosah 2,6 m, zásah 2).
- **Dračí farma** (hráč): draci kolem farmy občas létají a dělají poruchy vlakům, autům a lodím; když dostanou
  studentku, nelétají a poruchy nedělají; studentka (college girl, velká) je na farmě u draků a stará se o ně.
- Hráč: *„kdo řídí starou Volgu kabriolet, to je ošklivý, to není college girl“* – GAZ-21 kabriolet je kolegův GRF
  (Tatra osobní v11, řidička `to_gaz21_ridicka`). Hráč: *„no jo, grrrrf dává řidiče“* – řidiče dává kolega v `posadka`,
  hra kreslí, koho záznam jmenuje; prosba kolegovi o college girl (`college_girl_ridicka`).
- **Houby v lese** (hráč): *„porostou furt bez studentek, abych tam mohl chodit na houby; se studentkama les produkuje
  houby; houby budou vozit osobáky“* – houby na zemi pořád, studentky přidají jen náklad `MUSH`, ten vozí osobní auta
  (kolegovi: přestavba osobáků v jeho GRF na `MUSH`). Kolegovi doplněno v `ODPOVED-LES-PRUMYSL-SVETLA.md`.

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
- **Kino v noci moc svítí, dveře z očí nevidět – příčina:** světla domů (`svetlo_NN`) svítí v pohledu z očí na okolí po
  pixelech; každé další světlo bralo „vlastní barvu ve tmě“ z pixelu, který už rozsvítilo to předchozí, a svítilo
  na něj znovu – pod 16 žárovkami vchodu kina se světlo samo na sobě nafouklo do bílé. Teď se světla všech lamp
  sečtou najednou, vlastní barva se bere z tmavého pixelu jednou a dohromady svítí nejvýš jako den (karta i procesor,
  `DrawEyeLights()`, shader lamp). Jedna lampa svítí jako dřív. Záře (kolečka) žárovek v řadě jsou menší – nejvýš
  jako vzdálenost k sousední (`ThreeDGlowShare()`), na mapě i z očí.
- **Strašidla u kolejí** (hráč: *„strašidla u kolejí podél tratě zvýšený výskyt, můžou tam lítat i neškodná
  prostěradla, Billy na tříkolce pojede po kolejích“*): k městským strašidlům ještě až 3 na rovných kolejích kolem
  hráče (i mimo města; země podle nejbližšího města – klima, téma, den/noc z indexu). Jakékoli strašidlo, které
  je venku (Billy, prostěradlo, Baba Jaga…); kdo má chůzi, jede/letí po kolejích kus trati a zpátky. Index může
  dát i `"kde": "koleje"` (jen u kolejí). Rig: `test3d kolej …`, `test3d zvirata` ukazuje místo `koleje`.
- **Interiéry – formát pro tvůrce** (hráč: *„připrav s grrrrf systém interiérů; tvůrce udělá dům i interiér; naše
  interiéry ve hře jako náš průmysl, rozšiřitelné tvůrci; inspirace OpenMW“*): grrrrf `hra/3d/PROSBA-INTERIERY.md`
  (1ea69b0), navazuje na `INTERIERY-PLAN.md` 16. 10. (interiér v domě, okny ven). Index `interier`: false / true
  (`<dům>_interier.glb`) / jméno souboru / `{soubor, postavy}`. Stejný rámec jako dům. Uzly: `podlaha[_N]`,
  `dvere_NN` (pant, E otevře), `okno_NN` (otvor pro pohled i střelu), `noc`+`svetlo_NN`, `truhla_NN` (později).
  Dům s interiérem: průchod za každými dveřmi, křídlo vchodu `dvere_NN` v glb domu. OpenMW jen nápady (GPL 3).
  **Napřed kino** (kolega pošle interiér); hra se kóduje, až přijde.
- **`snimky.log` rozšířený:** u kreslení z očí na kartě zvlášť `priprava` (procesor chystá trojúhelníky), `karta`
  (posílání a kreslení kartou), `rohu` (rohů trojúhelníků na snímek) a `procesorem` (snímky bez karty). Na tomhle
  stroji (llvmpipe, karta v procesoru): ~9 snímků z očí/s, příprava 2 ms, karta ~78 ms, 84 tisíc rohů. Telefon
  (~10 snímků/s, 30 na otočku) ukáže, jestli brzdí karta (Vortek/zink) nebo procesor.
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
