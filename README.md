# soft_i2c - programowe I2C dla BK72xx / LibreTiny

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
| I2C1 | P20 / P21 | JTDO / JTDI - programator i logi ROM |
| I2C2 | P0 / P1 | TX2 / RX2 - logi ESPHome |

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
      url: https://github.com/codekaczmarek/soft-i2c.git
      ref: v1.0.0
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
    sda: TX1
    scl: RX1
    frequency: 100kHz
    scan: true
    internal_pullup: true

sensor:
  - platform: sht3xd
    address: 0x44
    i2c_id: bus_sht
    temperature:
      name: Temperatura
    humidity:
      name: Wilgotnosc
```

Klucze to `sda:` i `scl:` (nie `sda_pin:`). Opcja `internal_pullup: true` jest domyslne, ale
warto ja wpisac jawnie. Nie ma opcji `sda_input_mode:` / `scl_input_mode:` - ich obecnosc w YAMLu
konczy sie bled walidacji, a ich brak nie szkodzi.

W ESPHome 2026.9+ sterowniki SHT/DHT wystawiaja**Dlaczego kod przelacza tryby zamiast pisac wartosci.** `pinMode(pin, OUTPUT)` to tu
`gpio_config(id, GMODE_OUTPUT)`, a `GMODE_OUTPUT` zapisuje w rejestrze pinu wartosc `0x00`:
kierunek „wyjscie" z wyzerowanym bitem stanu (`GCFG_OUTPUT_POS 1`). Samo przełączenie w OUTPUT
startuje wiec od **stanu niskiego**. Open-drain emulujemy kierunkiem, nie wartoscia:
`drive_sda_low_()` = latch w 0 i `pin_mode(FLAG_OUTPUT)`, `release_sda_()` = `pin_mode(FLAG_INPUT |
FLAG_PULLUP)`. Sciezki „wyjscie i wysoki stan" nie ma - „OUTPUT i zapisz 1" na magistrali z
pull-upem to zwarcie do VCC w dziewiatym takcie.

Druga pulapka tego samego typu: `digitalWrite()` na pinie, ktory aktualnie **nie** jest wyjsciem,
nie zapisuje latcha, tylko przelacza tryb (`pinSetOutputPull` w `wiring_private.h`:
`pinMode(pinNumber, INPUT_PULLDOWN ^ !!status); return;`). Stad kolejnosc w `drive_sda_low_()`:
`digital_write(false)` przenosi pad w INPUT_PULLDOWN (0x2C), a `pin_mode(FLAG_OUTPUT)` domyka go w
OUTPUT(0x00) - bez przejazdu przez stan wysoki.

Filtr JTAG (`gpio_ops_filter`) ignoruje odczyty i zapisy tylko dla GPIO20..23, a LibreTiny i tak
wywoluje `gpio_ops_disable_filter()` przy starcie (`cores/beken-72xx/base/fixups/sys_ctrl.c`), wiec
nawet one dzialaja - ale to piny JTAG, na magistrale wez inne. SDK nie podaje rezystancji
wewnetrzego pull-upu (jest sam bit zalaczania), wiec przy 100 kHz i dluzszych przewodach daj
zewneczne 4.7k-10k do VCC.

## Diagnostyka

`setup()` nadaje jedna linie, ktora rozstrzyga, czy winny jest pad, czy urzadzenie:

```
[I][soft_i2c:3xx]: Line probe: idle SDA=1 SCL=1, driven-low SDA=1 SCL=1, scan enabled
```

| Widok | Interpretacja |
|---|---|
| `idle SDA=1 SCL=1` | pad reaguje, pull-up dziala, linie nie sa zwarte do masy |
| `idle SDA=0` albo `SCL=0` | linia trzymana w dol: zwarcie, brak pull-upu albo zly pin |
| `driven-low SDA=0 SCL=0` | wychodzace stejrunki dociagaja w dol (ale na BK72xx patrz uwaga pod tabela) |
| `driven-low SDA=1` (albo SCL=1) | na BK72xx **nie dowodzi niczego** - odczyt pinu bedacego wyjsciem zwraca rejestr danych (latch), nie stan padu |
| `scan disabled` | w tym buildzie nie ma `scan: true`, wiec log nie pokazze adresu urzadzenia, nawet gdy odpowiada |

> **Uwaga o polowie `driven-low`.** LibreTiny czyta pin przez `GPIO_Read()` -> `GPIO_RegType.data`, a gdy pad jest wyjsciem, ten rejestr jest **rejestrem danych wychodzacych**. Czyli `driven-low SDA=0` moge zobaczyc nawet na padzie, ktory fizycznie nic nie widzi. Rozstrzygajacy jest odczyt **idle** (obie linie puszczone MUSZA byc 1 - to ścieżka wejściowa, taki odczyt nie może kłamać) oraz skan. Jeśli idle jest OK, a skan pusty, a chcesz miec pewnosc co do wychodzacej sciezki: zaloz zworkę miedzy SDA i SCL (TX1 <-> RX1) i odpal probe ponownie - sterujac jedna linia w dol, druga (jako wejście) musi pokazac 0. To test padu bez logic analyzer.

Potem, przy pierwszym bledzie transakcji (pierwszy logowany jest zawsze, nastepne nie czesciej niz
raz na sekunde):

```
[W][soft_i2c:3xx]: I2C transaction failed: error=2 addr=0x44 sda=1 scl=1 (total failures: 1)
```

Kody z `i2c::ErrorCode`: `2` brak ACK (urzadzenie nie odpowiada - adres, zasilanie, inny uklad),
`3` timeout (SCL nie wchodzi w gore - stretching albo brak pull-upu), `4` magistrala nie
zainicjalizowana, `7` CRC. Poziomy `sda`/`scl` w tej samej linii mowia, czy w momencie bledu linia
byla fizycznie gorna.

l_input_mode: ANALOG` to nie dekoracja.** Na BK72xx
ustawienie pinu jako `INPUT` z wlaczonym pull-upem (`gpio_dma_pe(0)`) powoduje dryf
stanu wejscia i gubione zbocza. Sciezka domyslna (`PULLUP`) jest poprawna sprzetowo,
ale na tej platformie zawodna - stad w przykladach ANALOG plus rezystory 4.7 kOhm do VCC.

