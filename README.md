# soft_i2c — programowe I2C dla BK72xx / LibreTiny

Bit-bang master I2C na zwyklych pinach GPIO, jako zewnetrzny komponent ESPHome.
Dzieki niemu walidator wejscia `i2c:` (sprawdzajacy tryb pinu) nie blokuje juz
pinow, ktore nie maja funkcji peryferyjnej w firmwarze LibreTiny.

**Platforma:** BK7231 / BK7231N (np. modul CBU), ESPHome >= 2026.9 z pluginem LibreTiny.
**Testowane na:** CBU (BK7231N) + SHT3xD, ESPHome 2026.9.1, LibreTiny 1.13.0.
**Nie testowane:** inne uklady BK7231, czujniki wymagajace SMBus, urzadzenia powyzej 100 kHz.

## Problem

BK7231 ma dwa kontrolery I2C z gorszo przypisanymi pinami:

| Kontroler | Piny | Konflikt |
|---|---|---|
| I2C1 | P20 / P21 | JTDO / JTDI — programator i logi ROM |
| I2C2 | P0 / P1 | TX2 / RX2 — logi ESPHome |

Do tego walidator ESPHome (`esphome/components/libretiny/gpio.py`) dopuszcza w sekcji
`i2c:` tylko piny z funkcja peryferyjna. Czujniki na pinie bez tej funkcji (typowe
thc102 na TX1/RX1, czyli P11/P10) ida do kosza, chociaz sprzetowo da sie je obsluzye.
Ten komponent omija ograniczenie, generujac protokol programowo.

## Instalacja

Katalog ma strukture `components/soft_i2c/`, wiec ESPHome znajdzie komponent bez
wpisywania `path:`. Wystarczy:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/TWOJ_LOGIN/soft-i2c
      ref: main
```

Lokalnie (katalog `esphome-components/` obok pliku YAML, a w nim `soft_i2c/`):

```yaml
external_components:
  - source:
      type: local
      path: ./esphome-components
```

Pusta sekcja `i2c:` jest wymagana. Komponent nie ladowuje bazowego `i2c` (to wymusiloby
walidacje pinow, przed ktora uciekamy), a bez niego `i2c_id:` nie istnieje w schemacie:

```yaml
i2c: []

soft_i2c:
  - id: bus_sht
    frequency: 100kHz
    sda_pin: TX1
    scl_pin: RX1
    sda_input_mode: ANALOG
    scl_input_mode: ANALOG

sensor:
  - platform: sht3xd
    name: Temperatura
    address: 0x44
    i2c_id: bus_sht
```

W ESPHome nowszym niz 2026.9 klucz `i2c_id:` bywa zastapiony przez `bus_id:`.

**`sda_input_mode: ANALOG` i `scl_input_mode: ANALOG` to nie dekoracja.** Na BK72xx
ustawienie pinu jako `INPUT` z wlaczonym pull-upem (`gpio_dma_pe(0)`) powoduje dryf
stanu wejscia i gubione zbocza. Sciezka domyslna (`PULLUP`) jest poprawna sprzetowo,
ale na tej platformie zawodna — stad w przykladach ANALOG plus rezystory 4.7 kOhm do VCC.

## Jak to dziala

Klasa implementuje `i2c::I2CBus` — ten sam interfejs, ktory spelnia natywne `i2c-hard`.
Kazdy sterownik dziedziczacy po `i2c::I2CDevice` (`sht3xd`, `bme280`, `bh1750` i tak
daljej) dziala bez zmian, bo dostaje `I2CBus&`.

- Start, stop, restart: klasyczne sekwencje na linii pojedynczej.
- Odczekanie ACK: SDA puszczona na dziewiatym takcie, potem puszczajace SCL (timeout SMBus).
- Wykrycie zakleszczenia: SDA niski przed START — rusza standardowa procedura odzysku
  z ESPHome (dziewiec taktow plus STOP).
- Timeout na czekaniu na zwolnienie SDA (okolo 10 ms) — brak ACK nie zapetla petli.
- Czas: 100 kHz wyliczane z `micros()`; realnie wychodzi okolo 40 kHz przez narzut
  wolania GPIO. Za malo na SMBus, za malo na probe 400 kHz.

## Uwagi eksploatacyjne

- **Flashowanie po UART1 dziala mimo I2C na TX1/RX1.** Stan bezczynnosci magistrali
  (obie linie wyciagniete w gore) to stan bezczynnosci UART. Wejscie w bootloader
  wymaga zwarcia GPIO8 (REC/BOOT) do GND przy rescie.
- **Zasilanie modulu.** CBU na slabych LDO (AS1117) zapada sie przy nadawaniu WiFi:
  spadek napiecia resetowal modul w polowie pierwszego odczytu. Zasilanie 5 V z
  zasilacza problem eliminuje. Objaw: brak logow „zadnych”, jakby urzadzenie nie startowalo.
- **Zapis do flasha wylacza przerwania.** `save_state` (preferencja
  `flash_write_interval`) blokuje przerwania na okolo 20 ms — w tym oknie bit-bang moze
  pogubic bity. Przy `update_interval: 15s` to jeden zly odczyt na setki; przy 1 s czesciej.
- **Wyacznie jeden master** na magistrali. Brak CLK stretching, brak multiplekserow
  (`tca9548`), brak `i2c_scan` i `i2c_restart_dump` (dzialaja tylko z natywnym `i2c`).
- Magistrala bez rezystorow (wejsciowy stan dryfujacy) da `recovery failed` w logu i
  czujnik bedzie niewidoczny — to bledna konfiguracja, nie usterka komponentu.

## Pliki

| Plik | Rola |
|---|---|
| `components/soft_i2c/__init__.py` | schemat YAML, rejestracja magistrali i pinow |
| `components/soft_i2c/soft_i2c.h` | deklatacja `SoftI2CBus` |
| `components/soft_i2c/soft_i2c.cpp` | bit-bang: START, stop, bajt, ACK, odzysk |
| `example.yaml` | pelny projekt na CBU + SHT3xD |

Zgloszenia i pytania: przez Issues. Chetnie poprawie pod inne uklady BK7231, jezeli
ktos podesle logi.
