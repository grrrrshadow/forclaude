# Vestavěné GRF hry (the game's own sets)

Každý `.grf` v této složce jde do každé nové hry, ať se jmenuje jakkoli a má
jakékoli GRF ID. Hráč ho v okně NewGRF vidí a může ho posunout, ale nemůže ho
odebrat ani vypnout; nevypne ho ani jiný GRF (Action E). Jsou tu proto, aby
vždycky něco odvezlo všech 128 nákladů velkého průmyslu hry.

- Přidat GRF = dát ho sem. Hra ani CMake se kvůli tomu nemění
  (`CMakeLists.txt` o složku výš bere `decouple/*.grf`).
- Ve hře se čte ze složky `baseset/decouple/`.
- Rozehrané savy se nemění: vestavěné GRF dostane jen nová hra.

Kód: `AppendBuiltinGRFs()` v `src/newgrf_config.cpp`.

## grafika/ – grafika hry (the game's own graphics)

Každý `.grf` v podsložce `grafika/` je něco jiného: **statický** GRF hry, tedy
jen obrázky. Je v každé hře včetně rozehraných savů, hráč ho v okně NewGRF
nevidí, nikam se neukládá a nesmí měnit stav hry (hra ho čte jako statický GRF,
stejně jako ty, které si hráč zapíše do `[newgrf-static]`).

- Obrázky hry (gymnázium, automat, socha, dlaždice marihuanové plantáže,
  chatka hulírny, holky na zastávce, ohňostroj, plakát) jsou od 8. 10. 2026
  rovnou v `openttd.grf` jako 32bpp zin4 a zin8, protože se staví kolegovým
  yaglem (`../openttd/openttd_yagl.py`, zdroj `openttd.yagl` a listy).
  `budovy.grf`, který tytéž obrázky nesl jako statický GRF, tím zanikl.
- **Od 12. 10. 2026 (3D) se `openttd.grf` nemění** (hráč) a 16× obrázky
  (zin16) jdou sem, každý GRF se zdroji v podsložce svého jména a postavený
  yaglem (`yagl -e <abs>/jmeno.grf <abs>/jmeno`), pod 100 MB kvůli GitHubu:
  `zastavka.grf` – holky na zastávce v 16× přes sprity `openttd.grf`
  (Action A, `zastavka/README.md`). Sprity sad grafiky hry smějí i do bloku
  hry `SPR_DECOUPLE_3D_BASE` přes Action A s čísly od 0x8000. **Rendery budov
  z osmi směrů (`budovy3d_1`–`8`) od 15. 10. nejsou**: domy i náš průmysl
  (gymnázium, sochy, automat, chatka, plantáž z dílů pole) jsou 3D objekty
  v `objekty/mirne/` a `objekty/prumysl/` (hráč: „můžem škrtnout 250 MB“,
  kolega: 40 MB místo 176 MB). Soubory `budovy3d_*.grf` může hráč
  z `baseset/decouple/grafika/` nainstalované hry smazat.
- Nový obrázek od kolegy ve 4× a 8× do `openttd.grf`: PNG do `../openttd/`,
  pak `python3 openttd_budovy.py && python3 openttd_yagl.py` tamtéž
  (viz `COMPILING.md`).

Kód: `AppendStaticGRFConfigs()` v `src/newgrf_config.cpp`.

Every `.grf` here goes into every new game, whatever its name or GRF ID; the
player sees it in the NewGRF window and may move it, but can neither remove it
nor switch it off, and no other set can switch it off. A `.grf` under
`grafika/` is a static set of the game's own graphics instead: in every game
and savegame, unseen in the NewGRF window, never saved, and it may not change
game state.

## objekty/ – budovy jako skutečné 3D objekty

Každá podsložka `objekty/<sada>/` je sada budov jako 3D objektů (glTF `.glb`,
formát v grrrrf `hra/3d/OBJEKTY-3D.md`) se seznamem `objekty.json`, který říká,
který dům původní hry a které jeho barevné varianty každý objekt nahrazuje.
Hra je čte z `baseset/decouple/objekty/<sada>/` a kreslí je v otočeném pohledu
a v pohledu z kabiny; při pohledu na sever zůstávají sprity.

- `mirne/` – všech 21 domů mírného klimatu původní hry, 31 souborů (od kolegy,
  grrrrf `hra/3d/objekty/mirne/`, textury z OpenGFX 7.1, GPL-2.0).
- `subarkticke/` – 19 objektů, domy subarktického klimatu (od kolegy, grrrrf
  `hra/3d/objekty/subarkticke/`). Dům pod sněžnou čárou a tentýž nad ní je jeden
  objekt: `nahrazuje_house` je seznam `[pod sněhem, nad sněhem]` a `snih` jméno
  PNG vedle `.glb` se zasněženou texturou přes tytéž UV.
