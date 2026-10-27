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
      ref: v1.4.0
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

W ESPHome 2026.9+ sterowniki SHT/DHT wystawiaja dwa narzedzia, wiec nazwy ida do
zagniezdzonych blokow `temperature:` / `humidity:` - plytki klucz `name:` zostal
odrzucony przez walidator.

**Dlaczego kod przelacza tryby zamiast pisac wartosci.** `pinMode(pin, OUTPUT)` to tu
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

Filtr JTAG (`gpio_ops_filter` w `beken378/driver/gpio/gpio.c`) ignoruje odczyty i zapisy dla
GPIO20..23 i **w ESPHome jest zawsze wlaczony**: `gpio_ops_disable_filter()` jest wywolywany w
calym stosie tylko w jednym miejscu - `sctrl_enter_rtos_deep_sleep_fix()`, dokad trafia sie
przez `lt_deep_sleep_enter()`, czyli przez konfiguracje `deep_sleep:`. Build bez `deep_sleep:`
(taki jak diagnostyka na stole) nigdy tej sciezki nie wchodzi, wiec kazdy `gpio_config()` i
`gpio_output()` na GPIO20..23 jest cicho polykany: pad nie drgnie, brak bledu, transakcja konczy
sie timeoutem. `setup()` soft_i2c zdejmuje ten filtr sam (`jtagfilter=1` w linii diagnostycznej)
i dopiero wtedy P20/P22 - piny, na ktorych czujnik CHT8305 wisi fizycznie - da sie obsluzyc.
Rezerwa bez filtra: linie JTDI/JTDO/JTCK/JTMS, czyli programator. `CFG_JTAG_ENABLE` jest i tak 0.
SDK nie podaje rezystancji
wewnetrzego pull-upu (jest sam bit zalaczania), wiec przy 100 kHz i dluzszych przewodach daj
zewneczne 4.7k-10k do VCC.

## Diagnostyka

`dump_config()` nadaje **jedną linię** (na BK72xx logi z `setup()` potrafią zaginąć, dlatego wszystko siedzi w `dump_config` - tam dociera zawsze):

```
[C][soft_i2c:047]: soft_i2c v1.4.0 | bus SDA=22 SCL=20 100000Hz internal-pullup timeout=10000us | setup=1 | probe idle=1/1 driven=1/1 | loop=1/1 | cfgdrv=0x000A/0x000A | scan=1 found=1 first=0x40 | errors=0 last_err=0@0x00 jtagfilter=1
```

| Pole | Co znaczy |
|---|---|
| `bus SDA=22 SCL=20` | numery GPIO, nie numery pinów modułu - P22 to GPIO22 |
| `setup=1` | `setup()` przeszedl do konca (`0` = wyjatek wewnatrz, reszta pól jest bezwartosciowa) |
| `probe idle=1/1` | obie linie puszczone sa w gorze (pull-up dziala, brak zwarcia do masy). `0` po którejś stronie = zwarcie, brak pull-upu albo zly pad |
| `driven=1/1` | pin czytany **jako wyjscie** pokazał 0 po sterowaniu w dol. Na BK72xx **nie dowodzi niczego** - patrz uwaga pod tabela |
| `loop=1/1` | test zwory: sterowalem SDA w dol i **SCL czytana jako wejście** pokazala 0 (oraz odwrotnie). `0/0` przy zworze = wychodzaca sciezka padu nie dziala |
| `cfgdrv=0x000A/0x000A` | surowy `REG_GPIO_n_CONFIG` padu zmierzony w trakcie sterowania w dol. Bity: 0=INPUT, 1=OUTPUT, 2=INPUT_ENABLE, 3=OUTPUT_ENABLE, 4/5=pull. `0x000A` = OUTPUT+OUTPUT_ENABLE. `0x0000` = zapis SDK nie dolaczyl |
| `scan=1 found=1 first=0x40` | skan magistrali byl wlaczony (`scan: true`) i znaleziono jeden adres |
| `errors=1 last_err=2@0x40` | liczba bledow transakcji i ostatni blad: `2` = NACK (nikt nie odpowiada), `3` = timeout (SCL nie wchodzi w gore), `4` = magistrala nie zainicjalizowana, `7` = CRC |
| `jtagfilter=1` | `gpio_ops_disable_filter()` zostal wywolany. **Nie** znaczy, ze pady 20..23 sa wolne - patrz sekcja CHT83xx |

