#!/usr/bin/env bash
# Bramka spojnosci wersji dla repozytorium soft-i2c.
#
# Zrodlem prawdy jest plik VERSION w glownym katalogu. Ten skrypt wywala sie, jesli:
#   1) VERSION nie ma formatu X.Y.Z,
#   2) ktorekolwiek makro SOFT_I2C_VERSION / CHT83XX_VERSION rozni sie od VERSION,
#   3) CI leci na tagu v*, ktorego numer nie rowna sie VERSION.
#
# Powstalo po konkretnym incydencie: tag wskazywal v1.3.3, a SOFT_I2C_VERSION w kodzie mial
# "1.2.7". Log z urzadzenia ("soft_i2c v1.2.7 ...") nie dal sie wtedy przypisac do commita ani
# do Release, wiez nie do sprawdzenia - i runde debugowania na sprzecie kosztowalo dokladnie to,
# a nie sam blad. "Prosze pamietac" nie dziala; dziala zielony job.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

if [ ! -f VERSION ]; then
  echo "::error::Brak pliku VERSION - to on jest zrodlem prawdy dla numeru releases."
  exit 1
fi

v=$(tr -d '[:space:]' < VERSION)
if ! printf '%s\n' "$v" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
  echo "::error::VERSION ma niepoprawny format (oczekiwane X.Y.Z): '$v'"
  exit 1
fi
echo "Wersja releases (VERSION): $v"

# para "sciezka=makro" dla kazdego komponentu, ktory raportuje wersje w logu
for pair in \
  "components/soft_i2c/soft_i2c.h=SOFT_I2C_VERSION" \
  "components/cht83xx/cht83xx.h=CHT83XX_VERSION"
do
  f=${pair%%=*}
  m=${pair##*=}
  if [ ! -f "$f" ]; then
    echo "::error::Brak pliku $f"
    exit 1
  fi
  got=$(sed -n "s/^#define ${m}[[:space:]]*\"\([^\"]*\)\".*$/\1/p" "$f")
  if [ -z "$got" ]; then
    echo "::error::W pliku $f brak makra ${m} w formie #define ${m} \"X.Y.Z\""
    exit 1
  fi
  if [ "$got" != "$v" ]; then
    echo "::error::ROZJAZD: $f definiuje ${m}=\"$got\", a VERSION=\"$v\". Podnies oba w jednym commicie."
    exit 1
  fi
  echo "OK: $f -> ${m} = $got"
done

# Na tagu release musi odpowiadac kodowi, ktory publikujemy.
case "${GITHUB_REF:-}" in
  refs/tags/v*)
    tag=${GITHUB_REF#refs/tags/v}
    if [ "$tag" != "$v" ]; then
      echo "::error::ROZJAZD: tag to v${tag}, a VERSION to ${v}. Taguj commit, ktory zawiera ten VERSION."
      exit 1
    fi
    echo "OK: tag v${tag} odpowiada VERSION"
    ;;
esac

echo "Wersje spojne."
