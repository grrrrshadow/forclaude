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
- Kolega teď domy předělává kvůli **svítícím oknům** v noci → vezmu nové soubory, okna napojím na noc.
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
- Lampy a okna v noci.
- Kostra a animace glTF; draci; interiéry; boj.
- Klínek sousedního domu přes průčelí (#41). Domy z GRF objektem z balíčku (MAXd, #47). Doplňky (maják) podle stavu GRF.