## Jak to dziala

Klasa implementuje `i2c::I2CBus` - ten sam interfejs, ktory spelnia natywne `i2c-hard`.
Kazdy sterownik dziedziczacy po `i2c::I2CDevice` (`sht3xd`, `bme280`, `bh1750` i tak
daljej) dziala bez zmian, bo dostaje `I2CBus&`.

- Start, stop, restart: klasyczne sekwencje na linii pojedynczej.
- Odczekanie ACK: SDA puszczona na dziewiatym takcie, potem puszczajace SCL (timeout SMBus).
- Wykrycie zakleszczenia: SDA niski przed START - rusza standardowa procedura odzysku
  z ESPHome (dziewiec taktow plus STOP).
- Timeout na czekaniu na zwolnienie SDA (okolo 10 ms) - brak ACK nie zapetla petli.
- Czas: 100 kHz wyliczane z `micros()`; realnie wychodzi okolo 40 kHz przez narzut
  wolania GPIO. Za malo na SMBus, za malo na probe 400 kHz.

## Uwagi eksploatacyjne

- **Flashowanie po UART1 dziala mimo I2C na TX1/RX1.** Stan bezczynnosci magistrali
  (obie linie wyciagniete w gore) to stan bezczynnosci UART. Wejscie w bootloader
  wymaga zwarcia GPIO8 (REC/BOOT) do GND przy rescie.
- **Zasilanie modulu.** CBU na slabych LDO (AS1117) zapada sie przy nadawaniu WiFi:
  spadek napiecia resetowal modul w polowie pierwszego odczytu. Zasilanie 5 V z
  zasilacza problem eliminuje. Objaw: brak logow "zadnych", jakby urzadzenie nie startowalo.
- **Zapis do flasha wylacza przerwania.** `save_state` (preferencja
  `flash_write_interval`) blokuje przerwania na okolo 20 ms - w tym oknie bit-bang moze
  pogubic bity. Przy `update_interval: 15s` to jeden zly odczyt na setki; przy 1 s czesciej.
