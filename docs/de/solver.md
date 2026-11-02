**Sprachen**: [English](../solver.md) | [简体中文](../zh-CN/solver.md) | [繁體中文](../zh-TW/solver.md) | [日本語](../ja/solver.md) | [한국어](../ko/solver.md) | [Français](../fr/solver.md) | [Deutsch](solver.md) | [Español](../es/solver.md) | [Italiano](../it/solver.md) | [Русский](../ru/solver.md) | [العربية](../ar/solver.md)

[← Dokumentationsindex](README.md)

# Bitvektor-Beweisbackends

Standardmäßig verwendet NeverD seinen eingebauten Bitvektorsolver. Exakte MBA-Ableitung bleibt von allgemeinen Solverbackends unabhängig. Ausdruckssynthese übernimmt einen Kandidaten erst nach einem Äquivalenzbeweis; Gegenbeispiele und offene Abfragen lassen den Originalausdruck unverändert.

Die eingebaute Multiplikationsschaltung entfernt bekannte niederwertige Nullbits beider Faktoren, bildet das verbleibende Produkt und stellt danach das vollständige Ausgabewort wieder her. Der optionale Z3-Adapter codiert Produkte mit geradem konstantem Koeffizienten ebenfalls in der exakt beobachtbaren Breite. Beide erhalten Gleichheitsabfragen über die volle Breite, unabhängige Eingabemodelle und bestehende Ressourcenlimits. CLI und GUI verwenden bei der Quelltextoptimierung dieselbe eingebaute Schaltung ohne zusätzlichen Schalter.

<!-- i18n-section: builtin-comparisons -->

## Eingebaute Vergleichsschaltungen

Vergleiche über mehr als acht Bits vergleichen zuerst die oberen Hälften und verwenden bei deren Gleichheit die unteren Hälften. Kleine Abschnitte nutzen Subtraktionsüberträge. Der gemeinsame Encoder kann Gatter des oberen Präfixes wiederverwenden, wenn eine Teilregisteränderung nur untere Bits verändert. Vorzeichenbehaftete Vergleiche invertieren weiterhin beide Vorzeichenbits. Ausdruckssemantik und Ressourcengrenzen bleiben unverändert; ein ausgeschöpftes Budget liefert weiterhin `Unknown`.

`BitBlaster.WidePredicatesAgreeWithTheEvaluator` prüft vorzeichenbehaftete und vorzeichenlose Prädikate gegen den Ausdrucksauswerter bei ausgewählten Breiten von 8 bis 256 Bits, einschließlich ungerader Breiten, benachbarter Grenzwerte und signifikanter Bits oberhalb von 64 Bits. Falsche Ausgabewerte werden ebenfalls ausgeschlossen. Tests für Teilzähler prüfen vollständige Abfragen, Gegenbeispielmodelle und erschöpfte Gatterbudgets. Native Regressionen mit gespeichertem Vergleich beweisen beide Reihenfolgen von Vergleich und Aktualisierung unter Beobachtung aller Register und Flags und weisen einen geänderten ursprünglichen Schleifenrumpf zurück.

<!-- i18n-section: pristine-encoding -->

## Kopien der Kodierung vor der Suche

`BitVectorSolver::cloneEncoding()` kopiert eine vollständige Kodierung vor jedem SAT-Suchversuch. Nach einer Suche oder einem Kodierungsfehler liefert die Methode null. Kopien besitzen ihre veränderlichen Klauseln, Wurzelpropagation, Gatter und Bitzuordnungen selbst; Variablenreihenfolge, Gatterzählung und Solver-Einstellungen bleiben erhalten. Der Kontext muss beide Solver überleben; der Quellsolver darf unabhängig geändert oder zerstört werden.

Die SAT-Engine speichert vier Watch-Einträge direkt in jeder Literalliste; längere Listen wachsen dynamisch. Das vermeidet separate Allokationen kurzer Listen beim Aufbau, Kopieren vor der Suche und Freigeben. Propagationsreihenfolge, Klauseln, unabhängiger Besitz und alle Arbeitsgrenzen bleiben unverändert.

<!-- i18n-section: context-finite-proofs -->

## Abgeschlossene Beweise in einem Kontext

Der native Unabhängigkeitsprüfer bindet seinen Cache endlicher Wertebereiche an den tatsächlichen symbolischen Kontext, dem nur Knoten hinzugefügt werden. Kompakte Schlüssel enthalten das genaue Prädikat, geordnete Projektionen und die Werteobergrenze. Nur vollständige numerische Bereiche oder bewiesene Mehrdeutigkeit sind wiederverwendbar; ein Fehltreffer erfordert weiterhin die vollständige Aufzählung und den abschließenden Ausschlussbeweis. Kontext und Bedeutung vorhandener Knoten müssen während der Cache-Lebensdauer stabil bleiben. Vorbereitete Token dürfen weder Besitzer wechseln noch von einem Ersatz am selben Speicherort verwendet werden. Die bestehende Speicherobergrenze in Wörtern bleibt erhalten. Andere Nutzer behalten strukturelle Schlüssel und die Wiederverwendung durch Variablenumbenennung.

<!-- i18n-section: z3-build -->

## Optionaler Z3-Build

Mit aktiviertem Z3 lädt CMake über `FetchContent` die fixierte Quellrevision 4.13.3 und baut eine statische Bibliothek. Eine Systeminstallation ist nicht nötig:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
python scripts/build.py build-release --parallel 4 --target neverd \
  NeverDSolverTests NeverDSymbolicTests neverd-solver-bench