> **Uwaga o `driven=`.** LibreTiny czyta pin przez `GPIO_Read()`, a gdy pad jest wyjsciem, ten
> odczyt zwraca **rejestr danych wychodzacych**, nie stan padu. Dlatego `driven=0/0` moze oznaczac
> zarowno "pad nie dociaga", jak i "nic z tego nie wynika". Rozstrzygajace sa dwa pola: `loop=`
> (odczyt drugiej linii jako wejscie, wymaga zwory miedzy SDA i SCL) oraz `cfgdrv=` (rejestr padu
> pod obciazeniem). Rozstrzygniecie: `cfgdrv` ma bity OUTPUT+OUTPUT_ENABLE, a `loop` wciaz `0/0`
> => zapis wszedl, ale padem rzadzi mux innego bloku (JTAG/flash - GPIO20/22 to TCK/TDI i FSCK/FSI).
> `cfgdrv=0x0000` => zapis nie dolaczyl i przyczyny trzeba szukac wyzej w łańcuchu.

Potem, przy pierwszym bledzie transakcji (pierwszy logowany jest zawsze, nastepne nie czesciej niz
raz na sekunde):

```
[W][soft_i2c:283]: I2C transaction failed: error=2 addr=0x44 sda=1 scl=1 (total failures: 1)
```

Kody z `i2c::ErrorCode`: `2` brak ACK (urzadzenie nie odpowiada - adres, zasilanie, inny uklad),
`3` timeout (SCL nie wchodzi w gore - stretching albo brak pull-upu), `4` magistrala nie
zainicjalizowana, `7` CRC. Poziomy `sda`/`scl` w tej samej linii mowia, czy w momencie bledu linia
byla fizycznie gorna.

l_input_mode: ANALOG` to nie dekoracja.** Na BK72xx
ustawienie pinu jako `INPUT` z wlaczonym pull-upem (`gpio_dma_pe(0)`) powoduje dryf
stanu wejscia i gubione zbocza. Sciezka domyslna (`PULLUP`) jest poprawna sprzetowo,
ale na tej platformie zawodna - stad w przykladach ANALOG plus rezystory 4.7 kOhm do VCC.

### Tryby diagnostyczne (wlaczane opcjonalnie)

| Opcja | Domysl | Co robi i kiedy jej uzyc |
|---|---|---|
| `stretch_timeout` | `120ms` | Cierpliwosc na rozciagany SCL. Nie mylic z `timeout` (ten jest na takt bitu i ACK). Zwieksza sie ja dla czujnikow z dlugimi konwersjami, zmniejsza zeby skrocic boot przy martwej magistrali |
| `pad_census` | `[]` (wyl.) | Konfiguruje kazdy pad z listy jako wejscie z pull-upem, potem z pull-downem, i raportuje odczyt. Pad czytany 0 **w obu** konfiguracjach = trwale przywiazyany do masy, czyli przycisk. **Diody LED nie znajdzie**: prog ~1.8 V znaczy brak pradu przy pull-downie rzadu dziesiatek kOhm, wiec pad z dioda wyglada jak wiszacy. Po probie kazdy pad wraca do wejscia bez rezystorow |
| `protocol_probe` | `false` | Wysyla surowe sekwencje protokolu (SHT3x repeated-start i stop-start, SHT4x, CHT8305-style na rejestrach), loguje surowe bajty i werdykt CRC8 (Sensirion poly 0x31). Sluzy do rozstrzygniecia "skan ACK-uje, a sterownik nie chce czytac" |
| `protocol_probe_addresses` | `[0x44]` | Adresy do sondy |

> **Sonde odpala sie PO skanie, nie przed nia.** `protocol_probe` wysyla realna komende pomiaru,
> a ona rozciaga SCL. Odpalona przed skaniem zatrzaskuje magistrale i skan zglosi `found=0` na
> sprawnym ukladzie. Ten scenariusz kosztowal pol dnia blednego wniosku "czujnik nie zyje", bo
> `errors=12 last_err=3@0x44` wyglada dokladnie jak uklad martwy.

## Sterownik CHT83xx (CHT8305 / CHT8310)

Moduly T&H na CBU (Tuya, niebieska PCB) maja na pokladzie **Sensylink CHT8305**, nie SHT3xD.
Wyglada jak SHT3x (I2C, temp+hum), ale mowi innym jezykiem: 8-bitowy wskaznik rejestru i
16-bitowe wartosci big-endian, **bez CRC**. `sht3xd` wysyla za to slowa komend Sensiriona z
CRC-8 i konczy na `mark_failed("Failed to set heater mode")` - CHT83xx nie ma grzejki, a slowo
`0x3066` nie istnieje w jego tablicy komend. Stad drugi komponent w repo, `cht83xx`.

Mape pinow i adres potwierdza watek elektroda o tym samym sprzecie (dzialajacy OpenBeken):
**SDA = GPIO22, SCL = GPIO20**, adres **0x40** (AD0 do GND) - nie 0x44 ani 0x70.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/codekaczmarek/soft-i2c.git
      ref: v1.4.0

i2c: []

soft_i2c:
  - id: bus_cht
    sda: P22
    scl: P20
    frequency: 100kHz
    scan: true

sensor:
  - platform: cht83xx
    i2c_id: bus_cht
    address: 0x40
    update_interval: 60s
    temperature:
      name: Temperatura
    humidity:
      name: Wilgotnosc
```

