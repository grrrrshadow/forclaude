# Chůvička Finder

Android appka (Kotlin, min. Android 8.0, testováno pro Android 14/15) na pomoc při hledání
ztracené/zapadlé **analogové dětské chůvičky** podle síly jejího rádiového signálu — funguje
jako "hledač pokladu": čím blíž k vysílači jdeš, tím silnější je metr a tím rychleji appka píská.

## Proč to nejde jen tak, žádným "senzorem" v telefonu

Běžný telefon (Oppo A18, Motorola Edge 60 Fusion apod.) **nemá žádný obecný RF přijímač**.
Umí přijímat jen to, na co má vyrobený čip — WiFi a Bluetooth. Klasická analogová chůvička
vysílá typicky na 40 MHz, 49 MHz, 863–865 MHz, 900 MHz nebo DECT 1,9 GHz — na žádné z toho
telefon fyzicky nedosáhne bez přídavného hardwaru.

Proto appka potřebuje **externí USB RTL-SDR dongle** (např. "RTL-SDR Blog V3/V4", cena cca
600–900 Kč) připojený přes **USB-OTG kabel/redukci** k telefonu. RTL-SDR dongle s běžným
tunerem (R820T2) pokrývá zhruba **24 MHz – 1700 MHz** — to stačí na 40/49 MHz i 863–928 MHz
pásma, ale **ne** na DECT (1880–1900 MHz) ani 2,4 GHz — tam by bylo potřeba dražší SDR
(HackRF, Airspy) nebo úplně jiný přístup.

**Zjisti si nejdřív skutečnou frekvenci své chůvičky** — bývá na štítku uvnitř bateriového
prostoru (FCC ID / CE značení), nebo v manuálu výrobce.

## Jak appka pracuje (architektura)

Aby appka nemusela znovu vymýšlet křehký nízkoúrovňový USB/tuner ovladač (registry čipu
RTL2832U a R820T2), nemluví s dongle přímo. Místo toho se chová jako klient dobře zdokumentovaného
a roky stabilního protokolu **`rtl_tcp`** (součást projektu librtlsdr):

1. V telefonu nainstaluješ a spustíš svobodnou appku **"RTL2832U Driver"**
   (F-Droid / Google Play, autor Martin Marinov — stejný ovladač používá i známá appka
   "RF Analyzer"). Ta se postará o USB-OTG a nízkoúrovňovou komunikaci s dongle a
   umí spustit lokální `rtl_tcp` server (výchozí port 1234).
2. Tahle appka (Chůvička Finder) se k tomu serveru připojí jako běžný TCP klient
   (výchozí `127.0.0.1:1234`), pošle příkazy pro nastavení frekvence/vzorkovací
   frekvence/zesílení a čte proud syrových I/Q vzorků.
3. Z I/Q vzorků appka počítá FFT a sleduje výkon signálu těsně kolem nastavené
   frekvence → z toho je metr síly signálu + zvuková nápověda (piano rychlejší = blíž).

Žádná GPL část z RF Analyzer / rtl_tcp_andro- není v tomto repozitáři zkopírovaná — jde
o samostatnou appku, která se s tím ovladačem propojuje jen přes obyčejný TCP socket.

## Použití

1. Nainstaluj appku "RTL2832U Driver", zapoj dongle přes OTG kabel, potvrď oprávnění
   k USB zařízení, v appce spusť `rtl_tcp` server.
2. Spusť Chůvička Finder, klikni **Připojit** (výchozí `127.0.0.1:1234` funguje, pokud obě
   appky běží na stejném telefonu).
3. Vyber pásmo (nebo zadej frekvenci ručně podle štítku na chůvičce). Appka tě upozorní,
   pokud je frekvence mimo rozsah běžného dongle.
4. **Chůvičku vypni/odnes pryč** a klikni **Kalibrovat ticho** (3 s) — appka si zapamatuje
   šumové pozadí, aby metr ukazoval skutečně jen sílu chůvičky, ne obecný šum v okolí.
5. Zapni chůvičku, klikni **Spustit hledání** a pomalu se pohybuj po místnosti/domě —
   sleduj metr a/nebo poslouchej pípání.
6. Pokud přesně neznáš frekvenci, použij **"Najít přesnou frekvenci"** — appka prohledá
   okolí ±300 kHz kolem zadané frekvence a ukáže, kde je signál nejsilnější.

## Sestavení

Standardní Gradle/Android Studio projekt.

```
cd android-chuvicka-finder
# Otevři v Android Studio (vygeneruje gradlew wrapper automaticky), nebo:
gradle wrapper --gradle-version 8.7
./gradlew assembleDebug
```

- `compileSdk`/`targetSdk` 34, `minSdk` 26.
- Bez nativního kódu (NDK) — čistě Kotlin/Android SDK, takže by mělo jít sestavit
  bez problémů i bez USB/hardwarového laboratorního setupu.

## ⚠️ Stav a omezení

- **Kód zatím nebyl testovaný na reálném hardwaru** (v tomto vývojovém prostředí nebyl
  k dispozici telefon ani RTL-SDR dongle) — logika `rtl_tcp` protokolu, FFT a UI je
  napsaná podle veřejně zdokumentovaného, roky neměnného protokolu, ale první reálné
  zapojení si zaslouží ověření (viz kontrolní seznam níže).
- Appka neumí demodulovat/přehrát zvuk chůvičky — jen ukazuje sílu signálu (postačí na
  "studeno/teplo" hledání, na poslech obsahu by bylo potřeba přidat FM demodulátor).
- Pro DECT (1,9 GHz) a 2,4 GHz chůvičky běžný RTL-SDR dongle nestačí.

### Kontrolní seznam po prvním zapojení hardwaru

1. Appka "RTL2832U Driver" rozpozná dongle a rtl_tcp server naběhne (zkontroluj log/stav
   v té appce).
2. Chůvička Finder se úspěšně připojí a zobrazí typ toho tuneru (typicky "R820T").
3. Po kalibraci ticha a zapnutí chůvičky metr viditelně vyskočí nahoru, když se přiblížíš.
4. Pokud metr nereaguje vůbec, zkus tlačítko "Najít přesnou frekvenci" — možná je potřeba
   frekvenci doladit o desítky kHz (levné chůvičky mají nepřesný krystal).