```

`NEVERD_Z3_PROVIDER=FETCH` ist Standard. Der erste Configure-Schritt lädt den Quelltext; spätere Builds verwenden `_deps` erneut. Z3-CLI, Tests, Beispiele, Dokumentation und Sprachbindungen werden nicht gebaut. Z3-Python-Generatoren nutzen NeverDs vorhandenen Python-Interpreter. Für eine installierte Bibliothek `-DNEVERD_Z3_PROVIDER=SYSTEM` und optional `-DZ3_ROOT=/path/to/prefix` wählen. Fehlende Entwicklungsdateien führen zum Fehler, ohne stillen Providerwechsel. Offline-Builds können `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=/path/to/z3` auf einen lokalen Checkout setzen.

Bei `NEVERD_ENABLE_Z3=OFF` (Standard) lädt, sucht oder verlinkt NeverD Z3 nicht. Eine explizite Laufzeitanforderung für einen nicht verfügbaren Backend schlägt ohne Fallback fehl.

<!-- i18n-section: synthesis -->

## Beweisgesteuerte Ausdruckssynthese

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

`--solver=builtin` ist Standard. Solverauswahl erfordert `--synthesize`; der normale MBA-Vereinfacher ruft Z3 nicht auf. Das Z3-Timeout gilt pro Prüfung und zählt Ausdrucksübersetzung nicht mit. Abbruch ist kooperativ, keine harte Echtzeitfrist. Null wählt 1000 ms; `--exhaustive` entfernt das Limit. Konflikt-, Propagations- und Watch-Besuchslimits gelten nur für das eingebaute Backend und werden mit Z3 abgelehnt. Z3-Prüfungen erhöhen den Proof-Query-Zähler; eingebaute SAT-Arbeitszähler bleiben null.

Die C-API ergänzt `neverd_synthesize_options` um `solver_backend` und `solver_timeout_ms`. Größenbeschränkte Leser behalten für ältere Aufrufer das eingebaute Backend. `neverd_solver_backend_available()` meldet die Build-Fähigkeit; Python bietet dieselbe Wahl über `synthesize_expression(..., solver='z3', solver_timeout_ms=1000)`. Die Auswahl gilt derzeit nur für Ausdruckssynthese. Concolic-Ausführung, Sicherheitsanalyse und bestehende IR-Optimierungsdefaults behalten ihre Solverrichtlinien. Interne Nutzer können den Z3-Verifier über den Proof-Callback des semantischen Vereinfachers einspeisen.

```python
from neverd_plugin import synthesize_expression

result = synthesize_expression(
    '(x >> 4) + ((x >> 2) >> 2)', solver='z3', solver_timeout_ms=1000
)
```

<!-- i18n-section: checks -->

## Unabhängige Prüfungen und Query-Export

`NeverDSolverTests` enthält bei aktiviertem Z3 eine unabhängige Ausdrucksauswertung und Backendvergleiche. Referenzausdrücke werden direkt erstellt, damit ein Fehler in NeverDs Ausdrucks-Buildern nicht beide Testseiten gleich vereinfacht. Bei deaktiviertem Build werden Oracle-Fälle ausdrücklich übersprungen; der Vertrag für nicht verfügbare Backends wird weiter getestet.

Tests skalierter Produkte vergleichen die eingebaute Schaltung mit unabhängiger schriftlicher Multiplikation und Ganzzahlarithmetik bis 257 Bit. Z3-Tests beweisen die Übereinstimmung mit einer unabhängigen Codierung voller Breite und prüfen hohe Eingabebits sowie ein absichtlich falsches höchstwertiges Ausgabebit.

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench \
  tools/neverd-bench/solver-corpus.txt --width=32 --repeat=5 \
  --backend=both --timeout-ms=1000 --max-conflicts=10000 \
  --dump-dir=/tmp/neverd-queries > /tmp/neverd-solver-results.json
```

Jede Nicht-Kommentarzeile lautet `original ; candidate`; ohne Semikolon wird das Ergebnis des MBA-Vereinfachers als Kandidat verwendet. Das Tool meldet Urteile, Modell-Replay und Zeiten für Sessionaufbau, Übersetzung und Lösung, nicht aber Parsing, MBA-Vereinfachung, Export oder Abbau. Jede Wiederholung nutzt einen frischen Solver. SAT-Modelle müssen die Differenz im Ausdrucksevaluator reproduzieren. Widersprüchliche eindeutige Urteile sowie ungültige Abfragen/Modelle lassen den Lauf scheitern; `unknown` wird protokolliert und ist kein Äquivalenzbeweis.

SMT-LIB-Export enthält den gleichwertig codierten DAG, dauerhafte Assertions und Annahmen der letzten Abfrage; Replay mit `z3 query-N.smt2`. Ressourcenlimits und Solversionen mitprotokollieren. Die Backends verwenden unterschiedliche Budgeteinheiten: Verglichen werden begrenzte Workloads, nicht identische Arbeit. Bitvektorbeweise folgen der totalen Fixed-Width-Semantik der Ausdruckssprache. Maschinenfehler, Speichereffekte und LLVM poison bleiben Verantwortung der Lift-/Übersetzungsgrenzen; ein Ausdrucksbeweis zertifiziert sie nicht.

Eine Kopie der noch nicht durchsuchten Kodierung entfernt auf Wurzelebene belegte Variablen aus ihrer eigenen Entscheidungswarteschlange und stellt die strikte Heap-Reihenfolge nach Aktivität und Index wieder her. Belegungen und Klauseln bleiben erhalten; Wurzelfakten überstehen jedes Backtracking. Besitz der Quelle, tatsächliche Entscheidungen, vollständige Modelle und alle Suchbudgets bleiben unverändert.

Der in SMT-LIB exportierte DAG ist eine semantisch gleichwertige Codierung und kann den oben beschriebenen exakten Quotienten enthalten; seine Struktur muss nicht der des ursprünglichen DAG entsprechen.
