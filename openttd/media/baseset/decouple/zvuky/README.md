# Zvuky hry (the game's own sounds)

Každý `.wav` tady hra čte ze složky `baseset/decouple/zvuky/` (CMake o dvě
složky výš bere `decouple/zvuky/*.wav`). Formát: WAV, nekomprimované PCM,
mono, 8 nebo 16 bitů (čte ho zavaděč zvuků sad, `src/soundloader_wav.cpp`).

- `klakson.wav` – klakson auta, klávesa I v kabině a ve vozidle v pohledu
  z první osoby (`CarHornSound()` v `src/cab_view_gui.cpp`). Bez souboru auto
  netroubí. Zdroj: Pixabay, „Automobile Horn 02“ od Universfield (poslal
  hráč 17. 10. 2026), licence Pixabay Content License
  (https://pixabay.com/service/license-summary/). Upraveno: mono 22 050 Hz
  16 bitů, ticho na začátku a na konci ustřižené, +9 dB.

Every `.wav` here is read by the game from `baseset/decouple/zvuky/`:
uncompressed PCM, mono, 8 or 16 bits. `klakson.wav` is the car's horn
("Automobile Horn 02" by Universfield on Pixabay, Pixabay Content License;
made mono 22050 Hz 16-bit, silence trimmed, +9 dB).