Adres zalezy od AD0: GND `0x40`, VCC `0x41`, SDA `0x42`, SCL `0x43`. Jezeli `dump_config`
pokazuje `manufacturer=0x0000`, czujnik nie odpowiada - sprawdz adres, piny i pull-upy.

Wariant jest wykrywany z rejestru wersji: `0x8215` / `0x8315` to rodzina CHT831x, ktora wymaga
trigera `oneshot` (rejestr `0x0F`) i osobnego odczytu wilgotnosci z rejestru `0x01`, bo wspólne
okno odczytu `0x00..0x03` trafia tam na bit parzystosci ALERT. Konwersje:

| Wariant | Temperatura | Wilgotnosc |
|---|---|---|
| CHT8305 | `raw * 165/65535 - 40` | `raw * 100/65535` |
| CHT831x | `(raw >> 3) * 0.03125` | `(raw & 0x7FFF) / 32768 * 100` |

Zrodla protokolu: `drv_cht8305.c` z OpenBK7231T_App (sterownik utrzymywany dla tego sprzetu) i
RobTillaart/CHT8305 (mapa rejestrow z datasheetu: `0x00` TEMP, `0x01` HUM, `0x02` STATUS,
`0x03` CFG, `0x04` C_RATE, `0xFE` manufacturer = `0x5959`, `0xFF` wersja).

## Jak to dziala

Klasa implementuje `i2c::I2CBus` - ten sam interfejs, ktory spelnia natywne `i2c-hard`.
Kazdy sterownik dziedziczacy po `i2c::I2CDevice` (`sht3xd`, `bme280`, `bh1750` i tak
daljej) dziala bez zmian, bo dostaje `I2CBus&`.

- Start, stop, restart: klasyczne sekwencje na linii pojedynczej.
- Odczekanie ACK: SDA puszczona na dziewiatym takcie, potem puszczajace SCL (timeout SMBus).
- **Czysty odczyt nie moze wyslac slowa adresu jako WRITE.** ESPHome deleguje
  `I2CBus::read()` do `write_readv(address, nullptr, 0, buffer, len)`, wiec sterownik musi
  rozpoznac `write_count == 0` i wyslac adres z bitem READ. Bez tego transakcja konczy sie
  `ERROR_OK`, zero NACK-ow, zero timeoutow, a dane sa smieciowe. Dlatego objaw "skan widzi
  urzadzenie, a sterownik pisze *Retrying to reconnect the sensor*" **nie** dowodzi uszkodzenia
  sprzetu. Test rozstrzygajacy: odczyt numeru seryjnego (`Serial Number:` niezerowe).
