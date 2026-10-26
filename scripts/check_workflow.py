#!/usr/bin/env python3
"""Kontrola struktury .github/workflows/ci.yml.

actionlint i zizmor pilnuja skladni i bezpieczenstwa workflowa, ale nie wiedza, ktore joby
MUSZA w nim byc. Ten skrypt pilnuje akurat tego, bo taki blad juz sie zdarzyl: wstawianie
nowego joba metoda index()/replace() nadgryzlo sasiadni job "secret-scan" (skan sekretow), a
GitHub odrzucil plik, bo "compile" wskazywal needs: na joba, ktory zniknal. Zielony przebieg
actionlint nic by tu nie powiedzial - plik nie istnial jako poprawny workflow.

Sprawdza:
  1) oczekiwany zbior jobow jest obecny (nic nie zniknelo),
  2) kazdy "needs" wskazuje na job, ktory istnieje,
  3) kazdy step ma "uses" albo "run" (pusty krok = cichy brak pracy),
  4) kazde "run" przechodzi "bash -n" (skladnia powloki),
  5) kroki pobierajace akcje third-party sa przypiete do SHA (komentarz z wersją).

Blad = exit 1 z komunikatem "::error::" widocznym w UI GitHuba.
"""
import re
import subprocess
import sys
import tempfile
import os
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover
    print("::error::Brak PyYAML - zainstaluj 'pip install pyyaml' przed uruchomieniem skryptu.")
    sys.exit(1)

ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"

# Joby, ktore MUSZA istniec. Kazdy z nich pilnuje czego innego i utrata ktorego kolwiek
# oznaczalaby cichy regres w jakosci wydania.
REQUIRED_JOBS = {
    "version-consistency",  # VERSION == makra == tag
    "secret-scan",          # gitleaks: drzewo + cala historia
    "workflow-audit",       # actionlint + zizmor
    "validate",             # esphome config na pinowanej i najnowszej wersji
    "compile",              # pelny build firmware dla CBU/BK7231N
    "compile-legacy",       # ten sam build na ESPHome 2026.9.1 - pilnuje sciezki to_code() dla starszego API
    "release",              # publikacja Release na tagu v*
}

# Akcje third-party muszą byc pinowane po SHA (zizmor flaguje floating tags, ale nie sprawdza,
# czy komentarz z wersja istnieje - tu tylko kontrola, ze SHA jest 40-znakowy).
USES_RE = re.compile(r"^\s*(?P<owner>[\w.-]+)/(?P<repo>[\w.-]+)@(?P<sha>[0-9a-f]{40})\s*(?:#\s*\S+)?\s*$")


def err(msg: str) -> None:
    print(f"::error::{msg}")


def main() -> int:
    if not WORKFLOW.exists():
        err(f"Brak pliku {WORKFLOW}")
        return 1

    try:
        data = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    except yaml.YAMLError as exc:
        err(f"Workflow nie parsuje sie jako YAML: {exc}")
        return 1

    jobs = (data or {}).get("jobs") or {}
    problems = 0

    missing = REQUIRED_JOBS - set(jobs)
    if missing:
        err(f"W workflow brakuje jobow: {sorted(missing)}. Ktos je usunal albo nadgryzl przy edycji.")
        problems += 1
    extra = set(jobs) - REQUIRED_JOBS
    if extra:
        # Nie blad - swiadomie nowy job. Mowimy o tym, zebysmy nie przeoczyli literowki w nazwie.
        print(f"::notice::Nowe joby poza lista oczekiwana: {sorted(extra)}")

    for name, job in jobs.items():
        needs = job.get("needs") or []
        if isinstance(needs, str):
            needs = [needs]
        for dep in needs:
            if dep not in jobs:
                err(f"Job '{name}' ma needs: '{dep}', a taki job nie istnieje (GitHub odrzuci caly workflow).")
                problems += 1

        steps = job.get("steps") or []
        if not steps:
            err(f"Job '{name}' nie ma zadnych krokom.")
            problems += 1
        for i, step in enumerate(steps, 1):
            if not step.get("uses") and not step.get("run"):
                err(f"Job '{name}', krok {i}: brak 'uses' i brak 'run'.")
                problems += 1
            if step.get("uses"):
                if not USES_RE.match(str(step["uses"]).strip()):
                    err(f"Job '{name}', krok {i}: akcja '{step['uses']}' nie jest przypieta do 40-znakowego SHA.")
                    problems += 1
            if "run" in step:
                with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as fh:
                    fh.write(step["run"])
                    path = fh.name
                try:
                    res = subprocess.run(["bash", "-n", path], capture_output=True, text=True)
                finally:
                    os.unlink(path)
                if res.returncode:
                    err(f"Job '{name}', krok {i} ({step.get('name', 'bez nazwy')}): bash -n -> {res.stderr.strip()}")
                    problems += 1

    if problems:
        err(f"Kontrola workflow: {problems} problem(ow).")
        return 1
    print(f"Kontrola workflow OK: {len(jobs)} jobow, wszystkie needs rozwiazywalne, kroki niepuste, run przechodza bash -n.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