- `subtropicke/` – 15 objektů, domy subtropického klimatu (od kolegy, grrrrf
  `hra/3d/objekty/subtropicke/`).
- `mars/` – 12 objektů, sedm domů sady (Fixed) OpenGFX Mars Houses (BaNaNaS
  `524A450B`), ze které hra staví marsovská města (od kolegy, grrrrf
  `hra/3d/objekty/mars/`). Dům ze sady se hledá podle `nahrazuje_grf` (id sady,
  jak ho hra vypisuje) a `nahrazuje_house_grf` (číslo domu v sadě).
- **Bílé domy kolem roku 1950** (15. 10., od kolegy, grrrrf `hra/3d/PROSBA-DOMY-1950.md`): 19 záznamů, které byly
  domy z ručních tabulek s texturami OpenGFX (kostel 03, městské domy 06, obchody 0E, 0F, 10, 1D, starý stadion 14,
  staré domy 18, chalupy 19, domy 1A, kino 27), jsou vlastní modely v kvalitě deseti domů níž: omítky, `barvy`,
  skutečné `dvere`, kola opřená o zeď, prádlo, kočárky a holky, co tam bydlí (`postavy`).
- **Dům 0x1F je budova ÚV KSČ** (od kolegy, `PROSBA-UV-KSC.md`) místo divadla; `nazev` dává jméno, které ukáže
  informace o dlaždici (česky „ÚV KSČ“). Divadlo bude vlastní GRF dům s objektem v balíčku.
- Deset domů, které byly rendery v `budovy3d_1`–`4` (hotel, socha, fontána, dva
  parky, moderní kanceláře, kancelářský blok, cihlové byty, divadlo (dnes ÚV KSČ), obchodní
  středisko; od kolegy, grrrrf `hra/3d/PROSBA-DOMY-Z-BUDOVY3D.md`), jsou v `mirne/`
  s normálami (světlo si dělá hra jako u vozidel) a s `barvy`: střecha, omítka
  a plot mají masku a paletu a každý dům na mapě má barvu podle své dlaždice.
  Vysoký kancelářský dům 0x1c zůstává na památku ruční tabulkou.
- **Vozidla tu nejsou** (hráč, 15. 10.: „objekty musí být stejně nezávislé jako GRF,
  to nemůže nést hra, to má být u GRF“): vozidlo jako 3D objekt přijde v balíčku u
  svého GRF v hráčově `newgrf/` (složka nebo tar, jako každý GRF):
  `<balíček>/<jméno>.grf`, `<balíček>/objekty/vozidla/objekty.json` s jeho `.glb`
  a PNG, `<balíček>/objekty/postavy/` s posádkou. Hra je najde sama; žádný build.
  První je kolegův `M62_Sergej_3D-v1.tar` (GRF `MAX3`, Sergej ve třech nátěrech).
- `auta/` – auta hry, která jezdí sama (od kolegy, grrrrf `hra/3d/objekty/auta/`, `ODPOVED-MODY-A-HRA.md`): Tatra 613 VB
  jezdí v otočeném pohledu po silnicích od stání jedné budovy ÚV KSČ ke stání jiné (`jizda`, `stani` u domu), postojí
  a jede dál. Je to výzdoba hry jako holky a Mat, ne vozidlo GRF; Tatry jako vozidla (`MAXt`) jsou mód ve svém balíčku.
- `efekty/` – efekty jako 3D objekty (od kolegy, grrrrf `hra/3d/objekty/efekty/`): ohňostroj
  nad chatkou (`ohnostroj.glb`, start a tři fáze koule, barva číslem z palety pro každou
  fázi), v otočeném pohledu; na sever zůstávají sprity.
- `hlinene/` – **hliněné město** (od kolegy, grrrrf `hra/3d/objekty/hlinene/`, `PROSBA-MESTO-HLINENE.md`): 13 vlastních
  modelů hliněných domů s kopulemi jako vesnice Morrowindu (34 záznamů, `tema: "hlinene"`, `nahrazuje_house` jako
  `mirne`) a 4 obří houby (`druh: "strom"`). Hliněné město (`Town::theme`, nastavení „Hliněná města“, první tři Drakov,
  Houbovice, Čarodějov) staví domy mírného klimatu jako každé město; ve 3D pohledech je hra kreslí hliněnými modely
  a stromy na jeho území houbami. Tamriel Rebuilt jen vzor stylu, nic z něj.