- **Rozciaganie zegara (clock stretching).** Kazde czekanie na SCL dostaje limit jako parametr
  wywolania, nie wspolne pole: takt bitu potrzebuje ~10 ms, a oczekiwanie az slave skonczy
  konwersje (SHT3x ~8.5 ms) potrzebuje ~120 ms. Jeden wspolny limit daje albo zatrzask
  (przerwanie w srodku konwersji, a STOP nie wychodzi, bo STOP wymaga SCL w gorze), albo boot
  trwajacy pol minuty przy kazdym bledzie.
- **Odzysk po zakleszczeniu: dziewiec taktow SCL bezwarunkowo**, potem STOP, a bilans
  (`read_sda_` i `read_scl_`) mowi, co wisialo. Wersja `if (SDA wysoka) return OK;` lecyla tylko
  magistrale zdrowa - przy zatrzasku SDA jest wlasnie wysoka. Wersja `czekaj na SCL, potem taktuj`
  czekala do konca limitu i nigdy nie taktowala, a slave czekajacy na krawedz zegara nie dostaje
  jej od czekania.
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
- **Wyacznie jeden master** na magistrali. Brak multiplekserow
  (`tca9548`), brak `i2c_scan` i `i2c_restart_dump` (dzialaja tylko z natywnym `i2c`).
- Magistrala bez rezystorow (wejsciowy stan dryfujacy) da `recovery failed` w logu i
  czujnik bedzie niewidoczny - to bledna konfiguracja, nie usterka komponentu.
- **Zatrzasniety slave trzyma SCL w zerze, a reboot procesora tego nie leczy.** Rozpoznaje sie po
  `probe idle=x/0` (SCL w dole przy puszczonej linii) oraz po `scl_stuck` rosnacym w
  nieskonczonosc, podczas gdy rejestr pada SCL to `0x3C`, czyli wejscie z pull-upem - linii nie
  trzymamy my. Bezwarunkowe taktowanie uwalnia slave'a w wiekszosci przypadkow (tak bylo na
  SHT30/CBU: `scl_stuck` ze 131 do 1 i `found=1 first=0x44` w nastepnym boocie). Jesli nie uwolni,
  trzeba mu **odlaczyc zasilanie** - reset MCU resetuje tylko mastera.

## Pliki

| Plik | Rola |
|---|---|
| `components/soft_i2c/__init__.py` | schemat YAML, rejestracja magistrali i pinow |
| `components/soft_i2c/soft_i2c.h` | deklatacja `SoftI2CBus` |
| `components/soft_i2c/soft_i2c.cpp` | bit-bang: START, stop, bajt, ACK, odzysk |
| `components/cht83xx/__init__.py` | docstring sterownika CHT83xx (adresy AD0, protokol) |
| `components/cht83xx/sensor.py` | schemat YAML platformy `sensor: cht83xx` |
| `components/cht83xx/cht83xx.h` | deklatacja `CHT83XXComponent` |
| `components/cht83xx/cht83xx.cpp` | odczyt rejestrow, wykrywanie wariantu, konwersje |
| `example.yaml` | pelny projekt na CBU + CHT83xx (fabryczna płytka Tuya T&H) |
| `VERSION` | zrodlo prawdy dla numeru releases (patrz sekcja Wersjonowanie) |
| `scripts/check_version.sh` | bramka CI: VERSION == makra == nazwa tagu |
| `scripts/check_workflow.py` | bramka CI: oczekiwane joby, rozwiazywalne `needs`, niepuste kroki |

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
      ref: v1.4.0