- **Wyacznie jeden master** na magistrali. Brak CLK stretching, brak multiplekserow
  (`tca9548`), brak `i2c_scan` i `i2c_restart_dump` (dzialaja tylko z natywnym `i2c`).
- Magistrala bez rezystorow (wejsciowy stan dryfujacy) da `recovery failed` w logu i
  czujnik bedzie niewidoczny - to bledna konfiguracja, nie usterka komponentu.

## Pliki

| Plik | Rola |
|---|---|
| `components/soft_i2c/__init__.py` | schemat YAML, rejestracja magistrali i pinow |
| `components/soft_i2c/soft_i2c.h` | deklatacja `SoftI2CBus` |
| `components/soft_i2c/soft_i2c.cpp` | bit-bang: START, stop, bajt, ACK, odzysk |
| `example.yaml` | pelny projekt na CBU + SHT3xD |

Zgloszenia i pytania: przez Issues. Chetnie poprawie pod inne uklady BK7231, jezeli
ktos podesle logi.

## Device Builder (praca w Home Assistant)

Ten komponent da sie uzyc bez CLI i bez repozytorium z konfiguracja: w Home
Assistantie dodatek **ESPHome -> Device Builder** tworzy urzadzenie, a w polu
`external_components` wklejasz:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/codekaczmarek/soft-i2c.git
      ref: v1.0.0
```

Repo jest publiczne, wiec nie potrzeba zadnych token6w. Sekrety (SSID, haslo WiFi,
haslo OTA, klucz API) trafiaja do `secrets.yaml` w katalogu dodatka, nie do YAML-a
urzdzenia. Binariow z Release zwykle nie uzywasz - Device Builder buduje firmware z
Twojej konfiguracji, z Twoim kluczem API.

## CI/CD

Repo ma pipeline w `.github/workflows/ci.yml` (dziala bez zadnych sekretow):

| Job | Co robi |
|---|---|
| `secret-scan` | gitleaks: drzewo + cala historia, plus kontrola negatywna i skan uzupelniajacy |
| `workflow-audit` | `actionlint` (skladnia) + `zizmor` (bezpieczenstwo workflow) |
| `validate` | `esphome config` na `example.yaml` i `test/ci-config.yaml` (macierz: 2026.9.1 i `latest`) |
| `compile` | pelna kompilacja CBU/BK7231N, artefakty `.bin` i `.ota.rbl` na kazdym PR |
| `release` | tylko na tagu `v*`: tworzy Release z binariami |

Wydanie: `git tag v1.0.1 && git push origin v1.0.1`.

Akcje sa przypiete do pelnych commitow (nie tagow `@v3`), a gitleaks jest pobierany
z weryfikacja `sha256sum`. Swiadomie nie ma `actions/cache`: cache zapisywany przy
`pull_request` z forka to powierzchnia ataku (cache poisoning).

## Sekrety

Repozytorium **nie moze** zawierac sekretow - ani w drzewie, ani w historii. Dotyczy
to takze wartosci pozorna nieszkodliwych: hasel WiFi i OTA, kluczy API, tokenow.
Konfiguracja testowa (`test/ci-config.yaml`) uzywa wylacznie wartosci fikcyjnych
(`CI-NETWORK`), a `example.yaml` pobiera dane z `secrets.yaml`, ktory jest w `.gitignore`.

Bramka `secret-scan` odpala najpierw **kontrole negatywna**: sadzi syntetyczny token
w katalogu tymczasowym i failuje build, jesli skaner go nie znajdzie. Dlaczego: w
gitleaks >= 8 podanie pliku konfiguracyjnego (nawet z samym allowlistem) **zastepuje**
reguly domyslne, wiec zle skonfigurowany skaner zgloszy `no leaks found` na repo z
naprawde skradzionym tokenem. Reguly domyslne nie znaja ponadto formatu `github_pat_`
(fine-grained PAT) - stad krok uzupelniajacy na calej historii.

Jezeli sekret i tak wycieknie (np. zostal wkompilowany w Release): natychmiast
uniewaznij go w zrodle, a nie "usun commita" - historia i forkow zostaly.