- `tvorove/` – **draci** (od kolegy, grrrrf `hra/3d/objekty/tvorove/`, `PROSBA-DRACI.md`; OpenGameArt, CC0, kráčející
  CC BY 3.0 – autoři v `objekty.json`): `druh: "tvor"`, každá animace (klid, chůze, útok, smrt, let) jako snímky
  v uzlech. Občas přiletí kamkoli na mapu, s otevřeným pohledem z první osoby spíš k chodci; hnědý a černý přistanou,
  chodí, jdou po chodci a útočí, kladivem padnou; ledový a čínský přelétají a jednou se snesou (`three_d_dragons.cpp`).
  Soubory `_kostra.glb` (síť s kostrou a animacemi pod stejnými jmény, klíč `kostra`): hra z nich pózuje draka
  každý tik místo snímků a mezi animacemi pózy 8 tiků prolíná (barvy materiálů, víc sítí s kostrou: tělo a oči);
  bez kostry (čínský drak) zůstávají snímky.
- `carodejnice/` – **čarodějnice a strašidlo** létající v noci (od kolegy, grrrrf `hra/3d/objekty/carodejnice/`,
  `PROSBA-CARODEJNICE.md`, `CARODEJNICE-PRO-HRU.md`): Marisa (skydash, Sketchfab, CC BY 4.0), čarodějnice na koštěti
  ve fialovém a v zeleném (Witch Roll, xinus22, Sketchfab, CC BY 4.0), strašidlo (ghost, remi136, Sketchfab,
  CC BY 4.0); lucerna Roblox Lantern (Luigi2013, Sketchfab, CC BY 4.0), ruční svítilna kolegova. Modely otočené,
  zmenšené, s texturami nejvýš 1024 px, let ze snímků. `druh: "carodejnice"`, snímky letu `let_NN` s místy lucerny
  (`lucerna_NN`), svítilny (`svitilna_NN`, `paprsek_NN`) a sklem pod `noc_NN`, světla v `svetla`. V noci jich létá
  1–3 nad městy, lucerna svítí kolem sebe a svítilna dělá paprsek se skvrnou na zemi (`three_d_witches.cpp`).
- `predmety/` – předměty chodce v pohledu z první osoby (od kolegy, grrrrf `hra/3d/objekty/predmety/`,
  `PROSBA-PREDMETY-KLADIVO-TRUHLA.md`): válečné kladivo a truhla z OpenMW Example Suite
  (gitlab.com/OpenMW/example-suite, modely **CC0**, volné dílo), `druh: "predmet"`. Kladivo chodec nese v inventáři
  od začátku; F ho tasí a schová, levé tlačítko nebo mezerník s ním máchne (`first_person_gui.cpp`). Od 17. 10.
  (`LUK-SIP-PRO-HRU.md`) i luk a šíp (kolegovy vlastní, GPL-2.0), kuše (Example Suite, CC0) a šipka (kolegova),
  meč a obouruční meč (Example Suite, CC0) a zvuky luku `luk_napnuti.wav` a `luk_vystrel.wav` (Example Suite, CC0).
  Stavy předmětu (`stavy`: luk `volno`/`napnuto`, kuše `natazeno`/`vystreleno`) hra čte jako figury
  `<jmeno>/<stav>` (jen ten uzel), úchyt z uzlu `uchyt`, zářez z uzlu `zarez_<stav>`, zvuky z klíče `zvuky`.
  Klávesy 1–5 zbraň, cheat `iddqd` v konzoli dá zbraně, které chybí, a doplní šípy a šipky na 50.
  Od 18. 10. (`PROSBA-NOC-ZBRANE-TRABANT-LAMPY.md`) kulomet (Machine Gun, Alexeykzm425, Sketchfab, CC BY 4.0),
  raketomet (Roblox Classic Rocket Launcher, OrangeSauceu, Sketchfab, CC BY 4.0), raketa (Rocket, photon (that one
  larry), Sketchfab, CC BY 4.0), lucerna do ruky (Roblox Lantern, Luigi2013, Sketchfab, CC BY 4.0), kolegova
  pochodeň a náboj (GPL-2.0) a zvuk zhasnutí pochodně `pochoden_zhasnuti.wav` (`torch_out.wav` z Example Suite,
  CC0). Modely zmenšené nebo zvětšené a otočené.
- `lampy/` – lampy ve stylu Morrowindu (od kolegy, grrrrf `hra/3d/objekty/lampy/`, kolegovy vlastní, GPL-2.0):
  papírová lucerna, lampa na sloupu, nástěnná pochodeň, svícen, lampička a ohnivý koš, `druh: "doplnek"` s uzlem
  `noc` (papír, sklo a plamen svítí v noci), místy `svetlo_NN` a papírem v barvě číslem (`barvy`, část `papir`).
