# OpenTTD Windows Build — poznámky a postup

Cíl: mít **stoprocentně reprodukovatelný** postup kompilace OpenTTD pro
Windows (openttd.exe), postavený na oficiálním buildovacím postupu OpenTTD,
tak abychom nejdřív ověřili, že umíme zkompilovat čistý (vanilla) zdrojový
kód bez chyb — a teprve pak začali upravovat kód hry. Díky tomu, když po
úpravě kódu build spadne, víme jistě, že chyba je v našich úpravách, ne
v nestabilním/nedeterministickém CI postupu.

## Zdroj

- Oficiální repozitář: `OpenTTD/OpenTTD` (https://github.com/OpenTTD/OpenTTD)
- Verze: **15.3** (vydáno 2026-04-04, autor releasu PeterN)
- Tag: `15.3`
- Commit, na který tag `15.3` ukazuje: `14ec60f248547d4d062a1160f0fc26d742319888`
  (zaznamenáno při přípravě tohoto postupu — pokud by GitHub Actions checkout
  vytáhl jiný commit pro tag `15.3`, je to signál, že se tag přesunul/je
  něco jinak, a je potřeba to prošetřit).
- Žádné git submoduly nejsou potřeba (`.gitmodules` v repu není).

## Odkud vychází náš postup

Vycházel jsem přímo z oficiálních GitHub Actions workflow souborů v tagu
`15.3` repozitáře `OpenTTD/OpenTTD`:

- `.github/workflows/release.yml` — hlavní orchestrátor releasu (spouští
  source/docs/linux/macos/windows/windows-store joby a pak uploady na
  CDN/Steam/GOG/Windows Store).
- `.github/workflows/release-source.yml` — připraví zdrojový tarball,
  vygeneruje verzi/changelog, odstraní `.git` z distribuovaného archivu.
- `.github/workflows/release-windows.yml` — **toto je job, který reálně
  produkuje `openttd.exe`, který se stahuje z openttd.org.**
- `docs/../COMPILING.md`, `vcpkg.json`, `cmake/scripts/FindVersion.cmake` —
  pro pochopení závislostí a jak se určuje verze.

Náš workflow `.github/workflows/build-windows.yml` je zjednodušená, ale
věrná kopie kroků z `release-windows.yml` (job `windows`, větev "bez
instalátoru", protože instalátor a podepisování stejně nejde replikovat
bez OpenTTD interních tajných klíčů — viz níže).

## Přesné kroky, které oficiální build dělá (a náš taky)

1. **Checkout zdroje.** Oficiálně: stažení připraveného `source.tar.gz`
   (bez `.git`, ale s vygenerovaným `.ottdrev` souborem obsahujícím verzi).
   My místo toho děláme přímý `actions/checkout` na `OpenTTD/OpenTTD` s
   `ref: 15.3` a `fetch-depth: 0` (celá historie + tagy). Důvod: CMake
   (`cmake/scripts/FindVersion.cmake`) při přítomnosti `.git` adresáře sám
   spustí `git name-rev --tags` a zjistí verzi `15.3` stejně, jako by to
   udělal oficiální postup z `.ottdrev`. Funkčně stejný výsledek, o krok
   jednodušší pipeline.
2. **Rust toolchain** (`dtolnay/rust-toolchain@stable`) + cache
   (`Swatinem/rust-cache@v2`) — OpenTTD používá Rust pro část nástrojů
   (breakpad symbol dumping).
3. **vcpkg setup** přes `OpenTTD/actions/setup-vcpkg@v6` — oficiální akce
   OpenTTD projektu, stará se o instalaci vcpkg a jeho cache. `vcpkg.json`
   v kořeni repozitáře určuje přesné závislosti (breakpad, liblzma, libpng,
   lzo, opusfile, zlib pro Windows) a je zamčený na
   `builtin-baseline: b2cb0da531c2f1f740045bfe7c4dac59f0b2b69c` — tedy
   přesná verze vcpkg registru, ne "nejnovější, co je zrovna k dispozici".
4. **Choco závislosti:** `pandoc` (nepovinné — generuje `COPYING.rtf` do
   balíčku, build bez něj neselže, ale pro věrnost ho instalujeme).
   Vynechali jsme `nsis`, protože nestavíme instalátor (viz níže).
5. **`cargo install dump_syms`** — nástroj pro generování breakpad symbolů
   z `.pdb`, použitý později pro ladění pádů.
6. **MSVC problem matcher** (`ammaraskar/msvc-problem-matcher@master`) —
   jen kosmetika, zvýrazní chyby kompilátoru v logu GitHub Actions.
7. **Build "host tools"** — nejdřív se v `build-host/` zkompiluje `tools`
   target (`-DOPTION_TOOLS_ONLY=ON`), tedy nástroje jako `strgen`, které
   generují zdrojáky (string tabulky) pro hlavní build. Tohle se dělá
   v `x64` dev prostředí bez ohledu na cílovou architekturu.
8. **Hlavní build** — nastaví se MSVC dev prostředí pro cílovou architekturu
   (`x64`/`x86`/`arm64`) a spustí se:
   ```
   cmake <zdroj> -GNinja \
     -DVCPKG_TARGET_TRIPLET=<arch>-windows-static \
     -DCMAKE_TOOLCHAIN_FILE=<vcpkg toolchain z kroku 3> \
     -DHOST_BINARY_DIR=<build-host adresář z kroku 7> \
     -DCMAKE_BUILD_TYPE=RelWithDebInfo
   cmake --build . --target openttd
   ```
9. **Breakpad symboly** — `dump_syms openttd.pdb --inlines --store symbols`.
10. **CPack** — vytvoří distribuční balíček (zip) stejně jako oficiální
    build; přesune `.pdb` a `.exe` do složky symbolů podle jejich CODE_ID.
11. **Nahrání artefaktů** — bundle (zip), samostatný `openttd.exe`, a
    symboly, jako výstupy GitHub Actions run (ke stažení z UI).

## Zásadní poznatek: `windows-latest` není bezpečné pro reprodukovatelnost

Run #2 (viz tabulka výše) odhalil důležitou věc, kterou je potřeba mít na
paměti pořád, i po úpravách kódu OpenTTD:

`runs-on: windows-latest` je pohyblivý cíl — GitHub pod tímto názvem časem
vymění celý image (aktuálně obsahuje Visual Studio "18" Enterprise, MSVC
toolset `14.51.36231`, mnohem novější než VS2022, kterou OpenTTD 15.3
oficiálně používal při vydání 2026-04-04). Novější MSVC odstranil
zastaralou STL kompatibilitu `stdext::checked_array_iterator`. Zdrojový kód
knihovny **breakpad**, tak jak je zamčený ve `vcpkg.json` (port
`2023.06.01`, přes `builtin-baseline`), ji používá v
`src/client/windows/crash_generation/minidump_generator.cc:183` — s novým
kompilátorem to spadne na:

```
error C2653: 'stdext': is not a class or namespace name
error C2065: 'checked_array_iterator': undeclared identifier
error C2059: syntax error: '>'
```

Tohle **není chyba v OpenTTD kódu ani v našem workflow** — je to nekompatibilita
mezi starým vcpkg portem a novým MSVC kompilátorem, která by časem začala
škobrtat i oficiálnímu OpenTTD CI (pokud a dokud OpenTTD nezvýší
`builtin-baseline` na novější breakpad, nebo GitHub nezmění, co `windows-latest`
znamená).

**Řešení:** connect runner připnutý na `windows-2022` (konkrétní, stabilní
image), místo pohyblivého `windows-latest`. Tohle je přesně ten typ
"detailu, co si musíme pamatovat", o který šlo od začátku — bez tohoto
zápisu by se stejná chyba mohla objevit znovu v budoucnu a vypadat jako
chyba v našich úpravách kódu, i když by šlo jen o to, že se pod nohama
změnil build runner.

## Co jsme vědomě vynechali a proč

- **NSIS instalátor** (`-DOPTION_USE_NSIS=ON`) — instalátor se v oficiálním
  postupu buildí jen pro tagované release verze. Nám stačí samotný
  `openttd.exe`; instalátor je jen "obalovač" kolem stejného exe.
- **Azure code signing** — oficiální build podepisuje `.exe`/instalátor
  certifikátem OpenTTD projektu (`os/windows/sign.bat` + Azure Key Vault
  secrets). Tenhle klíč nemáme a mít nemůžeme. Důsledek: Windows SmartScreen
  může při prvním spuštění ukázat "neznámý vydavatel" — funkčnost programu
  to nijak neovlivňuje.
- **survey_key** — oficiální build generuje a vkládá "survey key" (pro
  anonymní telemetrii OpenTTD projektu) přes jejich privátní signing klíč.
  My necháváme `OPTION_SURVEY_KEY` na výchozí hodnotě (prázdno) — to je
  přesně to, co se stane i official CI, když běží ve forku bez nastavené
  `SURVEY_TYPE` proměnné. Nemá to žádný vliv na funkčnost hry.
- **Ostatní platformy a joby** (`release-docs`, `release-linux`,
  `release-macos`, `release-windows-store`, `upload-cdn`, `upload-steam`,
  `upload-gog`, `upload-windows-store`) — netýkají se Windows `openttd.exe`,
  vyžadují další tajné klíče/účty (Steam, GOG, Microsoft Store, OpenTTD CDN),
  které nemáme a nepotřebujeme.
- **Matice architektur** — oficiální build dělá `x86`, `x64` i `arm64`
  najednou. Náš workflow to umí taky (vstupní parametr `arch`), ale zatím
  spouštíme jen `x64` (běžná 64bit Windows architektura), dokud nemáme
  ověřeno, že je pipeline stabilní a opakovatelná. Jakmile to potvrdíme,
  můžeme přidat i `x86`/`arm64` do jednoho společného runu (matrix).

## Reprodukovatelnost — jak to ověřujeme

Workflow (`build-windows.yml`) se spouští ručně (`workflow_dispatch`) se
vstupy `source_repo`, `source_ref`, `arch` — výchozí hodnoty odpovídají
přesně `OpenTTD/OpenTTD` na tagu `15.3`, `x64`. Dokud neupravujeme kód,
každé spuštění se stejnými vstupy musí dát **funkčně stejný** `openttd.exe`
(stejná verze nástrojů/závislostí je zamčená přes `vcpkg.json`
`builtin-baseline`, `dtolnay/rust-toolchain@stable` a `windows-latest`
runner OS image).

Log jednotlivých spuštění (doplňovat po každém běhu):

| # | Datum | source_ref | arch | Výsledek | Poznámka |
|---|-------|-----------|------|----------|----------|
| 1 | 2026-08-13 | (prázdné → viz poznámka) | (prázdné → viz poznámka) | ❌ selhalo | Spuštěno přes `push` trigger (workaround, viz níže), ne přes `workflow_dispatch` → `inputs.*` byly prázdné. Krok "Checkout OpenTTD source" proto checkoutnul náš vlastní repo `forclaude` místo `OpenTTD/OpenTTD`. CMake selhal: `CMake Error: The source directory "D:/a/forclaude/forclaude" does not appear to contain CMakeLists.txt.` OpenTTD zdroj se vůbec nestáhl, jde o chybu naší pipeline (workflow file), ne o OpenTTD kód. **Oprava:** přidány fallback výrazy `${{ inputs.X || 'default' }}` na všech místech, kde se `inputs.*` používá, aby workflow fungoval správně i bez `workflow_dispatch` vstupů (commit "Add fallback defaults..."). |
| 2 | 2026-08-13 | 15.3 (push, s fallbackem) | x64 | ❌ selhalo | Checkout, vcpkg i "Build tools" proběhly správně (fallback fungoval). Spadl krok "Build OpenTTD" při kompilaci vcpkg závislosti **breakpad**: `error C2653: 'stdext'...`, `error C2065: 'checked_array_iterator'...`. Příčina: `windows-latest` teď nese mnohem novější MSVC, který odstranil starou STL kompatibilitu, kterou používá pinned verze breakpad portu. Detailní rozbor viz sekce "Zásadní poznatek: windows-latest není bezpečné..." níže. **Oprava:** `runs-on` připnuto na `windows-2022`. |
| 3 | 2026-08-13 | 15.3 (workflow_dispatch) | x64 | ❌ selhalo (potvrzeno) | Spuštěno pro ověření, že `workflow_dispatch` API dispatch teď funguje (potvrzeno — `run_workflow` vrátil "queued" místo 404, ne 404). Běželo na `windows-latest` (ještě před opravou runneru) a spadlo na úplně stejném kroku a stejnou chybou jako #2 (breakpad `stdext::checked_array_iterator`), čímž potvrdilo diagnózu. |
| 4 | 2026-08-13 | 15.3 (workflow_dispatch) | x64 | ⏳ probíhá (nadějně) | Na `windows-2022`. Dostal se přes krok "Build OpenTTD" (kde předtím padalo #2 i #3) a v tuto chvíli je stále v tomto kroku (kompilace samotné hry, po úspěšné kompilaci breakpad závislosti) — zatím žádná chyba. Výsledek bude doplněn po dokončení. |
| 5 | — | 15.3 | x64 | | ověření reprodukovatelnosti #2 (musí dát stejný výsledek jako #4) |
| 6 | — | 15.3 | x64 | | ověření reprodukovatelnosti #3 |

Až tu budeme mít 2-3 zelené, identické běhy, přesuneme se k úpravám kódu
(vlastní fork/branch zdrojáků OpenTTD v tomto repu) a workflow přesměrujeme
na náš vlastní zdroj místo `OpenTTD/OpenTTD`.

## Jak spustit build

Poznámka k historii: zpočátku GitHub Actions REST API pro toto propojení
(session ↔ GitHub) vracelo 404 i s Actions povoleným v nastavení
repozitáře (viz run #1-#2, kde jsme workflow spouštěli přes `push` na
branch jako workaround). Po chvíli se povolení propsalo a `workflow_dispatch`
API dispatch (`run_workflow`) začal fungovat normálně (run #3 dál) — `push`
trigger byl proto z workflow souboru odstraněn, protože zbytečně spouštěl
build i při commitech, které se buildu netýkaly (např. run #5, zrušen jako
duplicitní s #4).

1. GitHub → repozitář `grrrrshadow/forclaude` → záložka **Actions**.
2. Vlevo vybrat workflow **"Build OpenTTD (Windows)"**.
3. Tlačítko **"Run workflow"** → ponechat výchozí hodnoty
   (`OpenTTD/OpenTTD`, `15.3`, `x64`) → **Run workflow**.
   (Nebo přes GitHub API `workflow_dispatch` — takhle build spouštím já.)
4. Po doběhnutí (běžně cca 15-25 minut kvůli kompilaci vcpkg závislostí a
   samotné hry) stáhnout artefakt `openttd-exe-x64` ze stránky daného běhu.

## Otevřené otázky / TODO

- [ ] Spustit build poprvé a zaznamenat výsledek/čas/případné chyby do
      tabulky výše.
- [ ] Spustit ještě 2× pro potvrzení reprodukovatelnosti.
- [ ] Rozhodnout, jak budeme vendorovat zdrojový kód OpenTTD do tohoto repa
      až začneme dělat úpravy (fork OpenTTD/OpenTTD vs. vlastní strom
      v tomto repu).