```

Repo jest publiczne, wiec nie potrzeba zadnych token6w. Sekrety (SSID, haslo WiFi,
haslo OTA, klucz API) trafiaja do `secrets.yaml` w katalogu dodatka, nie do YAML-a
urzdzenia. Binariow z Release zwykle nie uzywasz - Device Builder buduje firmware z
Twojej konfiguracji, z Twoim kluczem API.

### Diagnostyka padow (`soft_i2c pads`)

Druga linia logu `soft_i2c` pokazuje zawartosc rejestru konfiguracyjnego padu (`GPIO_BASE_ADDR + n*4`)
i dwie kontrole przyrzadu, zeby zer nie bylo interpretowane na oko:

| Pole | Znaczenie |
|------|-----------|
| `pins=22/20` | numery GPIO realnie uzyte do odczytu. `-1` = numer nie zostal odczytany, wowczas **caly cfgdrv jest zerem z definicji i nic nie znaczy** |
| `cfgidle=x/y` | rejestr w stanie idle (wejscie z pullupem). Non-zero = `gpio_config()` tu dotarl |
| `cfgdrv=x/y` | rejestr w momencie sterowania w dol. Brak bitow OUTPUT/OUTPUT_ENABLE przy sprawnym `pins` = zapis nie dociera do padu |
| `ctl(uart2)=x` | pad GPIO0, ktorego nie konfiguruje soft_i2c, tylko build. **Zero tutaj uniewaznia caly odczyt** — znaczy, ze przyrzad mierzy zle, nie ze pad jest zepsuty |
| `ref(free)=x` | pad nieuzywany, baseline |

Wnioskowanie po tej linii, w tej kolejnosci: najpierw `pins` (czy w ogole mierzymy wlasciwy pad),
potem `ctl` (czy przyrzad dziala), dopiero potem `cfgidle`/`cfgdrv`. Odwrotna kolejnosc prowadzi do
fałszywych wnioskow — zero z parsowania i zero z padu wygladaja identycznie.

## Wersjonowanie

Numer releases bierze sie z pliku `VERSION` w glownym katalogu. Ten sam numer musza miec makra
`SOFT_I2C_VERSION` (`components/soft_i2c/soft_i2c.h`) i `CHT83XX_VERSION`
(`components/cht83xx/cht83xx.h`) - oba trafiaja do logu urzadzenia, wiec wlasnie one lacza linie
`soft_i2c v...` z konkretnym Release i konkretnym commitem.

Podnoszac wersje zmien w jednym commicie trzy rzeczy: `VERSION`, oba makra i przyklady `ref:`
w tym README. Nazwe tagu pilnuje CI (`scripts/check_version.sh`, job "Spojnosc wersji"): `v1.4.0`
przechodzi tylko z `VERSION` = `1.4.0`, wiec otagowanie commita z innym numerem daje czerwony job
i blokuje Release.

Historia: przez kilka wydania tag (`v1.3.x`) i `SOFT_I2C_VERSION` (`1.2.x`) rozjezdzaly sie ze
sobą, wiec log z urzadzenia nie mowil, ktory obraz jest wgrany - i trudno bylo udowodnic, ktora
wersja filtru JTAG faktycznie kreci sie na sprzecie.

## CI/CD

Repo ma pipeline w `.github/workflows/ci.yml` (dziala bez zadnych sekretow):

| Job | Co robi |
|---|---|
| `version-consistency` | `scripts/check_version.sh`: `VERSION` == `SOFT_I2C_VERSION` == `CHT83XX_VERSION` == nazwa tagu |
| `secret-scan` | gitleaks: drzewo + cala historia, plus kontrola negatywna i skan uzupelniajacy |
| `workflow-audit` | `actionlint` (skladnia) + `zizmor` (bezpieczenstwo workflow) + `scripts/check_workflow.py` (oczekiwane joby, `needs`, kroki) |
| `validate` | `esphome config` na `example.yaml` i `test/ci-config.yaml` (macierz: 2026.9.1 i `latest`) |
| `compile` | pelna kompilacja CBU/BK7231N na `ESPHOME_VERSION` (2026.10.0, wersja Device Buildera), artefakty `.bin` i `.ota.rbl` na kazdym PR |
| `compile-legacy` | ten sam build na ESPHome 2026.9.1 - `esphome config` nie wykonuje `to_code()`, wiec tylko realny build lapie API nieistniejace w starszych wersjach |
| `release` | tylko na tagu `v*`: tworzy Release z binariami; wymaga zielonego `version-consistency` |

Wydanie (w tej kolejnosci): podnies `VERSION` i oba makra w jednym commicie, otaguj **ten** commit
(`git tag v1.4.0 && git push origin v1.4.0`). Tag z innym numerem niz `VERSION` dostaje czerwony
`version-consistency` i nie publikuje Release.

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