- `psi/`, `zvirata/`, `strasidla/`, `houby/` – psi, kočky, lišky a vlci, prodavači psů, zvířata u měst, strašidla
  a houby ke sbírání (od kolegy, grrrrf `hra/3d/objekty/…`, `PSI-ZVIRATA-HOUBY-PRO-HRU.md`; otočené, zmenšené na
  skutečnou velikost, kostra a animace modelu ve formátu `pozy`). Všechno ze Sketchfabu. **CC BY 4.0**: gepard,
  žirafa a mamut (planeta-elefante), somálská kočka (DreamNoms), kreslená kočka (Omabuarts Studio), liška
  (dragonsnap), fenek (Dylan Martínez Sánchez), Falkor (sharksie), roztomilý pes (Tech developers), šedý vlk a veselý
  pes (Stakler), malý pes (samuelsgr art), lovecký pes (Jurassic Radassic), pouliční pes (MAROUANE), nafocený pes
  (3DigitalArtist), skákající liška (sheana), šedá kočka (savounited), rezavá kočka (ilhamtr), lesní vlk (AX-7YK),
  dvoubarevná kočka (kenchoo), stylizovaný vlk (ESO), vlk u dveří (victory_mirosya), prodavačka psů farmářka (AKM),
  prodavač Bodi (Jacob Quintana), prodavač dalmatin (iRahulRajput), los, wapiti, jelen, laň, vidloroh, ovce
  tlustorohá a medvěd (Stylized Animals BIG PACK, WildMesh 3D), slon (GremorySaiyan), opice (Guilherme Navarro,
  FabianRetamal, ToonMesh 3D, HumAi Club), houbový bojovník (POTVORA, zjednodušený), houby (Mushrooms, leha_pu,
  rozdělené, svítící verze kolegova). **CC BY-SA 4.0** (úpravy pod stejnou licencí): hnědý pes (1-3D.com), bulteriér,
  baset, malamut, jack russell a bulmastif (low poly dog pack, compost the second). **CC BY-NC 4.0** (jen
  nekomerčně): kočka, která chodí (pamoan10), vlk (OLEKSO), lev, loutka na tříkolce a přízrak v `carodejnice/`
  (LostBoyz2078). Odkazy na modely jsou v `objekty.json` u každého záznamu (`autor`, `licence`).
- Sady tady (`mirne/`, … `postavy/`) hra čte i z hráčovy složky
  `Dokumenty/OpenTTD/baseset/decouple/objekty/<sada>/`, ta má přednost, takže ani
  nový dům nepotřebuje build.
- `postavy/` – postavy jako 3D objekty (od kolegy, grrrrf `hra/3d/objekty/postavy/`):
  College Girl (Rotmill, Sketchfab, CC BY 4.0) stojící, kouřící (osm snímků
  `snimek_00`–`07` v jednom souboru, `animace`) a opřená jako řidička; Matylda
  (Matilda, nicolekeane, Sketchfab, **CC BY-NC-SA 4.0**: jen nekomerčně, obrázky
  s ní pod stejnou licencí a se jménem autorky); holka v tyrkysovém tílku
  (Character People Girl 001, kiemtruongkts, Sketchfab, CC BY 4.0). Jsou na
  zastávkách v otočeném pohledu; na sever zůstávají sprity. Mat opravář (model
  Armored Wave, Sketchfab, CC BY 4.0; postavička ze seriálu Pat a Mat, Lubomír
  Beneš / Patmat Film, fanouškovský model, nekomerčně) chodí v otočeném pohledu
  po chodnících měst ode dveří domů ke dveřím (`dvere` v `mirne/objekty.json`).
  Na střeše hotelu stojí kouřící College Girl u slunečníků (`postavy` u hotelu).
  Mat točí pivo ze sudu u grilu na stadionu (`mat_toci`) a řídí hasičskou Tatru (`mat_ridi`). Chimata (Chimata for
  Touhou Multi Scroll Shooting 2, skydash, Sketchfab, CC BY 4.0, otočená, textury nejvýš 1024 px) čeká na interiéry.
- Domy se hledají podle čísla, v kterémkoli klimatu: i v tematickém městě
  mírného klimatu, které staví arktické nebo pouštní domy („Domy z“).
- Přidat sadu = dát sem složku se soubory a `objekty.json`; CMake ji zkopíruje
  (`.glb`, `.png` a `objekty.json`).
- Hra je kreslí v otočeném pohledu a v pohledu z kabiny; vozidla i na sever.

Kód: `src/three_d_objects.cpp`, popis v `docs/decouple_three_d.md`.

## zvuky/ – zvuky hry

Každý `.wav` v `zvuky/` hra čte z `baseset/decouple/zvuky/`: klakson auta
(`klakson.wav`, klávesa I ve vozidle). Původ a licence v `zvuky/README.md`.
